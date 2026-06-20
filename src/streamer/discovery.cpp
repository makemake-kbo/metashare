#include "discovery.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "protocol.hpp"

namespace metashare {

DiscoveryResponder::~DiscoveryResponder() { stop(); }

bool DiscoveryResponder::start(std::uint16_t stream_port, std::string& err) {
    stream_port_ = stream_port;
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        err = std::string("discovery socket: ") + std::strerror(errno);
        return false;
    }
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(proto::kDiscoveryPort);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("discovery bind: ") + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    running_ = true;
    thread_ = std::thread([this] { serve_loop(); });
    return true;
}

void DiscoveryResponder::stop() {
    if (!running_.exchange(false)) {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        return;
    }
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void DiscoveryResponder::serve_loop() {
    while (running_) {
        proto::DiscoveryProbe probe{};
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        ssize_t n = ::recvfrom(fd_, &probe, sizeof(probe), 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 0) {
            if (!running_) break;
            if (errno == EINTR) continue;
            break;
        }
        if (static_cast<std::size_t>(n) < sizeof(probe)) continue;
        if (std::memcmp(probe.magic, proto::kDiscoveryMagic,
                        sizeof(probe.magic)) != 0)
            continue;
        if (probe.version != proto::kProtocolVersion) continue;

        proto::DiscoveryOffer offer{};
        std::memcpy(offer.magic, proto::kDiscoveryMagic, sizeof(offer.magic));
        offer.version = proto::kProtocolVersion;
        offer.stream_port = stream_port_;
        offer.reserved = 0;
        if (::gethostname(offer.host_name, sizeof(offer.host_name)) != 0)
            std::snprintf(offer.host_name, sizeof(offer.host_name), "metashare");
        offer.host_name[sizeof(offer.host_name) - 1] = '\0';

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        std::fprintf(stderr, "[discovery] probe from %s -> offering port %u\n",
                     ip, static_cast<unsigned>(stream_port_));

        ::sendto(fd_, &offer, sizeof(offer), 0,
                 reinterpret_cast<sockaddr*>(&from), flen);
    }
}

}  // namespace metashare
