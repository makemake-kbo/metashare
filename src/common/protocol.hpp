// MetaShare discovery protocol — shared between streamer and client.
//
// Two channels:
//   * Discovery  : UDP, default port 7777. Client broadcasts a probe, streamer
//                  replies with an offer describing where to connect.
//   * Signaling  : TCP, default port 7778. Used to exchange WebRTC SDP offers/
//                  answers and ICE candidates; once negotiated, real media
//                  flows over WebRTC (UDP/SRTP via libdatachannel).
//
// All multi-byte integers are little-endian on the wire. We target x86_64 hosts
// and aarch64 (Quest) clients, both little-endian, so no byte swapping is done.

#pragma once

#include <cstdint>

namespace metashare::proto {

inline constexpr std::uint16_t kDiscoveryPort = 7777;

// Bumped whenever the discovery structs below change incompatibly.
//
// v3 (webrtc): discovery structs are unchanged from v2, but the TCP stream
//              protocol is GONE — port 7778 now carries signaling for WebRTC
//              rather than the legacy binary frame stream. Clients must
//              implement the new line-based signaling protocol
//              (src/common/signaling.hpp) instead of the v2 frame header.
inline constexpr std::uint32_t kProtocolVersion = 3;

// ---- Discovery (UDP) -------------------------------------------------------

inline constexpr char kDiscoveryMagic[8] = {'M', 'S', 'H', 'A',
                                            'R', 'E', 'D', '1'};

// Sent by the client as a broadcast to kDiscoveryPort.
struct DiscoveryProbe {
    char magic[8];              // kDiscoveryMagic
    std::uint32_t version;      // kProtocolVersion
    std::uint32_t client_caps;  // ClientCaps bitfield
} __attribute__((packed));

// Streamer's unicast reply back to the probing client.
struct DiscoveryOffer {
    char magic[8];          // kDiscoveryMagic
    std::uint32_t version;  // kProtocolVersion
    std::uint16_t
        signaling_port;  // TCP base port (monitor i is at signaling_port + i)
    std::uint16_t monitor_count;  // number of available monitors (0 → 1)
    char host_name[64];           // human-readable, NUL-terminated
} __attribute__((packed));

enum ClientCaps : std::uint32_t {
    kCapH264 = 1u << 0,
    kCapH265 = 1u << 1,
};

// Video codec enum — used by the encoder and the WebRTC server to negotiate
// the right RTP packetizer. Kept here because the discovery probe carries
// kCapH264/kCapH265 bits that map 1:1 to these values.
enum class Codec : std::uint32_t {
    kH264 = 0,
    kH265 = 1,
};

}  // namespace metashare::proto
