// client.cpp - chat client using poll() to watch keyboard AND socket
//
// Usage: ./bin/client [host] [port]     (defaults: 127.0.0.1 8080)

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>

#include "common.hpp"

static int connect_to(const char* host, const char* port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(rc) << "\n";
        return -1;
    }
    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char* argv[]) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const char* port = argc > 2 ? argv[2] : DEFAULT_PORT;

    int sock = connect_to(host, port);
    if (sock < 0) {
        std::cerr << "Could not connect to " << host << ":" << port << "\n";
        return 1;
    }
    std::cout << "Connected to " << host << ":" << port << "\n";

    LineReader reader(sock);
    pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {sock, POLLIN, 0}};

    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        // Data from the server
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (reader.fill() <= 0) {
                std::cout << "Disconnected from server.\n";
                break;
            }
            std::string line;
            while (reader.pop_line(line)) std::cout << line << std::endl;
        }

        // Keyboard input
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            std::string input;
            if (!std::getline(std::cin, input)) {  // Ctrl+D
                send_all(sock, "/quit\n");
                break;
            }
            if (!send_all(sock, input + "\n")) {
                std::cout << "Send failed.\n";
                break;
            }
            if (input == "/quit") {
                // give the server's goodbye a moment to arrive, then exit
                if (reader.fill() > 0) {
                    std::string line;
                    while (reader.pop_line(line)) std::cout << line << std::endl;
                }
                break;
            }
        }
    }
    ::close(sock);
    return 0;
}
