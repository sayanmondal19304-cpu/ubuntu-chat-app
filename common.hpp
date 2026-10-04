// common.hpp - helpers shared by the chat server and client
#pragma once

#include <cerrno>
#include <chrono>
#include <ctime>
#include <string>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

constexpr const char* DEFAULT_PORT = "8080";
constexpr std::size_t MAX_LINE = 1024;  // max bytes in one chat line
constexpr std::size_t MAX_NAME = 16;    // max username length

// Send the whole string, handling partial writes and EINTR.
// MSG_NOSIGNAL stops the process dying with SIGPIPE if the peer vanished.
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

// TCP is a byte stream, not a message stream: one recv() may contain half a
// line or three lines. LineReader buffers bytes and hands out complete lines.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    // Do one recv(). Returns bytes read, 0 if peer closed, -1 on error.
    ssize_t fill() {
        char tmp[2048];
        for (;;) {
            ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n > 0) buf_.append(tmp, static_cast<std::size_t>(n));
            return n;
        }
    }

    // Extract one complete line from the buffer (without '\n' / '\r').
    bool pop_line(std::string& line) {
        auto pos = buf_.find('\n');
        if (pos == std::string::npos) {
            if (buf_.size() < MAX_LINE) return false;
            line = buf_.substr(0, MAX_LINE);  // overly long line: cut it
            buf_.clear();
            return true;
        }
        line = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

    // Blocking: wait until a full line is available. false = disconnected.
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

// "14:03:22" style timestamp
inline std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char out[16];
    std::strftime(out, sizeof(out), "%H:%M:%S", &tm);
    return out;
}
