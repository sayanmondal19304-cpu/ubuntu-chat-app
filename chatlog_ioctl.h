#ifndef CHATLOG_IOCTL_H
#define CHATLOG_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CHATLOG_MAX_MSG 4096          /* max bytes accepted per write() */

struct chatlog_stats {
	__u64 total_bytes;   
	__u64 stored_bytes;   
	__u64 messages;       
	__u32 capacity;      
	__u32 _pad;
};

#define CHATLOG_IOC_MAGIC  'C'
#define CHATLOG_IOC_CLEAR  _IO(CHATLOG_IOC_MAGIC, 1)
#define CHATLOG_IOC_STATS  _IOR(CHATLOG_IOC_MAGIC, 2, struct chatlog_stats)

#endif 
