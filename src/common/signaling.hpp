// MetaShare signaling protocol — TCP line-based, used to bootstrap a raw RTP
// session (no SDP, no ICE). The streamer advertises its stream parameters, the
// client replies with the UDP port it will listen on, and the streamer starts
// pushing RTP. After that, signaling only carries the final BYE.
//
// Wire format: newline-terminated ASCII lines. Bodies (HELLO/READY) are compact
// JSON (no embedded newlines), so framing stays trivial and base64 is gone.
//
//   HELLO <json>   server -> client: stream params
//                  (codec, resolution, fps, SSRCs, payload types, clock rates)
//   READY <json>   client -> server: {"port": <client UDP port>}
//   START                       server -> client: RTP streaming begins
//   BYE                         either side: clean shutdown
//
// One UDP port carries both video and audio, demultiplexed by SSRC.
//
// Discovery (UDP) still uses the binary structs from protocol.hpp.

#pragma once

#include <cstdint>
#include <string>

namespace metashare::signal {

// Reserved TCP port for the signaling server.
inline constexpr std::uint16_t kDefaultSignalingPort = 7778;

// One decoded signaling message. PING is a client->server keepalive so the
// server can distinguish an idle-but-alive client from a vanished one (and
// free the single per-monitor slot for reconnection).
enum class Type { kHello, kReady, kStart, kBye, kPing };

struct Message {
    Type type;
    // JSON body for HELLO/READY; empty for START/BYE.
    std::string body;
};

// Serialize a Message to its wire line (without the trailing newline).
std::string serialize(const Message& m);

// Parse one wire line (newline already stripped). Returns false on malformed
// input; out is left untouched on failure.
bool parse(std::string_view line, Message& out);

}  // namespace metashare::signal
