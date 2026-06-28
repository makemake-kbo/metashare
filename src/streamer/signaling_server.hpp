// TCP signaling server: accepts one connection per client, reads newline-
// framed signaling messages, dispatches them via a callback. Send is direct
// via send_message(). Each client connection runs on its own thread.
//
// This is intentionally minimal — no auth, no TLS, no multiplexing. LAN-only.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "signaling.hpp"

namespace metashare::signal {

class Server {
  public:
    using OnMessage = std::function<void(const Message&)>;
    using OnDisconnect = std::function<void()>;

    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Start listening on the given port. on_message is invoked on the
    // server's accept thread for each received message from any client.
    // At most one client is supported at a time; a second connect blocks
    // until the first disconnects.
    bool start(std::uint16_t port, OnMessage on_message, std::string& err);
    void stop();

    // True if a client is currently connected.
    bool has_client() const;

    // Send a message to the connected client. Returns false if no client
    // is connected or the write fails.
    bool send(const Message& m);

  private:
    void client_loop(int fd, OnMessage on_message);

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::atomic<int> client_fd_{-1};
    mutable std::mutex send_mu_;
};

}  // namespace metashare::signal
