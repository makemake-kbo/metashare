// TCP server that fans out encoded frames to connected clients.
//
// On accept, a client receives the StreamHeader, then frames once the next
// keyframe arrives (so it can start decoding cleanly). Slow/broken clients are
// dropped rather than stalling the encoder.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.hpp"

namespace metashare {

class NetServer {
  public:
    NetServer() = default;
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Must be set before start(); describes the stream to late-joining clients.
    void set_stream_header(const proto::StreamHeader& h) { header_ = h; }

    bool start(std::uint16_t port, std::string& err);
    void stop();

    // Send one encoded access unit to all ready clients.
    void broadcast(const proto::FrameHeader& fh, const std::uint8_t* payload);

    int client_count() const;

  private:
    struct Client {
        int fd = -1;
        bool ready = false;  // has received a keyframe yet
    };

    void accept_loop();
    static bool send_all(int fd, const void* data, std::size_t len);

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    proto::StreamHeader header_{};
    mutable std::mutex clients_mu_;
    std::vector<Client> clients_;
};

}  // namespace metashare
