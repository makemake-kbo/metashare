// Raw RTP packetizers for the MetaShare streamer.
//
// Builds RTP packets (RFC 3550) for already-encoded media:
//   * H265RtpPacketizer  — H.265/HEVC per RFC 7798 (Single NAL + FU; AP on rx)
//   * OpusRtpPacketizer  — one Opus packet per RTP packet (RFC 7587)
//
// Input to the H.265 packetizer is an Annex B access unit (start codes + NALs)
// — exactly what the libavcodec encoder emits. Each call to packetize() emits
// the complete set of RTP packets for one access unit / Opus frame, advancing
// the internal sequence number and stamping a single PTS-derived timestamp.
//
// A RetransmitBuffer keeps a sliding window of recently-sent packets so the
// UDP server can retransmit on NACK.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace metashare::rtp {

// RTP fixed header constants (RFC 3550). No CSRC list, no header extension.
inline constexpr std::uint32_t kVideoClockRate = 90000;  // Hz
inline constexpr std::uint32_t kOpusClockRate = 48000;   // Hz
inline constexpr std::uint8_t kVideoPayloadType = 96;    // dynamic
inline constexpr std::uint8_t kOpusPayloadType = 111;    // dynamic, convention
inline constexpr std::size_t kRtpHeaderSize = 12;
// Largest RTP packet we emit (header + payload). Stays well under a 1500-byte
// Ethernet frame once IP + UDP headers are added.
inline constexpr std::size_t kMaxPacketSize = 1400;

// One complete RTP packet (header + payload), ready for sendto().
using Packet = std::vector<std::uint8_t>;

// Build the fixed 12-byte RTP header into dst (which must have room). marker
// is the M bit; the timestamp/ssrc/seq identify the packet.
void write_header(std::uint8_t* dst, std::uint8_t payload_type, bool marker,
                  std::uint16_t seq, std::uint32_t timestamp,
                  std::uint32_t ssrc);

// Read the 16-bit sequence number out of an RTP packet (big-endian bytes 2-3).
inline std::uint16_t seq_of(const Packet& p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[2]) << 8) |
                                      p[3]);
}

// Common sequence-number + config state for the per-codec packetizers.
class PacketizerBase {
  public:
    PacketizerBase(std::uint32_t ssrc, std::uint8_t payload_type,
                   std::uint32_t clock_rate)
        : ssrc_(ssrc), pt_(payload_type), clock_rate_(clock_rate) {}

    virtual ~PacketizerBase() = default;

    std::uint32_t ssrc() const { return ssrc_; }
    std::uint8_t payload_type() const { return pt_; }
    std::uint32_t clock_rate() const { return clock_rate_; }

    // Produce complete RTP packets for the given payload, appending to out.
    virtual void packetize(const std::uint8_t* data, std::size_t size,
                           std::int64_t pts_usec, std::vector<Packet>& out) = 0;

  protected:
    std::uint16_t next_seq() {
        return seq_.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint32_t stamp(std::int64_t pts_usec) const {
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(pts_usec) * clock_rate_) / 1'000'000);
    }

    const std::uint32_t ssrc_;
    const std::uint8_t pt_;
    const std::uint32_t clock_rate_;
    std::atomic<std::uint16_t> seq_{0};
};

// H.265/HEVC RTP packetizer (RFC 7798). Emits Single NAL Unit packets for NALs
// that fit in the MTU, and Fragmentation Units for larger ones. The RTP marker
// bit is set on the final packet of the access unit.
class H265RtpPacketizer : public PacketizerBase {
  public:
    explicit H265RtpPacketizer(std::uint32_t ssrc)
        : PacketizerBase(ssrc, kVideoPayloadType, kVideoClockRate) {}

    // data/size is one Annex B access unit. Appends complete RTP packets to
    // out.
    void packetize(const std::uint8_t* data, std::size_t size,
                   std::int64_t pts_usec, std::vector<Packet>& out) override;
};

// H.264/AVC RTP packetizer (RFC 6184). Same Single NAL + FU-A strategy as the
// H.265 packetizer but with 1-byte NAL headers. Used when the encoder could not
// open a hardware HEVC encoder and fell back to software H.264.
class H264RtpPacketizer : public PacketizerBase {
  public:
    explicit H264RtpPacketizer(std::uint32_t ssrc)
        : PacketizerBase(ssrc, kVideoPayloadType, kVideoClockRate) {}

    void packetize(const std::uint8_t* data, std::size_t size,
                   std::int64_t pts_usec, std::vector<Packet>& out) override;
};

// Opus RTP packetizer (RFC 7587). One Opus packet per RTP packet; marker bit
// is always set (each Opus packet is a complete frame).
class OpusRtpPacketizer : public PacketizerBase {
  public:
    explicit OpusRtpPacketizer(std::uint32_t ssrc)
        : PacketizerBase(ssrc, kOpusPayloadType, kOpusClockRate) {}

    void packetize(const std::uint8_t* data, std::size_t size,
                   std::int64_t pts_usec, std::vector<Packet>& out) override;
};

// Sliding window of recently-sent packets keyed by sequence number, used to
// service NACK retransmission requests. Thread-safe.
class RetransmitBuffer {
  public:
    explicit RetransmitBuffer(std::size_t capacity = 512)
        : capacity_(capacity) {}

    // Record a sent packet (copy stored). seq is its RTP sequence number.
    void record(std::uint16_t seq, Packet packet);

    // Copy a retained packet into out. Returns false if not retained. A copy
    // (not a pointer) so concurrent record() eviction can't invalidate it.
    bool get(std::uint16_t seq, Packet& out) const;

  private:
    mutable std::mutex mu_;
    std::map<std::uint16_t, Packet> packets_;
    // Insertion order for eviction — evicting by lowest seq breaks at the
    // 16-bit wrap (it would evict the *newest* packets).
    std::deque<std::uint16_t> order_;
    std::size_t capacity_;
};

}  // namespace metashare::rtp
