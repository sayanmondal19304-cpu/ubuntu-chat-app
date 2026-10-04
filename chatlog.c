// SPDX-License-Identifier: GPL-2.0
/*
 * chatlog - a character device driver that keeps the chat history in a
 *           kernel ring buffer.
 *
 *   /dev/chatlog
 *     write()  : append a chat line (oldest data is overwritten when full)
 *     read()   : read history; blocks until new data (like `tail -f`)
 *     poll()   : select/poll/epoll support
 *     ioctl()  : CHATLOG_IOC_CLEAR, CHATLOG_IOC_STATS
 *
 * Every open() gets its own read position, so many programs can read the
 * log at the same time without stealing data from each other.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/version.h>
#include <asm/div64.h>

#include "chatlog_ioctl.h"

#define DEVICE_NAME "chatlog"

static unsigned int buf_size = 65536;
module_param(buf_size, uint, 0444);
MODULE_PARM_DESC(buf_size, "Ring buffer size in bytes (4096..1048576, default 65536)");

/*
 * The buffer is a ring, but positions are kept as ever-growing 64-bit
 * "logical" offsets. Logical offset p lives at buf[p % size]. This makes it
 * trivial for each reader to detect that it was lapped (pos < oldest).
 */
struct chatlog_dev {
	struct cdev cdev;
	struct class *class;
	struct device *device;
	dev_t devno;

	struct mutex lock;           /* protects everything below */
	wait_queue_head_t wq;        /* readers sleep here        */
	char *buf;
	u32 size;
	u64 total;                   /* logical end of the stream */
	u64 start;                   /* set by CLEAR              */
	u64 messages;
};

struct chatlog_reader {
	u64 pos;                     /* this open file's logical read offset */
};

static struct chatlog_dev chatlog;

static inline u32 ring_off(u64 pos, u32 size)
{
	return do_div(pos, size);    /* do_div: safe 64-bit modulo on 32-bit CPUs */
}

/* Oldest logical offset that is still valid. Call with lock held. */
static u64 oldest_pos(struct chatlog_dev *d)
{
	u64 lapped = d->total > d->size ? d->total - d->size : 0;

	return max(lapped, d->start);
}

static int chatlog_open(struct inode *inode, struct file *filp)
{
	struct chatlog_dev *d = container_of(inode->i_cdev, struct chatlog_dev, cdev);
	struct chatlog_reader *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	mutex_lock(&d->lock);
	r->pos = oldest_pos(d);      /* new readers see the full history */
	mutex_unlock(&d->lock);

	filp->private_data = r;
	return nonseekable_open(inode, filp);
}

static int chatlog_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static ssize_t chatlog_read(struct file *filp, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct chatlog_dev *d = &chatlog;
	struct chatlog_reader *r = filp->private_data;
	size_t done = 0;
	u64 avail;
	int ret;

	if (!count)
		return 0;

	for (;;) {
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;

		if (r->pos < oldest_pos(d))      /* we were lapped: skip lost data */
			r->pos = oldest_pos(d);
		if (r->pos < d->total)
			break;                   /* data available, keep the lock */

		mutex_unlock(&d->lock);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(d->wq, r->pos < READ_ONCE(d->total));
		if (ret)
			return ret;              /* -ERESTARTSYS on signal */
	}

	avail = d->total - r->pos;
	count = min_t(u64, count, avail);

	while (done < count) {
		u32 off = ring_off(r->pos, d->size);
		size_t chunk = min_t(size_t, count - done, d->size - off);

		if (copy_to_user(ubuf + done, d->buf + off, chunk)) {
			if (!done)
				done = -EFAULT;
			break;
		}
		r->pos += chunk;
		done += chunk;
	}
	mutex_unlock(&d->lock);
	return done;
}

static ssize_t chatlog_write(struct file *filp, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct chatlog_dev *d = &chatlog;
	size_t n = min_t(size_t, count, CHATLOG_MAX_MSG);
	size_t done = 0;

	if (!n)
		return 0;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;

	while (done < n) {
		u32 off = ring_off(d->total, d->size);
		size_t chunk = min_t(size_t, n - done, d->size - off);

		if (copy_from_user(d->buf + off, ubuf + done, chunk)) {
			if (!done) {
				mutex_unlock(&d->lock);
				return -EFAULT;
			}
			break;
		}
		d->total += chunk;
		done += chunk;
	}
	d->messages++;
	mutex_unlock(&d->lock);

	wake_up_interruptible(&d->wq);   /* wake sleeping readers / pollers */
	return done;
}

static __poll_t chatlog_poll(struct file *filp, poll_table *wait)
{
	struct chatlog_dev *d = &chatlog;
	struct chatlog_reader *r = filp->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;      /* always writable */

	poll_wait(filp, &d->wq, wait);

	mutex_lock(&d->lock);
	if (r->pos < oldest_pos(d))
		r->pos = oldest_pos(d);
	if (r->pos < d->total)
		mask |= EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&d->lock);
	return mask;
}

static long chatlog_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct chatlog_dev *d = &chatlog;

	switch (cmd) {
	case CHATLOG_IOC_CLEAR:
		mutex_lock(&d->lock);
		d->start = d->total;     /* everything before this is "gone" */
		d->messages = 0;
		mutex_unlock(&d->lock);
		return 0;

	case CHATLOG_IOC_STATS: {
		struct chatlog_stats st = {};

		mutex_lock(&d->lock);
		st.total_bytes  = d->total;
		st.stored_bytes = d->total - oldest_pos(d);
		st.messages     = d->messages;
		st.capacity     = d->size;
		mutex_unlock(&d->lock);

		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;          /* unknown ioctl */
	}
}

static const struct file_operations chatlog_fops = {
	.owner          = THIS_MODULE,
	.open           = chatlog_open,
	.release        = chatlog_release,
	.read           = chatlog_read,
	.write          = chatlog_write,
	.poll           = chatlog_poll,
	.unlocked_ioctl = chatlog_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

static int __init chatlog_init(void)
{
	struct chatlog_dev *d = &chatlog;
	int ret;

	if (buf_size < 4096 || buf_size > (1u << 20)) {
		pr_err("chatlog: buf_size must be 4096..1048576\n");
		return -EINVAL;
	}

	d->size = buf_size;
	d->buf = kvzalloc(d->size, GFP_KERNEL);
	if (!d->buf)
		return -ENOMEM;
	mutex_init(&d->lock);
	init_waitqueue_head(&d->wq);

	/* 1. ask the kernel for a free major/minor number */
	ret = alloc_chrdev_region(&d->devno, 0, 1, DEVICE_NAME);
	if (ret)
		goto err_buf;

	/* 2. register the file operations for that number */
	cdev_init(&d->cdev, &chatlog_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devno, 1);
	if (ret)
		goto err_region;

	/* 3. create /sys/class/chatlog so udev makes /dev/chatlog */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	d->class = class_create(DEVICE_NAME);
#else
	d->class = class_create(THIS_MODULE, DEVICE_NAME);
#endif
	if (IS_ERR(d->class)) {
		ret = PTR_ERR(d->class);
		goto err_cdev;
	}
	d->device = device_create(d->class, NULL, d->devno, NULL, DEVICE_NAME);
	if (IS_ERR(d->device)) {
		ret = PTR_ERR(d->device);
		goto err_class;
	}

	pr_info("chatlog: loaded, major=%d minor=%d, buffer=%u bytes\n",
		MAJOR(d->devno), MINOR(d->devno), d->size);
	return 0;

err_class:
	class_destroy(d->class);
err_cdev:
	cdev_del(&d->cdev);
err_region:
	unregister_chrdev_region(d->devno, 1);
err_buf:
	kvfree(d->buf);
	return ret;
}

static void __exit chatlog_exit(void)
{
	struct chatlog_dev *d = &chatlog;

	device_destroy(d->class, d->devno);
	class_destroy(d->class);
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->devno, 1);
	kvfree(d->buf);
	pr_info("chatlog: unloaded\n");
}

module_init(chatlog_init);
module_exit(chatlog_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sayan Mondal");
MODULE_DESCRIPTION("Ring-buffer character device for chat history");
MODULE_VERSION("1.0");
