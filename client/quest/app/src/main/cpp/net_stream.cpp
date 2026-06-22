#include "net_stream.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace metashare {

namespace {
bool read_all(int fd, void* buf, std::size_t len) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::recv(fd, p + off, len - off, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}
}  // namespace

NetStream::~NetStream() { stop(); }

void NetStream::set_host(const std::string& host, std::uint16_t port) {
    host_ = host;
    port_ = port;
}

bool NetStream::discover(int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    proto::DiscoveryProbe probe{};
    std::memcpy(probe.magic, proto::kDiscoveryMagic, sizeof(probe.magic));
    probe.version = proto::kProtocolVersion;
    // The Quest 3 has hardware decoders for both H.264 and HEVC; advertise both
    // so the streamer can pick whichever its hardware encode path supports.
    probe.client_caps = proto::kCapH264 | proto::kCapH265;

    auto send_to = [&](in_addr_t ip) {
        sockaddr_in d{};
        d.sin_family = AF_INET;
        d.sin_addr.s_addr = ip;
        d.sin_port = htons(proto::kDiscoveryPort);
        ::sendto(fd, &probe, sizeof(probe), 0, reinterpret_cast<sockaddr*>(&d),
                 sizeof(d));
    };
    send_to(htonl(INADDR_BROADCAST));
    send_to(htonl(INADDR_LOOPBACK));  // same-machine streamer + client
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) == 0) {
        for (ifaddrs* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_BROADCAST) ||
                (p->ifa_flags & IFF_LOOPBACK))
                continue;
            if (p->ifa_broadaddr)
                send_to(reinterpret_cast<sockaddr_in*>(p->ifa_broadaddr)
                            ->sin_addr.s_addr);
        }
        ::freeifaddrs(ifa);
    }

    proto::DiscoveryOffer offer{};
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    ssize_t n = ::recvfrom(fd, &offer, sizeof(offer), 0,
                           reinterpret_cast<sockaddr*>(&from), &flen);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(offer))) return false;
    if (std::memcmp(offer.magic, proto::kDiscoveryMagic, sizeof(offer.magic)) !=
        0)
        return false;

    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    host_ = ip;
    port_ = offer.stream_port;
    return true;
}

bool NetStream::connect_and_run(std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints,
                      &res) != 0) {
        err = "getaddrinfo failed";
        return false;
    }
    fd_ = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd_ = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd_ < 0) continue;
        if (::connect(fd_, a->ai_addr, a->ai_addrlen) == 0) break;
        ::close(fd_);
        fd_ = -1;
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) {
        err = "connect failed";
        return false;
    }
    int yes = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    if (!read_all(fd_, &header_, sizeof(header_)) ||
        std::memcmp(header_.magic, proto::kStreamMagic,
                    sizeof(header_.magic)) != 0) {
        err = "bad stream header";
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    running_ = true;
    thread_ = std::thread([this] { recv_loop(); });
    return true;
}

void NetStream::recv_loop() {
    while (running_) {
        proto::FrameHeader fh{};
        if (!read_all(fd_, &fh, sizeof(fh))) break;

        EncodedFrame f;
        f.data.resize(fh.payload_size);
        if (!read_all(fd_, f.data.data(), fh.payload_size)) break;
        f.pts_usec = fh.pts_usec;
        f.keyframe = (fh.flags & proto::kFrameKeyframe) != 0;

        std::lock_guard<std::mutex> lk(mu_);
        // If the consumer falls behind, drop the oldest non-key frames so we
        // stay close to live.
        while (queue_.size() >= kMaxQueued && !queue_.front().keyframe)
            queue_.pop_front();
        if (queue_.size() >= kMaxQueued) queue_.pop_front();
        queue_.push_back(std::move(f));
        cv_.notify_one();
    }
    running_ = false;
    cv_.notify_all();
}

bool NetStream::pop_frame(EncodedFrame& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty() || !running_; }))
        return false;
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void NetStream::stop() {
    running_ = false;
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

}  // namespace metashare
