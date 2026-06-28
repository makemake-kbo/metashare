// UDP discovery responder. Listens on the default discovery port for client
// broadcast probes and replies with a unicast offer pointing at our signaling
// port — so the Quest client finds the host with zero configuration.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace metashare {

class DiscoveryResponder {
  public:
    DiscoveryResponder() = default;
    ~DiscoveryResponder();

    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    bool start(std::uint16_t signaling_port, std::uint16_t monitor_count,
               std::string& err);
    void stop();

  private:
    void serve_loop();

    int fd_ = -1;
    std::uint16_t signaling_port_ = 0;
    std::uint16_t monitor_count_ = 1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace metashare
