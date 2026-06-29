#include "signaling_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <string>

namespace metashare::signal {

namespace {

// Read one newline-terminated line from a socket. Returns false on EOF/error.
bool read_line(int fd, std::string& line) {
    line.clear();
    char c = 0;
    while (true) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        line.push_back(c);
        // Cap pathological lines so a malicious peer can't OOM us.
        if (line.size() > 65536) return false;
    }
}

bool send_all(int fd, const void* data, std::size_t len) {
    auto* p = static_cast<const char*>(data);
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

Server::~Server() { stop(); }

bool Server::start(std::uint16_t port, OnConnect on_connect,
                   OnMessage on_message, std::string& err) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        err = std::strerror(errno);
        return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
        0) {
        err = "bind: " + std::string(std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 1) < 0) {
        err = "listen: " + std::string(std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread([this, port,
                                  on_connect = std::move(on_connect),
                                  on_message = std::move(on_message)] {
        while (running_) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int fd =
                ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (fd < 0) {
                if (!running_) break;
                continue;
            }
            char ip[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            std::fprintf(stderr, "[signaling] client connected from %s\n", ip);
            client_loop(fd, peer, on_connect, on_message);
            std::fprintf(stderr, "[signaling] client disconnected\n");
        }
    });
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    int fd = client_fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

bool Server::has_client() const { return client_fd_.load() >= 0; }

bool Server::send(const Message& m) {
    std::lock_guard<std::mutex> lk(send_mu_);
    int fd = client_fd_.load();
    if (fd < 0) return false;
    std::string line = serialize(m) + '\n';
    return send_all(fd, line.data(), line.size());
}

void Server::client_loop(int fd, const sockaddr_in& peer,
                         const OnConnect& on_connect,
                         const OnMessage& on_message) {
    client_fd_ = fd;

    // Notify the owner that a client is connected so it can send the initial
    // HELLO. The new raw-RTP protocol has no "OK" marker — HELLO is first.
    if (on_connect) on_connect(peer);

    std::string line;
    while (running_) {
        if (!read_line(fd, line)) break;
        Message msg;
        if (!parse(line, msg)) {
            std::fprintf(stderr, "[signaling] malformed line: %zu bytes\n",
                         line.size());
            continue;
        }
        if (on_message) on_message(msg);
        if (msg.type == Type::kBye) break;
    }

    int prev = client_fd_.exchange(-1);
    if (prev == fd) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

}  // namespace metashare::signal
