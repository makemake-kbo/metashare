#include "rtp_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

namespace metashare {

namespace {

// Monotonic source of globally-unique SSRCs across all RtpServer instances.
std::uint32_t next_ssrc() {
    static std::atomic<std::uint32_t> counter{0x12340000};
    return counter.fetch_add(0x100, std::memory_order_relaxed);
}

bool send_all_tcp(int fd, const void* data, std::size_t len) {
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

// Parse a tiny JSON value: {"port": <number>} or {"port":<number>}. Returns
// false on any deviation — we control both ends, so strictness is fine.
bool parse_ready_port(const std::string& body, std::uint16_t& port_out) {
    auto key = body.find("\"port\"");
    if (key == std::string::npos) return false;
    auto colon = body.find(':', key);
    if (colon == std::string::npos) return false;
    std::size_t i = colon + 1;
    while (i < body.size() &&
           (body[i] == ' ' || body[i] == '\t' || body[i] == '\n'))
        ++i;
    if (i >= body.size() || body[i] < '0' || body[i] > '9') return false;
    unsigned long v = 0;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        v = v * 10 + static_cast<unsigned long>(body[i] - '0');
        ++i;
    }
    if (v == 0 || v > 65535) return false;
    port_out = static_cast<std::uint16_t>(v);
    return true;
}

}  // namespace

RtpServer::RtpServer() : video_ssrc_(next_ssrc()), audio_ssrc_(next_ssrc()) {}

RtpServer::~RtpServer() { stop(); }

bool RtpServer::start(std::uint16_t signaling_port, std::string& err) {
    // Build the packetizers up front so the HELLO and the wire agree on
    // SSRC/PT.
    if (video_.codec == proto::Codec::kH265) {
        video_packer_ = std::make_unique<rtp::H265RtpPacketizer>(video_ssrc_);
    } else {
        video_packer_ = std::make_unique<rtp::H264RtpPacketizer>(video_ssrc_);
    }
    audio_packer_ = std::make_unique<rtp::OpusRtpPacketizer>(audio_ssrc_);

    if (!open_udp(err)) return false;

    running_ = true;
    nack_thread_ = std::thread([this] { nack_loop(); });
    sender_thread_ = std::thread([this] { sender_loop(); });

    if (!signaling_.start(
            signaling_port,
            [this](const sockaddr_in& peer) { on_connect(peer); },
            [this](const signal::Message& m) { on_message(m); }, err)) {
        running_ = false;
        return false;
    }
    std::fprintf(stderr, "[rtp] signaling on tcp/%u, video ssrc=%u\n",
                 signaling_port, video_ssrc_);
    return true;
}

bool RtpServer::open_udp(std::string& err) {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        err = std::string("udp socket: ") + std::strerror(errno);
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);  // ephemeral source port
    if (::bind(udp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("udp bind: ") + std::strerror(errno);
        ::close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }
    // Bump socket buffers: large frames (4K) produce 100+ RTP packets per
    // access unit; the default ~208 KB kernel buffers overflow during bursts.
    int rcvbuf = 1 << 20;  // 1 MB for incoming RTCP
    ::setsockopt(udp_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int sndbuf = 4 << 20;  // 4 MB for outgoing media bursts
    ::setsockopt(udp_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    return true;
}

void RtpServer::stop() {
    signaling_.stop();
    running_ = false;
    send_cv_.notify_all();  // wake the sender thread out of its wait
    if (udp_fd_ >= 0) {
        // Wake the NACK recvfrom.
        ::shutdown(udp_fd_, SHUT_RDWR);
    }
    if (nack_thread_.joinable()) nack_thread_.join();
    // Join the sender before closing the socket it writes to.
    if (sender_thread_.joinable()) sender_thread_.join();
    if (udp_fd_ >= 0) {
        ::close(udp_fd_);
        udp_fd_ = -1;
    }
    std::lock_guard<std::mutex> lk(peer_mu_);
    peer_streaming_ = false;
}

int RtpServer::peer_count() const {
    std::lock_guard<std::mutex> lk(peer_mu_);
    return peer_streaming_ ? 1 : 0;
}

void RtpServer::on_connect(const sockaddr_in& peer) {
    // Remember the client's TCP peer address; the UDP port arrives in READY.
    {
        std::lock_guard<std::mutex> lk(peer_mu_);
        peer_udp_ = peer;
        peer_udp_.sin_port = 0;  // unknown until READY
        peer_streaming_ = false;
    }
    send_hello();
}

void RtpServer::send_hello() {
    const char* codec = (video_.codec == proto::Codec::kH265) ? "h265" : "h264";
    char buf[512];
    int n = std::snprintf(
        buf, sizeof(buf),
        "{\"video\":{\"codec\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":%d,"
        "\"pt\":%u,\"ssrc\":%u,\"clock\":%u},"
        "\"audio\":{\"codec\":\"opus\",\"rate\":%d,\"channels\":%d,"
        "\"pt\":%u,\"ssrc\":%u,\"clock\":%u}}",
        codec, video_.width, video_.height, video_.fps_num,
        static_cast<unsigned>(rtp::kVideoPayloadType), video_ssrc_,
        rtp::kVideoClockRate, audio_rate_, audio_channels_,
        static_cast<unsigned>(rtp::kOpusPayloadType), audio_ssrc_,
        rtp::kOpusClockRate);
    if (n <= 0) return;
    signal::Message hello{signal::Type::kHello, std::string(buf, buf + n)};
    if (!signaling_.send(hello))
        std::fprintf(stderr, "[rtp] failed to send HELLO\n");
}

void RtpServer::on_message(const signal::Message& m) {
    if (m.type == signal::Type::kReady) {
        std::uint16_t port = 0;
        if (!parse_ready_port(m.body, port)) {
            std::fprintf(stderr, "[rtp] malformed READY: %s\n", m.body.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(peer_mu_);
            if (peer_udp_.sin_family != AF_INET ||
                peer_udp_.sin_addr.s_addr == 0) {
                std::fprintf(stderr, "[rtp] READY before connect\n");
                return;
            }
            peer_udp_.sin_port = htons(port);
            peer_streaming_ = true;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        {
            std::lock_guard<std::mutex> lk(peer_mu_);
            ::inet_ntop(AF_INET, &peer_udp_.sin_addr, ip, sizeof(ip));
        }
        std::fprintf(stderr, "[rtp] client ready -> udp %s:%u\n", ip, port);
        if (!signaling_.send({signal::Type::kStart, ""}))
            std::fprintf(stderr, "[rtp] failed to send START\n");
    } else if (m.type == signal::Type::kInput) {
        if (on_input) {
            input::Event e;
            if (input::parse(m.body, e)) on_input(e);
        }
    } else if (m.type == signal::Type::kBye) {
        std::lock_guard<std::mutex> lk(peer_mu_);
        peer_streaming_ = false;
        std::fprintf(stderr, "[rtp] client bye\n");
    }
}

void RtpServer::broadcast_video(const std::uint8_t* data, std::size_t size,
                                std::int64_t pts_usec, bool keyframe) {
    (void)keyframe;
    {
        std::lock_guard<std::mutex> lk(peer_mu_);
        if (!peer_streaming_) return;
    }

    std::vector<rtp::Packet> packets;
    video_packer_->packetize(data, size, pts_usec, packets);
    if (packets.empty()) return;

    for (const auto& pkt : packets) {
        video_retx_.record(rtp::seq_of(pkt), pkt);
        video_pkts_.fetch_add(1, std::memory_order_relaxed);
        video_octets_.fetch_add(pkt.size(), std::memory_order_relaxed);
    }

    // Hand the packets to sender_thread_ instead of writing (and pacing) them
    // here. The pacer's token-bucket sleep used to run inline on this — the
    // capture/encode thread — so a big IDR could block it for hundreds of ms
    // and break the even-PTS cadence. Enqueue-and-return keeps this thread free
    // to grab the next frame on the tick grid; the wire is still paced, just
    // from the other side of this queue.
    {
        std::lock_guard<std::mutex> lk(send_mu_);
        // Bound the backlog. A healthy stream drains between keyframes, so an
        // overflow means the link genuinely can't carry the encoder's output;
        // shed the oldest (most stale) packets rather than grow latency without
        // limit — the client's NACK/PLI path resyncs from the next keyframe.
        constexpr std::size_t kMaxQueued = 4096;
        if (send_q_.size() + packets.size() > kMaxQueued) {
            std::size_t overflow = send_q_.size() + packets.size() - kMaxQueued;
            while (overflow-- > 0 && !send_q_.empty()) send_q_.pop_front();
            send_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        for (auto& pkt : packets) send_q_.push_back(std::move(pkt));
    }
    send_cv_.notify_one();

    video_last_ts_.store(
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(pts_usec) * rtp::kVideoClockRate) /
            1'000'000),
        std::memory_order_relaxed);
}

void RtpServer::sender_loop() {
    std::vector<rtp::Packet> batch;
    while (running_) {
        {
            std::unique_lock<std::mutex> lk(send_mu_);
            send_cv_.wait(lk, [this] { return !running_ || !send_q_.empty(); });
            if (!running_ && send_q_.empty()) break;
            // Take everything queued so far in one swipe (still paced per packet
            // below); anything that arrives while we drain waits for the next
            // pass. Keeps send_mu_ held only briefly.
            batch.clear();
            batch.reserve(send_q_.size());
            for (auto& p : send_q_) batch.push_back(std::move(p));
            send_q_.clear();
        }
        sockaddr_in dst{};
        bool streaming;
        {
            std::lock_guard<std::mutex> lk(peer_mu_);
            streaming = peer_streaming_;
            dst = peer_udp_;
        }
        if (!streaming) continue;  // client gone — drop the batch
        for (const auto& pkt : batch) {
            if (!running_) break;
            // The shared pacer smooths keyframe bursts across *all* pipelines so
            // the aggregate stays under what the WiFi link can absorb. Now that
            // it runs here, the sleep costs wire latency, not a capture stall.
            if (pacer_) pacer_->consume(pkt.size());
            ::sendto(udp_fd_, pkt.data(), pkt.size(), 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
    }
}

void RtpServer::broadcast_audio(std::uint32_t /*channel_id*/,
                                const std::uint8_t* data, std::size_t size,
                                std::int64_t pts_usec) {
    sockaddr_in dst{};
    {
        std::lock_guard<std::mutex> lk(peer_mu_);
        if (!peer_streaming_) return;
        dst = peer_udp_;
    }

    std::vector<rtp::Packet> packets;
    audio_packer_->packetize(data, size, pts_usec, packets);
    for (auto& pkt : packets) {
        audio_retx_.record(rtp::seq_of(pkt), pkt);
        audio_pkts_.fetch_add(1, std::memory_order_relaxed);
        audio_octets_.fetch_add(pkt.size(), std::memory_order_relaxed);
        ::sendto(udp_fd_, pkt.data(), pkt.size(), 0,
                 reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }
    if (!packets.empty()) {
        audio_last_ts_.store(
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(pts_usec) * rtp::kOpusClockRate) /
                1'000'000),
            std::memory_order_relaxed);
    }
}

void RtpServer::nack_loop() {
    sockaddr_in from{};
    std::vector<std::uint8_t> buf(1 << 16);
    // Timeout so we can re-check running_ between receives.
    timeval tv{0, 200000};
    ::setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    auto last_sr = std::chrono::steady_clock::now();
    std::uint64_t prev_pkts = 0, prev_retx = 0, prev_drops = 0;
    while (running_) {
        socklen_t flen = sizeof(from);
        ssize_t n = ::recvfrom(udp_fd_, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
        if (n <= 0) {
            if (n < 0 &&
                (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                ;
            else if (!running_)
                break;
        } else {
            handle_rtcp(buf.data(), static_cast<std::size_t>(n), from);
        }
        // Emit RTCP Sender Reports roughly once per second for A/V sync.
        auto now = std::chrono::steady_clock::now();
        if (now - last_sr > std::chrono::seconds(1)) {
            last_sr = now;
            bool streaming;
            {
                std::lock_guard<std::mutex> lk(peer_mu_);
                streaming = peer_streaming_;
            }
            if (streaming) {
                send_sender_report(video_ssrc_, video_pkts_.load(),
                                   video_octets_.load(), video_last_ts_.load());
                send_sender_report(audio_ssrc_, audio_pkts_.load(),
                                   audio_octets_.load(), audio_last_ts_.load());
                // Per-monitor throughput/health, so monitor 0 (physical) and
                // the virtual monitors can be compared at a glance. High retx or
                // loss on the virtual line => the flicker is packet loss on the
                // wire; near-zero on both while it still flickers => the problem
                // is downstream (client decode of a second concurrent stream).
                const std::uint64_t pkts = video_pkts_.load();
                const std::uint64_t retx = video_retx_count_.load();
                const std::uint64_t drops = send_drops_.load();
                std::fprintf(stderr,
                             "[monitor %d stats] %llu pkt/s, %llu retx/s, "
                             "%llu qdrop/s, loss %.1f%%\n",
                             id_,
                             static_cast<unsigned long long>(pkts - prev_pkts),
                             static_cast<unsigned long long>(retx - prev_retx),
                             static_cast<unsigned long long>(drops - prev_drops),
                             static_cast<double>(
                                 last_loss_.load(std::memory_order_relaxed)) *
                                 100.0);
                prev_pkts = pkts;
                prev_retx = retx;
                prev_drops = drops;
            }
        }
    }
}

void RtpServer::handle_rtcp(const std::uint8_t* data, std::size_t size,
                            const sockaddr_in& /*from*/) {
    // Minimal RTCP feedback parser (RFC 4585). We service NACK (RTPFB, PT=205,
    // FMT=1) and PLI (PSFB, PT=206, FMT=1).
    if (size < 8) return;
    const std::uint8_t b0 = data[0];
    if ((b0 & 0xC0) != 0x80) return;  // V must be 2
    const std::uint8_t fmt = b0 & 0x1F;
    const std::uint8_t pt = data[1];
    const std::uint16_t len16 =
        (static_cast<std::uint16_t>(data[2]) << 8) | data[3];
    const std::size_t pkt_len =
        static_cast<std::size_t>(len16 + 1) * 4;  // 32-bit words
    if (pkt_len > size || pkt_len < 8) return;

    if (pt == 206 && fmt == 1) {
        // PLI — Picture Loss Indication. Force a keyframe on the next frame.
        if (on_keyframe_request) {
            std::fprintf(stderr, "[rtp] PLI received — requesting keyframe\n");
            on_keyframe_request();
        }
        return;
    }

    if (pt == 205 && fmt == 1) {
        // NACK. media SSRC at offset 8 selects the retransmit buffer.
        if (pkt_len < 16) return;  // header (12) + at least one FCI (4)
        std::fprintf(stderr, "[rtp] NACK received\n");
        std::uint32_t media_ssrc = (static_cast<std::uint32_t>(data[8]) << 24) |
                                   (data[9] << 16) | (data[10] << 8) | data[11];
        rtp::RetransmitBuffer* buf = nullptr;
        sockaddr_in dst{};
        {
            std::lock_guard<std::mutex> lk(peer_mu_);
            if (!peer_streaming_) return;
            dst = peer_udp_;
        }
        if (media_ssrc == video_ssrc_)
            buf = &video_retx_;
        else if (media_ssrc == audio_ssrc_)
            buf = &audio_retx_;
        if (!buf) return;

        for (std::size_t off = 12; off + 4 <= pkt_len; off += 4) {
            std::uint16_t pid = (data[off] << 8) | data[off + 1];
            std::uint16_t blp = (data[off + 2] << 8) | data[off + 3];
            auto retransmit = [&](std::uint16_t seq) {
                rtp::Packet p;
                if (buf->get(seq, p)) {
                    if (pacer_) pacer_->consume(p.size());
                    video_retx_count_.fetch_add(1, std::memory_order_relaxed);
                    ::sendto(udp_fd_, p.data(), p.size(), 0,
                             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
                }
            };
            retransmit(pid);
            for (int i = 0; i < 16; ++i) {
                if (blp & (1u << i)) retransmit(pid + 1 + i);
            }
        }
        return;
    }

    if (pt == 201) {
        // Receiver Report. Scan the report blocks for our video SSRC and
        // surface fraction-lost to the bitrate controller.
        const std::uint8_t rc = b0 & 0x1F;
        std::size_t off = 8;  // header + reporter SSRC
        for (std::uint8_t i = 0; i < rc && off + 24 <= pkt_len;
             ++i, off += 24) {
            std::uint32_t block_ssrc =
                (static_cast<std::uint32_t>(data[off]) << 24) |
                (data[off + 1] << 16) | (data[off + 2] << 8) | data[off + 3];
            if (block_ssrc != video_ssrc_) continue;
            const float fraction_lost =
                static_cast<float>(data[off + 4]) / 256.0f;
            last_loss_.store(fraction_lost, std::memory_order_relaxed);
            if (on_loss_report) on_loss_report(fraction_lost);
        }
        return;
    }
}

void RtpServer::send_sender_report(std::uint32_t ssrc, std::uint64_t packets,
                                   std::uint64_t octets,
                                   std::uint32_t last_rtp_ts) {
    sockaddr_in dst{};
    {
        std::lock_guard<std::mutex> lk(peer_mu_);
        if (!peer_streaming_) return;
        dst = peer_udp_;
    }
    // NTP timestamp (wall clock since 1900 epoch).
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() %
        1'000'000'000;
    std::uint32_t ntp_msw = static_cast<std::uint32_t>(secs) + 2'208'988'800u;
    std::uint32_t ntp_lsw = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(ns) * 4'294'967'296ull / 1'000'000'000ull);

    std::uint8_t sr[28];
    sr[0] = 0x80;  // V=2, P=0, RC=0
    sr[1] = 200;   // PT=SR
    sr[2] = 0x00;  // length = 6 words (28 bytes)
    sr[3] = 0x06;
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        sr[off] = (v >> 24) & 0xFF;
        sr[off + 1] = (v >> 16) & 0xFF;
        sr[off + 2] = (v >> 8) & 0xFF;
        sr[off + 3] = v & 0xFF;
    };
    put32(4, ssrc);
    put32(8, ntp_msw);
    put32(12, ntp_lsw);
    put32(16, last_rtp_ts);
    put32(20, static_cast<std::uint32_t>(packets));
    put32(24, static_cast<std::uint32_t>(octets));
    ::sendto(udp_fd_, sr, sizeof(sr), 0, reinterpret_cast<sockaddr*>(&dst),
             sizeof(dst));
}

}  // namespace metashare
