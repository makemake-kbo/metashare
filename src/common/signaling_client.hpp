// TCP signaling client: connects to the streamer's signaling port and reads
// newline-framed messages. Used by the desktop test client to exchange SDP
// offers/answers and ICE candidates with the WebRtcServer.
//
// Same wire protocol as signaling_server.hpp. Each instance = one TCP
// connection = one PeerConnection.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "signaling.hpp"

namespace metashare::signal {

class Client {
  public:
    using OnMessage = std::function<void(const Message&)>;

    Client() = default;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Connect to host:port. Blocks until either the initial OK is received
    // (returns true) or the connection fails (returns false, err is set).
    bool connect(const std::string& host, std::uint16_t port,
                 OnMessage on_message, std::string& err);
    void disconnect();

    bool send(const Message& m);

  private:
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread read_thread_;
    mutable std::mutex send_mu_;
};

}  // namespace metashare::signal
