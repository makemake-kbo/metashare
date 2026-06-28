#include "signaling_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <string>

namespace metashare::signal {

namespace {
bool read_line(int fd, std::string& line) {
    line.clear();
    char c = 0;
    while (true) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        line.push_back(c);
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

Client::~Client() { disconnect(); }

bool Client::connect(const std::string& host, std::uint16_t port,
                     OnMessage on_message, std::string& err) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        err = std::strerror(errno);
        return false;
    }

    in_addr addr{};
    if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) {
        err = "invalid host (use IPv4 literal for now): " + host;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr = addr;
    sin.sin_port = htons(port);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        err = "connect: " + std::string(std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Wait for the initial "OK" line.
    std::string line;
    if (!read_line(fd_, line) || line != "OK") {
        err = "expected OK, got: " + line;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    running_ = true;
    read_thread_ = std::thread([this, on_message = std::move(on_message)] {
        std::string l;
        while (running_) {
            if (!read_line(fd_, l)) break;
            Message m;
            if (!parse(l, m)) continue;
            if (on_message) on_message(m);
            if (m.type == Type::kBye) break;
        }
        running_ = false;
    });
    return true;
}

void Client::disconnect() {
    if (!running_.exchange(false)) return;
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    if (read_thread_.joinable()) read_thread_.join();
}

bool Client::send(const Message& m) {
    std::lock_guard<std::mutex> lk(send_mu_);
    if (fd_ < 0) return false;
    std::string line = serialize(m) + '\n';
    return send_all(fd_, line.data(), line.size());
}

}  // namespace metashare::signal
