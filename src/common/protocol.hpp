// MetaShare discovery protocol — shared between streamer and client.
//
// Two channels:
//   * Discovery  : UDP, default port 7777. Client broadcasts a probe, streamer
//                  replies with an offer describing where to connect.
//                  (Transport-agnostic — unchanged across the raw-RTP switch.)
//   * Signaling  : TCP, default port 7778. Exchanges stream params + UDP ports
//                  (HELLO/READY/START/BYE, see src/common/signaling.hpp), then
//                  raw H.265/Opus RTP flows over UDP.
//
// All multi-byte integers are little-endian on the wire. We target x86_64 hosts
// and aarch64 (Quest) clients, both little-endian, so no byte swapping is done.

#pragma once

#include <cstdint>

namespace metashare::proto {

inline constexpr std::uint16_t kDiscoveryPort = 7777;

// Bumped whenever the discovery structs below change incompatibly.
//
// v4 (raw-rtp): media transport switched from WebRTC/libdatachannel to raw
//               H.265/Opus RTP over UDP. Discovery structs are binary-identical
//               to v3 (the client_caps field is retained for size compat but
//               ignored — codec negotiation now happens over TCP signaling).
//               The TCP signaling protocol changed (HELLO/READY/START/BYE
//               replace OFFER/ANSWER/ICE/OK), and there is no SDP/ICE.
inline constexpr std::uint32_t kProtocolVersion = 4;

// ---- Discovery (UDP) -------------------------------------------------------

inline constexpr char kDiscoveryMagic[8] = {'M', 'S', 'H', 'A',
                                            'R', 'E', 'D', '1'};

// Sent by the client as a broadcast to kDiscoveryPort.
struct DiscoveryProbe {
    char magic[8];              // kDiscoveryMagic
    std::uint32_t version;      // kProtocolVersion
    std::uint32_t client_caps;  // reserved (was ClientCaps bitfield); ignored
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

// Video codec enum — selects the RTP packetizer / MediaCodec decoder.
// Negotiation moved to the TCP handshake in v4; this enum is shared by the
// encoder, the RTP server and the HELLO JSON "codec" field.
enum class Codec : std::uint32_t {
    kH264 = 0,
    kH265 = 1,
};

}  // namespace metashare::proto
