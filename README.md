# Ubuntu Chat Application using c++ and linux socket

A multi-client, terminal-based chat application written in modern C++17 using
raw POSIX/Linux TCP sockets. No external libraries.

## Author
Sayan Mondal

## Features
- Multi-client TCP server (one `std::thread` per client)
- Unique usernames with validation
- Public chat with timestamps
- Private messages (`/msg user text`)
- `/list`, `/help`, `/quit` commands
- Join / leave notifications
- Graceful shutdown on Ctrl+C
- Correct handling of partial reads/writes (TCP is a byte stream)
- Client uses `poll()` to watch keyboard and socket together

## Project structure
```
ubuntu-chat-cpp/
├── include/common.hpp       # send_all(), LineReader, timestamp()
├── src/server.cpp           # chat server
├── src/client.cpp           # chat client
├── driver/
│   ├── chatlog.c            # Linux kernel character device driver
│   ├── chatlog_ioctl.h      # ioctl interface shared with user space
│   └── Makefile             # kbuild makefile for the module
├── tools/chatlog_tool.cpp   # CLI for /dev/chatlog (dump/follow/stats/clear/write)
├── scripts/load.sh          # build + insmod + set permissions
├── scripts/unload.sh        # rmmod
├── Makefile
├── .gitignore
├── LICENSE
└── README.md
```

## Requirements
- Ubuntu / any Linux
- `g++` (C++17) and `make`

```bash
sudo apt update
sudo apt install -y build-essential git
```

## Build
```bash
make          # creates bin/server and bin/client
make clean    # removes bin/
```

## Run
Terminal 1 - server:
```bash
./bin/server 8080
```
Terminal 2, 3, ... - clients:
```bash
./bin/client 127.0.0.1 8080
```
To chat across machines, run the client with the server's LAN IP
(`hostname -I`) and allow the port: `sudo ufw allow 8080/tcp`.

## Commands
| Command | Description |
|---|---|
| `/help` | show commands |
| `/list` | list online users |
| `/msg <user> <text>` | private message |
| `/quit` | leave (Ctrl+D also works) |

## Protocol
Plain text, one message per line, terminated by `\n`. Maximum line length is
1024 bytes. You can even test with `nc 127.0.0.1 8080`.

## How it works
Server: `socket()` -> `setsockopt(SO_REUSEADDR)` -> `bind()` -> `listen()` ->
`accept()` loop; each client runs in `handle_client()` in its own thread. A
mutex protects the shared `username -> fd` map.

Client: `getaddrinfo()` -> `connect()` -> `poll()` on stdin and the socket.

## Kernel device driver: `/dev/chatlog`

`driver/chatlog.c` is a loadable kernel module (character device driver). It
keeps the most recent chat history in a kernel ring buffer (default 64 KB).
The chat server can write every public message to it, and any program can read
the history back through the normal file API.

| Syscall | Behaviour |
|---|---|
| `open()` | each open gets its own read position (starts at oldest data) |
| `write()` | appends a line; when full, oldest data is overwritten |
| `read()` | returns history; blocks for new data (`O_NONBLOCK` -> `EAGAIN`) |
| `poll()` | works with `select`/`poll`/`epoll` |
| `ioctl()` | `CHATLOG_IOC_STATS`, `CHATLOG_IOC_CLEAR` |

Kernel concepts shown: `alloc_chrdev_region`, `cdev_add`, `class_create` /
`device_create` (udev makes `/dev/chatlog`), `copy_to_user` / `copy_from_user`,
`mutex`, wait queues, module parameters, error unwinding with `goto`.

### Build and load
```bash
sudo apt install -y linux-headers-$(uname -r)
make driver                      # builds driver/chatlog.ko
sudo scripts/load.sh             # insmod + chmod; optional: buf_size=131072
dmesg | tail                     # "chatlog: loaded, major=..."
```
### Use it
```bash
make                                        # builds server, client, chatlog_tool
./bin/server 8080 --chatlog /dev/chatlog    # server logs chat into the kernel
./bin/chatlog_tool follow                   # live view of the log (tail -f style)
./bin/chatlog_tool stats                    # buffer statistics via ioctl
./bin/chatlog_tool clear
cat /dev/chatlog                            # plain cat also works
sudo scripts/unload.sh                      # remove the module
```
### Notes
- The driver is built against the kernel headers installed in the Ubuntu VM.
  Run `uname -r` to see the running kernel version and install the matching
  headers with `sudo apt install -y linux-headers-$(uname -r)`.
- The source includes `LINUX_VERSION_CODE` guards for the `class_create()`
  API change introduced in Linux 6.4.
- With Secure Boot enabled, unsigned modules are refused: disable Secure Boot
  for testing, or sign the module (`mokutil`, `kmodsign`).
- `load.sh` sets the node to mode 666 for convenience. For anything beyond
  learning, use a udev rule with a dedicated group instead.
- Test inside a virtual machine first. A kernel bug can crash the whole machine.

## Ideas for improvement
- Add a `/proc/chatlog` or sysfs interface to the driver
- Replace thread-per-client with `epoll` for thousands of clients
- Chat rooms / channels, message history
- TLS with OpenSSL
- Per-client send queues so a slow client can't block broadcasts
- Unit tests, CMake, GitHub Actions CI

## Author
Sayan Mondal

## License
MIT
