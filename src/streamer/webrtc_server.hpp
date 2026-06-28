// WebRTC server: accepts a signaling-driven PeerConnection (negotiated via
// SignalingServer) and exposes a simple broadcast API for already-encoded
// H.265 video access units and Opus audio packets.
//
// One WebRtcServer per monitor pipeline. Multiple simultaneously-connected
// clients are supported (each gets its own PeerConnection); media is fanned
// out to all of them.
//
// Codec plan (matches what we built the encoder for):
//   * video: H.265 (HEVC) via libdatachannel's H265RtpPacketizer
//   * audio: Opus via the base RtpPacketizer (Opus packets fit in one RTP
//            packet — no fragmentation needed)
//
// libdatachannel handles DTLS-SRTP, ICE, NACK retransmit, RTCP SR/RR.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "protocol.hpp"
#include "signaling.hpp"
#include "signaling_server.hpp"

namespace rtc {
class PeerConnection;
class Track;
class RtpPacketizationConfig;
class H265RtpPacketizer;
class RtpPacketizer;
}  // namespace rtc

namespace metashare {

struct WebRtcVideoFormat {
    int width = 0;
    int height = 0;
    int fps_num = 30;
    int fps_den = 1;
    proto::Codec codec = proto::Codec::kH265;
};

struct WebRtcAudioFormat {
    int sample_rate = 48000;
    int channels = 2;
    int bitrate_kbps = 96;
};

class WebRtcServer {
  public:
    WebRtcServer() = default;
    ~WebRtcServer();

    WebRtcServer(const WebRtcServer&) = delete;
    WebRtcServer& operator=(const WebRtcServer&) = delete;

    bool start(std::uint16_t signaling_port, std::string& err);
    void stop();

    // Format must be set before start() — it determines SDP codec prefs.
    void set_video_format(const WebRtcVideoFormat& v) { video_ = v; }
    void set_audio_system_format(const WebRtcAudioFormat& a) { audio_sys_ = a; }
    void set_audio_mic_format(const WebRtcAudioFormat& a) { audio_mic_ = a; }
    void set_enable_audio_system(bool b) { enable_sys_ = b; }
    void set_enable_audio_mic(bool b) { enable_mic_ = b; }

    // Send one encoded video access unit (HEVC NAL sequence, Annex B start
    // codes) to all peers. keyframe flag is informational (drives the RTP
    // marker bit) — the packetizer doesn't need to know.
    void broadcast_video(const std::uint8_t* data, std::size_t size,
                         std::int64_t pts_usec, bool keyframe);

    // Send one Opus packet to the system or mic audio track.
    // channel_id: 1 = system, 2 = mic (matches protocol.hpp channel ids).
    void broadcast_audio(std::uint32_t channel_id, const std::uint8_t* data,
                         std::size_t size, std::int64_t pts_usec);

    int peer_count() const;

  private:
    struct Peer;
    void on_signaling_message(const signal::Message& m);
    // Create a PeerConnection and wire up tracks based on the local format.
    std::shared_ptr<Peer> create_peer(const std::string& offer_sdp,
                                      std::string& answer_sdp);

    signal::Server signaling_;
    WebRtcVideoFormat video_{};
    WebRtcAudioFormat audio_sys_{};
    WebRtcAudioFormat audio_mic_{};
    bool enable_sys_ = false;
    bool enable_mic_ = false;

    mutable std::mutex peers_mu_;
    std::vector<std::shared_ptr<Peer>> peers_;

    // SSRCs are assigned sequentially per server instance.
    std::atomic<std::uint32_t> next_ssrc_{1};
};

}  // namespace metashare
