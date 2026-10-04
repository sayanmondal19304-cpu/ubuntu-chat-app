// server.cpp - multi-client TCP chat server (thread per client)
//
// Usage: ./bin/server [port] [--chatlog /dev/chatlog]
//
// Flow: socket() -> setsockopt() -> bind() -> listen() -> accept() loop.
// Every accepted connection gets its own std::thread.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "common.hpp"

static std::mutex g_mutex;                    // protects g_clients
static std::map<std::string, int> g_clients;  // username -> socket fd
static std::atomic<bool> g_running{true};
static std::atomic<int> g_active{0};          // live client threads
static int g_logfd = -1;                      // /dev/chatlog (optional)

// Append a line to the kernel driver's ring buffer, if it is enabled.
static void klog(const std::string& line) {
    if (g_logfd < 0) return;
    std::string out = line + "\n";
    if (::write(g_logfd, out.data(), out.size()) < 0) perror("chatlog write");
}

static void log(const std::string& msg) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::cout << "[" << timestamp() << "] " << msg << std::endl;
}

static void on_signal(int) { g_running = false; }  // accept() gets EINTR

// Send to everybody (optionally skipping one fd).
static void broadcast(const std::string& msg, int skip_fd = -1) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& [name, fd] : g_clients)
        if (fd != skip_fd) send_all(fd, msg + "\n");
}

static bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > MAX_NAME) return false;
    for (unsigned char c : n)
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

static std::string user_list() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string out = "Online (" + std::to_string(g_clients.size()) + "): ";
    bool first = true;
    for (auto& [name, fd] : g_clients) {
        if (!first) out += ", ";
        out += name;
        first = false;
    }
    return out;
}

static const char* HELP =
    "Commands:\n"
    "  /help              show this help\n"
    "  /list              list online users\n"
    "  /msg <user> <text> private message\n"
    "  /quit              leave the chat";

static void handle_client(int fd, std::string ip) {
    LineReader reader(fd);
    std::string line, name;

    // ---- 1. choose a username ----
    send_all(fd, "Welcome! Enter a username (letters, digits, _; max 16):\n");
    while (name.empty()) {
        if (!reader.read_line(line)) {
            ::close(fd);
            --g_active;
            return;
        }
        line = trim(line);
        if (!valid_name(line)) {
            send_all(fd, "Invalid username, try again:\n");
            continue;
        }
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_clients.count(line)) {
            send_all(fd, "Username already taken, try another:\n");
            continue;
        }
        g_clients[line] = fd;
        name = line;
    }

    log(name + " joined from " + ip);
    send_all(fd, "Hello " + name + "! Type /help for commands.\n");
    broadcast("* " + name + " joined the chat", fd);
    klog("[" + timestamp() + "] * " + name + " joined");

    // ---- 2. main message loop ----
    while (reader.read_line(line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (line == "/quit") {
            send_all(fd, "Goodbye!\n");
            break;
        } else if (line == "/help") {
            send_all(fd, std::string(HELP) + "\n");
        } else if (line == "/list") {
            send_all(fd, user_list() + "\n");
        } else if (line.rfind("/msg ", 0) == 0) {
            std::string rest = trim(line.substr(5));
            auto sp = rest.find(' ');
            if (sp == std::string::npos) {
                send_all(fd, "Usage: /msg <user> <text>\n");
                continue;
            }
            std::string target = rest.substr(0, sp);
            std::string text = trim(rest.substr(sp + 1));
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_clients.find(target);
            if (it == g_clients.end()) {
                send_all(fd, "No such user: " + target + "\n");
            } else {
                send_all(it->second, "[private from " + name + "] " + text + "\n");
                send_all(fd, "[private to " + target + "] " + text + "\n");
            }
        } else if (line[0] == '/') {
            send_all(fd, "Unknown command. Type /help\n");
        } else {
            log(name + ": " + line);
            std::string out = "[" + timestamp() + "] " + name + ": " + line;
            broadcast(out);
            klog(out);
        }
    }

    // ---- 3. cleanup ----
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_clients.erase(name);
    }
    ::close(fd);
    log(name + " left");
    broadcast("* " + name + " left the chat");
    klog("[" + timestamp() + "] * " + name + " left");
    --g_active;
}

int main(int argc, char* argv[]) {
    int port = std::atoi(DEFAULT_PORT);
    const char* logdev = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--chatlog") == 0 && i + 1 < argc) logdev = argv[++i];
        else port = std::atoi(argv[i]);
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "Usage: " << argv[0] << " [port] [--chatlog /dev/chatlog]\n";
        return 1;
    }
    if (logdev) {
        g_logfd = ::open(logdev, O_WRONLY);
        if (g_logfd < 0) perror("open chatlog (continuing without kernel log)");
        else std::cout << "Logging chat to kernel device " << logdev << std::endl;
    }

    // Ctrl+C -> graceful shutdown. No SA_RESTART so accept() is interrupted.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int yes = 1;  // allow quick restart of the server on the same port
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // all interfaces
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (::listen(listen_fd, SOMAXCONN) < 0) { perror("listen"); return 1; }

    log("Server listening on port " + std::to_string(port) + " (Ctrl+C to stop)");

    while (g_running) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&cli), &len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        std::string who = std::string(ip) + ":" + std::to_string(ntohs(cli.sin_port));

        ++g_active;
        std::thread(handle_client, fd, who).detach();
    }

    // ---- graceful shutdown ----
    log("Shutting down...");
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto& [name, fd] : g_clients) {
            send_all(fd, "* Server is shutting down\n");
            ::shutdown(fd, SHUT_RDWR);  // wakes each thread's recv()
        }
    }
    for (int i = 0; i < 40 && g_active > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ::close(listen_fd);
    if (g_logfd >= 0) ::close(g_logfd);
    return 0;
}
