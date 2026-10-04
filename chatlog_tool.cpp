// chatlog_tool.cpp - command line tool to talk to the /dev/chatlog driver
//
// Usage: chatlog_tool [-d /dev/chatlog] <command> [text]
//   dump        print everything currently stored, then exit
//   follow      print history, then keep waiting for new lines (like tail -f)
//   write TEXT  append a line
//   stats       show buffer statistics (ioctl)
//   clear       empty the log (ioctl)

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "chatlog_ioctl.h"

static void usage(const char* p) {
    std::cerr << "Usage: " << p << " [-d device] dump|follow|stats|clear|write <text>\n";
}

int main(int argc, char* argv[]) {
    std::string dev = "/dev/chatlog";
    int i = 1;
    if (i + 1 < argc && std::strcmp(argv[i], "-d") == 0) { dev = argv[i + 1]; i += 2; }
    if (i >= argc) { usage(argv[0]); return 1; }
    std::string cmd = argv[i++];

    int flags = (cmd == "write") ? O_WRONLY : (cmd == "dump" ? O_RDONLY | O_NONBLOCK : O_RDWR);
    int fd = ::open(dev.c_str(), flags);
    if (fd < 0) {
        std::perror(("open " + dev).c_str());
        std::cerr << "Is the module loaded?  (sudo scripts/load.sh)\n";
        return 1;
    }

    if (cmd == "stats") {
        chatlog_stats st{};
        if (ioctl(fd, CHATLOG_IOC_STATS, &st) < 0) { perror("ioctl"); return 1; }
        std::cout << "capacity     : " << st.capacity << " bytes\n"
                  << "stored       : " << st.stored_bytes << " bytes\n"
                  << "total written: " << st.total_bytes << " bytes\n"
                  << "messages     : " << st.messages << "\n";
    } else if (cmd == "clear") {
        if (ioctl(fd, CHATLOG_IOC_CLEAR) < 0) { perror("ioctl"); return 1; }
        std::cout << "log cleared\n";
    } else if (cmd == "write") {
        if (i >= argc) { usage(argv[0]); return 1; }
        std::string line = std::string(argv[i]) + "\n";
        if (::write(fd, line.data(), line.size()) < 0) { perror("write"); return 1; }
    } else if (cmd == "dump" || cmd == "follow") {
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof(buf));   // follow: blocks in the driver
            if (n > 0) {
                if (::write(STDOUT_FILENO, buf, static_cast<size_t>(n)) < 0) break;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;                                       // dump: EAGAIN = end of history
        }
    } else {
        usage(argv[0]);
        return 1;
    }
    ::close(fd);
    return 0;
}
