// MetaShare signaling protocol — TCP line-based, used to exchange WebRTC
// SDP offers/answers and trickle ICE candidates between streamer and client
// before the actual media flows over UDP/SRTP via libdatachannel.
//
// Wire format: newline-terminated ASCII lines. The SDP/candidate bodies are
// base64-encoded so the framing stays trivial (no JSON escaping of `\r\n`).
//
//   OFFER <b64-sdp>            client -> server: SDP offer
//   ANSWER <b64-sdp>           server -> client: SDP answer
//   ICE <b64-candidate> <mid>  bidirectional: trickle ICE candidate
//                              (mid = sdpMid, an unsigned int)
//   OK                         server -> client: signaling ready
//   BYE                       either side: clean shutdown
//
// Discovery (UDP) still uses the legacy binary structs from protocol.hpp.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace metashare::signal {

// Reserved TCP port for the signaling server (was kStreamPort).
inline constexpr std::uint16_t kDefaultSignalingPort = 7778;

// One decoded signaling message.
enum class Type { kOffer, kAnswer, kIce, kOk, kBye };

struct Message {
    Type type;
    std::string body;  // SDP (offer/answer) or candidate string (ice)
    std::string mid;   // sdpMid (ice only); empty otherwise
};

// Serialize a Message to its wire line (without the trailing newline).
std::string serialize(const Message& m);

// Parse one wire line (newline already stripped). Returns false on malformed
// input; out is left untouched on failure.
bool parse(std::string_view line, Message& out);

}  // namespace metashare::signal
