// TCP signaling server: accepts one connection per client, notifies on
// connect (so the owner can send the initial HELLO), reads newline-framed
// signaling messages, dispatches them via a callback. Send is direct via
// send_message(). Each client connection runs on its own thread.
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

#include <netinet/in.h>

#include "signaling.hpp"

namespace metashare::signal {

class Server {
  public:
    using OnMessage = std::function<void(const Message&)>;
    using OnDisconnect = std::function<void()>;
    // Fired once per accepted client, right before the read loop starts.
    // peer holds the client's TCP source address (so the owner can target the
    // same host for UDP media).
    using OnConnect = std::function<void(const sockaddr_in& peer)>;

    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Start listening on the given port. on_connect is invoked once per
    // accepted client (server thread); on_message is invoked for each received
    // message. At most one client is supported at a time; a second connect
    // blocks until the first disconnects.
    bool start(std::uint16_t port, OnConnect on_connect, OnMessage on_message,
               std::string& err);
    void stop();

    // True if a client is currently connected.
    bool has_client() const;

    // Send a message to the connected client. Returns false if no client
    // is connected or the write fails.
    bool send(const Message& m);

  private:
    void client_loop(int fd, const sockaddr_in& peer,
                     const OnConnect& on_connect, const OnMessage& on_message);

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::atomic<int> client_fd_{-1};
    mutable std::mutex send_mu_;
};

}  // namespace metashare::signal
