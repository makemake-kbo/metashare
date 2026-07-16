// Raw RTP server: accepts a TCP signaling connection (HELLO/READY/START/BYE),
// then streams H.265/H.264 video + Opus audio as raw RTP over UDP to the
// client's announced port. Replaces WebRtcServer — no libdatachannel, no SDP,
// no ICE. One UDP port carries both media, demultiplexed by SSRC.
//
// One RtpServer per monitor pipeline. Same broadcast API surface as the old
// WebRtcServer so main.cpp only swaps the type.
//
// NACK retransmission: a background thread reads RTCP NACKs from the UDP socket
// and retransmits requested packets from a sliding-window buffer.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <netinet/in.h>

#include "input_event.hpp"
#include "pacer.hpp"
#include "protocol.hpp"
#include "rtp_packetizer.hpp"
#include "signaling.hpp"
#include "signaling_server.hpp"

namespace metashare {

struct VideoFormat {
    int width = 0;
    int height = 0;
    int fps_num = 30;
    int fps_den = 1;
    proto::Codec codec = proto::Codec::kH265;
};

class RtpServer {
  public:
    RtpServer();
    ~RtpServer();

    RtpServer(const RtpServer&) = delete;
    RtpServer& operator=(const RtpServer&) = delete;

    bool start(std::uint16_t signaling_port, std::string& err);
    void stop();

    // Human-readable id (the monitor index) used only to tag the 1 Hz stats
    // line, so per-monitor loss/retransmit rates are easy to compare in the log.
    void set_id(int id) { id_ = id; }

    // Format must be set before start() — it picks the packetizer and feeds
    // the HELLO JSON.
    void set_video_format(const VideoFormat& v) { video_ = v; }
    void set_audio_format(int sample_rate, int channels) {
        audio_rate_ = sample_rate;
        audio_channels_ = channels;
    }

    // Send one encoded video access unit (Annex B start codes) to all clients.
    void broadcast_video(const std::uint8_t* data, std::size_t size,
                         std::int64_t pts_usec, bool keyframe);

    // Send one Opus packet. channel_id is accepted for source compatibility
    // but collapsed onto the single audio SSRC (multi-channel audio is not
    // supported by the raw-RTP transport yet).
    void broadcast_audio(std::uint32_t channel_id, const std::uint8_t* data,
                         std::size_t size, std::int64_t pts_usec);

    int peer_count() const;

    // Optional host-wide pacer; when set, video sends (including
    // retransmissions) are throttled through it. Must outlive this server.
    void set_pacer(Pacer* pacer) { pacer_ = pacer; }

    // Fired when a client sends a PLI / keyframe request. Wire this to
    // Encoder::force_keyframe() from the owning pipeline.
    std::function<void()> on_keyframe_request;

    // Fired (from the RTCP thread, ~1/s) with the video fraction-lost from
    // the client's Receiver Reports, 0.0..1.0. Drives bitrate adaptation.
    std::function<void(float)> on_loss_report;

    // Fired (from the signaling thread) for each decoded INPUT event from the
    // client. Left unset when the pipeline's source has no input backend, in
    // which case client input is silently dropped.
    std::function<void(const input::Event&)> on_input;

  private:
    void on_connect(const sockaddr_in& peer);
    void on_message(const signal::Message& m);
    void send_hello();
    // Open the UDP socket (once) and start the NACK receiver thread.
    bool open_udp(std::string& err);
    void nack_loop();
    // Drains send_q_ and writes packets to the socket at the paced rate. Runs
    // on its own thread so the token-bucket sleep never blocks the capture
    // thread that calls broadcast_video().
    void sender_loop();
    void handle_rtcp(const std::uint8_t* data, std::size_t size,
                     const sockaddr_in& from);
    // Build + send an RTCP Sender Report for one SSRC to the client.
    void send_sender_report(std::uint32_t ssrc, std::uint64_t packets,
                            std::uint64_t octets, std::uint32_t last_rtp_ts);

    signal::Server signaling_;
    Pacer* pacer_ = nullptr;
    VideoFormat video_{};
    int audio_rate_ = 48000;
    int audio_channels_ = 2;

    // SSRCs for this server's video/audio streams (advertised in HELLO).
    std::uint32_t video_ssrc_ = 0;
    std::uint32_t audio_ssrc_ = 0;

    int udp_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread nack_thread_;

    // Dedicated send path. broadcast_video() packetizes on the capture thread
    // and pushes onto send_q_; sender_thread_ drains it and runs the pacing
    // sleep, so an IDR burst delays only the wire, never the next capture tick.
    std::thread sender_thread_;
    std::mutex send_mu_;
    std::condition_variable send_cv_;
    std::deque<rtp::Packet> send_q_;
    std::atomic<std::uint64_t> send_drops_{0};  // packets shed on queue overflow

    mutable std::mutex peer_mu_;
    bool peer_streaming_ = false;
    sockaddr_in peer_udp_{};  // where to send RTP (client ip:port)

    std::unique_ptr<rtp::PacketizerBase> video_packer_;
    std::unique_ptr<rtp::OpusRtpPacketizer> audio_packer_;
    // Separate sliding windows per SSRC (video and audio have independent
    // sequence-number spaces, so a NACK's media SSRC selects the buffer).
    rtp::RetransmitBuffer video_retx_{1024};
    rtp::RetransmitBuffer audio_retx_{512};

    // Sender Report accounting + last-stamped RTP timestamp (per SSRC).
    std::atomic<std::uint64_t> video_pkts_{0};
    std::atomic<std::uint64_t> video_octets_{0};
    std::atomic<std::uint64_t> audio_pkts_{0};
    std::atomic<std::uint64_t> audio_octets_{0};
    std::atomic<std::uint32_t> video_last_ts_{0};
    std::atomic<std::uint32_t> audio_last_ts_{0};

    // Diagnostics: monitor id, retransmitted-packet count, and the last
    // fraction-lost the client reported. Printed once per second by nack_loop().
    int id_ = 0;
    std::atomic<std::uint64_t> video_retx_count_{0};
    std::atomic<float> last_loss_{0.0f};
};

}  // namespace metashare
