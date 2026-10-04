#pragma once

#include <cerrno>
#include <chrono>
#include <ctime>
#include <string>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

constexpr const char* DEFAULT_PORT = "8080";
constexpr std::size_t MAX_LINE = 1024;  
constexpr std::size_t MAX_NAME = 16;    

inline bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    ssize_t fill() {
        char tmp[2048];
        for (;;) {
            ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n > 0) buf_.append(tmp, static_cast<std::size_t>(n));
            return n;
        }
    }

    bool pop_line(std::string& line) {
        auto pos = buf_.find('\n');
        if (pos == std::string::npos) {
            if (buf_.size() < MAX_LINE) return false;
            line = buf_.substr(0, MAX_LINE);  
            buf_.clear();
            return true;
        }
        line = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

    bool read_line(std::string& line) {
        for (;;) {
            if (pop_line(line)) return true;
            if (fill() <= 0) return false;
        }
    }

private:
    int fd_;
    std::string buf_;
};

inline std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char out[16];
    std::strftime(out, sizeof(out), "%H:%M:%S", &tm);
    return out;
}
