#include "rtp_packetizer.hpp"

#include <cstring>

namespace metashare::rtp {

void write_header(std::uint8_t* p, std::uint8_t payload_type, bool marker,
                  std::uint16_t seq, std::uint32_t timestamp,
                  std::uint32_t ssrc) {
    p[0] = 0x80;  // V=2, P=0, X=0, CC=0
    p[1] = static_cast<std::uint8_t>((marker ? 0x80 : 0x00) |
                                     (payload_type & 0x7F));
    p[2] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(seq & 0xFF);
    p[4] = static_cast<std::uint8_t>((timestamp >> 24) & 0xFF);
    p[5] = static_cast<std::uint8_t>((timestamp >> 16) & 0xFF);
    p[6] = static_cast<std::uint8_t>((timestamp >> 8) & 0xFF);
    p[7] = static_cast<std::uint8_t>(timestamp & 0xFF);
    p[8] = static_cast<std::uint8_t>((ssrc >> 24) & 0xFF);
    p[9] = static_cast<std::uint8_t>((ssrc >> 16) & 0xFF);
    p[10] = static_cast<std::uint8_t>((ssrc >> 8) & 0xFF);
    p[11] = static_cast<std::uint8_t>(ssrc & 0xFF);
}

namespace {

// Walk an Annex B byte stream and call on_nal(ptr, size) for each NAL unit
// (start codes stripped). Trailing garbage shorter than 2 bytes is dropped
// (a valid H.265 NAL header is 2 octets).
template <typename F>
void for_each_nal(const std::uint8_t* data, std::size_t size, F&& on_nal) {
    std::size_t i = 0;
    const std::size_t n = size;
    // Find the first start code.
    auto find_start = [&](std::size_t from) -> std::size_t {
        for (std::size_t k = from; k + 2 < n; ++k) {
            if (data[k] == 0 && data[k + 1] == 0) {
                if (data[k + 2] == 1) return k + 3;  // 00 00 01
                if (k + 3 < n && data[k + 2] == 0 && data[k + 3] == 1)
                    return k + 4;  // 00 00 00 01
            }
        }
        return n;
    };

    std::size_t nal_start = find_start(0);
    while (nal_start < n) {
        // The next NAL begins where the following start code would be. Scan for
        // 00 00 (0[1] | 0001) without skipping the current NAL prematurely.
        std::size_t k = nal_start;
        std::size_t nal_end = n;
        for (; k + 2 < n; ++k) {
            if (data[k] == 0 && data[k + 1] == 0 &&
                (data[k + 2] == 1 ||
                 (k + 3 < n && data[k + 2] == 0 && data[k + 3] == 1))) {
                nal_end = k;
                break;
            }
        }
        // Trim trailing zero-padding bytes that some encoders emit before the
        // next start code (prevents synthesizing a fake 00 00 00 NAL).
        std::size_t end = nal_end;
        while (end > nal_start && data[end - 1] == 0) --end;
        if (end - nal_start >= 2) on_nal(data + nal_start, end - nal_start);
        nal_start = find_start(nal_end);
    }
}

}  // namespace

void H265RtpPacketizer::packetize(const std::uint8_t* data, std::size_t size,
                                  std::int64_t pts_usec,
                                  std::vector<Packet>& out) {
    if (size == 0) return;
    const std::uint32_t ts = stamp(pts_usec);

    // First pass: collect NAL units so we know which is last (for the marker
    // bit) without re-scanning.
    struct Nal {
        const std::uint8_t* p;
        std::size_t len;
    };
    std::vector<Nal> nals;
    for_each_nal(data, size, [&](const std::uint8_t* p, std::size_t len) {
        nals.push_back({p, len});
    });
    if (nals.empty()) return;

    const std::size_t single_max = kMaxPacketSize - kRtpHeaderSize;  // 1388
    // FU payload budget per packet: MTU - RTP hdr - 2 PayloadHdr - 1 FU hdr.
    const std::size_t fu_payload_max =
        kMaxPacketSize - kRtpHeaderSize - 3;  // 1385

    for (std::size_t ni = 0; ni < nals.size(); ++ni) {
        const auto& nal = nals[ni];
        const bool is_last_nal = (ni + 1 == nals.size());

        if (nal.len <= single_max) {
            // Single NAL Unit packet (payload is the whole NAL incl. header).
            Packet pkt(kRtpHeaderSize + nal.len);
            write_header(pkt.data(), pt_, is_last_nal, next_seq(), ts, ssrc_);
            std::memcpy(pkt.data() + kRtpHeaderSize, nal.p, nal.len);
            out.push_back(std::move(pkt));
            continue;
        }

        // Fragmentation Unit. NAL header is the first 2 bytes.
        const std::uint8_t h0 = nal.p[0];
        const std::uint8_t h1 = nal.p[1];
        const unsigned orig_type = (h0 >> 1) & 0x3F;
        const std::uint8_t fu_hdr0 =
            static_cast<std::uint8_t>((h0 & 0x81) | (49u << 1));  // type=49
        const std::uint8_t fu_hdr1 = h1;
        const std::uint8_t* body = nal.p + 2;
        std::size_t body_len = nal.len - 2;

        std::size_t off = 0;
        while (off < body_len) {
            std::size_t chunk = std::min(fu_payload_max, body_len - off);
            bool is_first = (off == 0);
            bool is_last_frag = (off + chunk == body_len);
            Packet pkt(kRtpHeaderSize + 3 + chunk);
            std::uint8_t* dst = pkt.data();
            // Marker set only if this FU fragment is the last packet of the
            // whole access unit (last fragment of the last NAL).
            const bool marker = is_last_nal && is_last_frag;
            write_header(dst, pt_, marker, next_seq(), ts, ssrc_);
            dst[kRtpHeaderSize] = fu_hdr0;
            dst[kRtpHeaderSize + 1] = fu_hdr1;
            dst[kRtpHeaderSize + 2] = static_cast<std::uint8_t>(
                (is_first ? 0x80 : 0x00) | (is_last_frag ? 0x40 : 0x00) |
                (orig_type & 0x3F));
            std::memcpy(dst + kRtpHeaderSize + 3, body + off, chunk);
            out.push_back(std::move(pkt));
            off += chunk;
        }
    }
}

void H264RtpPacketizer::packetize(const std::uint8_t* data, std::size_t size,
                                  std::int64_t pts_usec,
                                  std::vector<Packet>& out) {
    if (size == 0) return;
    const std::uint32_t ts = stamp(pts_usec);

    struct Nal {
        const std::uint8_t* p;
        std::size_t len;
    };
    std::vector<Nal> nals;
    for_each_nal(data, size, [&](const std::uint8_t* p, std::size_t len) {
        nals.push_back({p, len});
    });
    if (nals.empty()) return;

    const std::size_t single_max = kMaxPacketSize - kRtpHeaderSize;
    // FU-A payload budget: MTU - RTP hdr - 1 FU indicator - 1 FU header.
    const std::size_t fu_payload_max = kMaxPacketSize - kRtpHeaderSize - 2;

    for (std::size_t ni = 0; ni < nals.size(); ++ni) {
        const auto& nal = nals[ni];
        const bool is_last_nal = (ni + 1 == nals.size());

        if (nal.len <= single_max) {
            Packet pkt(kRtpHeaderSize + nal.len);
            write_header(pkt.data(), pt_, is_last_nal, next_seq(), ts, ssrc_);
            std::memcpy(pkt.data() + kRtpHeaderSize, nal.p, nal.len);
            out.push_back(std::move(pkt));
            continue;
        }

        // FU-A (RFC 6184). NAL header is the first byte.
        const std::uint8_t h0 = nal.p[0];
        const unsigned orig_type = h0 & 0x1F;
        const std::uint8_t fu_ind = static_cast<std::uint8_t>((h0 & 0xE0) | 28);
        const std::uint8_t* body = nal.p + 1;
        std::size_t body_len = nal.len - 1;

        std::size_t off = 0;
        while (off < body_len) {
            std::size_t chunk = std::min(fu_payload_max, body_len - off);
            bool is_first = (off == 0);
            bool is_last_frag = (off + chunk == body_len);
            Packet pkt(kRtpHeaderSize + 2 + chunk);
            std::uint8_t* dst = pkt.data();
            const bool marker = is_last_nal && is_last_frag;
            write_header(dst, pt_, marker, next_seq(), ts, ssrc_);
            dst[kRtpHeaderSize] = fu_ind;
            dst[kRtpHeaderSize + 1] = static_cast<std::uint8_t>(
                (is_first ? 0x80 : 0x00) | (is_last_frag ? 0x40 : 0x00) |
                (orig_type & 0x1F));
            std::memcpy(dst + kRtpHeaderSize + 2, body + off, chunk);
            out.push_back(std::move(pkt));
            off += chunk;
        }
    }
}

void OpusRtpPacketizer::packetize(const std::uint8_t* data, std::size_t size,
                                  std::int64_t pts_usec,
                                  std::vector<Packet>& out) {
    if (size == 0) return;
    Packet pkt(kRtpHeaderSize + size);
    write_header(pkt.data(), pt_, /*marker=*/true, next_seq(), stamp(pts_usec),
                 ssrc_);
    std::memcpy(pkt.data() + kRtpHeaderSize, data, size);
    out.push_back(std::move(pkt));
}

void RetransmitBuffer::record(std::uint16_t seq, Packet packet) {
    std::lock_guard<std::mutex> lk(mu_);
    auto [it, inserted] = packets_.insert_or_assign(seq, std::move(packet));
    if (inserted) order_.push_back(seq);
    while (order_.size() > capacity_) {
        packets_.erase(order_.front());
        order_.pop_front();
    }
}

bool RetransmitBuffer::get(std::uint16_t seq, Packet& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = packets_.find(seq);
    if (it == packets_.end()) return false;
    out = it->second;
    return true;
}

}  // namespace metashare::rtp
