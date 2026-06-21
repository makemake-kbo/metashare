// Discovery + TCP receive for the Quest client. Portable POSIX C++ (works as-is
// on Android); reuses the shared wire protocol. Decode-independent: it only
// produces encoded access units (H.264 or HEVC, per the stream header) for the
// Decoder to consume.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.hpp"

namespace metashare {

struct EncodedFrame {
    std::vector<std::uint8_t> data;
    std::uint64_t pts_usec = 0;
    bool keyframe = false;
};

class NetStream {
public:
    NetStream() = default;
    ~NetStream();

    // Broadcast a probe and block up to timeout_ms for an offer. On success
    // fills host_/port_ and returns true.
    bool discover(int timeout_ms);

    // Use an explicit host instead of discovery.
    void set_host(const std::string& host, std::uint16_t port);

    // Connect, read the StreamHeader, and spawn the receive thread.
    bool connect_and_run(std::string& err);
    void stop();

    // Pop the next encoded frame (waits up to timeout_ms). Returns false on
    // timeout or disconnect.
    bool pop_frame(EncodedFrame& out, int timeout_ms);

    const proto::StreamHeader& header() const { return header_; }
    bool connected() const { return running_; }

private:
    void recv_loop();

    std::string host_;
    std::uint16_t port_ = proto::kStreamPort;
    int fd_ = -1;
    proto::StreamHeader header_{};

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<EncodedFrame> queue_;
    static constexpr std::size_t kMaxQueued = 8;  // drop old frames if behind
};

}  // namespace metashare
