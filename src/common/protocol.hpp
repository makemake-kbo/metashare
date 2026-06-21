// MetaShare wire protocol — shared between streamer and client.
//
// Two channels:
//   * Discovery  : UDP, default port 7777. Client broadcasts a probe, streamer
//                  replies with an offer describing where to connect.
//   * Stream     : TCP, default port 7778. After connect the streamer sends one
//                  StreamHeader, then an unbounded sequence of FrameHeader +
//                  payload (an encoded video access unit, Annex-B for H.264).
//
// All multi-byte integers are little-endian on the wire. We target x86_64 hosts
// and aarch64 (Quest) clients, both little-endian, so no byte swapping is done.

#pragma once

#include <cstdint>

namespace metashare::proto {

inline constexpr std::uint16_t kDiscoveryPort = 7777;
inline constexpr std::uint16_t kStreamPort = 7778;

// Bumped whenever any struct below changes incompatibly.
inline constexpr std::uint32_t kProtocolVersion = 1;

// ---- Discovery (UDP) -------------------------------------------------------

inline constexpr char kDiscoveryMagic[8] = {'M', 'S', 'H', 'A', 'R', 'E', 'D', '1'};

// Sent by the client as a broadcast to kDiscoveryPort.
struct DiscoveryProbe {
    char magic[8];              // kDiscoveryMagic
    std::uint32_t version;      // kProtocolVersion
    std::uint32_t client_caps;  // ClientCaps bitfield
} __attribute__((packed));

// Streamer's unicast reply back to the probing client.
struct DiscoveryOffer {
    char magic[8];              // kDiscoveryMagic
    std::uint32_t version;      // kProtocolVersion
    std::uint16_t stream_port;  // TCP base port (monitor i is at stream_port + i)
    std::uint16_t monitor_count;// number of available monitors (0 → 1)
    char host_name[64];         // human-readable, NUL-terminated
} __attribute__((packed));

enum ClientCaps : std::uint32_t {
    kCapH264 = 1u << 0,
    kCapH265 = 1u << 1,
};

// ---- Stream (TCP) ----------------------------------------------------------

enum class Codec : std::uint32_t {
    kH264 = 0,
    kH265 = 1,
};

inline constexpr char kStreamMagic[8] = {'M', 'S', 'H', 'A', 'R', 'E', 'S', '1'};

// First message after the TCP connection is established.
struct StreamHeader {
    char magic[8];              // kStreamMagic
    std::uint32_t version;      // kProtocolVersion
    std::uint32_t codec;        // Codec
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fps_num;
    std::uint32_t fps_den;
} __attribute__((packed));

enum FrameFlags : std::uint32_t {
    kFrameKeyframe = 1u << 0,   // IDR / keyframe; safe decode entry point
};

// Precedes every encoded access unit.
struct FrameHeader {
    std::uint32_t payload_size; // bytes following this header
    std::uint32_t flags;        // FrameFlags
    std::uint64_t pts_usec;     // presentation timestamp, microseconds
};

}  // namespace metashare::proto
