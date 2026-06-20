#include "net_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace metashare {

NetServer::~NetServer() { stop(); }

bool NetServer::start(std::uint16_t port, std::string& err) {
    port_ = port;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        err = std::string("socket: ") + std::strerror(errno);
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
        err = std::string("bind: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 8) < 0) {
        err = std::string("listen: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void NetServer::stop() {
    if (!running_.exchange(false)) {
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        return;
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    std::lock_guard<std::mutex> lk(clients_mu_);
    for (auto& c : clients_)
        if (c.fd >= 0) ::close(c.fd);
    clients_.clear();
}

void NetServer::accept_loop() {
    while (running_) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd < 0) {
            if (!running_) break;
            if (errno == EINTR) continue;
            break;
        }

        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        timeval tv{2, 0};  // drop a client that can't drain within 2s
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::fprintf(stderr, "[net] client connected: %s\n", ip);

        if (!send_all(fd, &header_, sizeof(header_))) {
            ::close(fd);
            continue;
        }
        std::lock_guard<std::mutex> lk(clients_mu_);
        clients_.push_back(Client{fd, false});
    }
}

bool NetServer::send_all(int fd, const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
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

void NetServer::broadcast(const proto::FrameHeader& fh,
                          const std::uint8_t* payload) {
    const bool key = (fh.flags & proto::kFrameKeyframe) != 0;
    std::lock_guard<std::mutex> lk(clients_mu_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        Client& c = *it;
        if (!c.ready) {
            if (!key) { ++it; continue; }  // wait for a clean entry point
            c.ready = true;
        }
        bool ok = send_all(c.fd, &fh, sizeof(fh)) &&
                  send_all(c.fd, payload, fh.payload_size);
        if (!ok) {
            std::fprintf(stderr, "[net] client dropped\n");
            ::close(c.fd);
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

int NetServer::client_count() const {
    std::lock_guard<std::mutex> lk(clients_mu_);
    return static_cast<int>(clients_.size());
}

}  // namespace metashare
