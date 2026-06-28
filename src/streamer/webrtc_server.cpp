#include "webrtc_server.hpp"

#include <rtc/candidate.hpp>
#include <rtc/configuration.hpp>
#include <rtc/description.hpp>
#include <rtc/global.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/h265rtppacketizer.hpp>
#include <rtc/nalunit.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtp.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>
#include <rtc/track.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace metashare {

namespace {

constexpr bool kH265 = true;  // set false at build time if needed later

constexpr std::uint32_t kVideoClockRate = 90000;  // Hz
constexpr std::uint32_t kOpusClockRate = 48000;   // Hz
constexpr int kVideoPayloadType = 96;             // dynamic
constexpr int kOpusPayloadTypeSys = 111;          // dynamic, convention
constexpr int kOpusPayloadTypeMic = 112;          // dynamic

// One-time libdatachannel init.
std::once_flag g_rtc_init_once;
void init_rtc() {
    std::call_once(g_rtc_init_once,
                   [] { rtc::InitLogger(rtc::LogLevel::Warning); });
}

}  // namespace

struct WebRtcServer::Peer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_sys_track;
    std::shared_ptr<rtc::Track> audio_mic_track;
    std::shared_ptr<rtc::RtpPacketizationConfig> video_cfg;
    std::shared_ptr<rtc::RtpPacketizationConfig> audio_sys_cfg;
    std::shared_ptr<rtc::RtpPacketizationConfig> audio_mic_cfg;
};

WebRtcServer::~WebRtcServer() { stop(); }

bool WebRtcServer::start(std::uint16_t signaling_port, std::string& err) {
    init_rtc();
    if (!signaling_.start(
            signaling_port,
            [this](const signal::Message& m) { on_signaling_message(m); }, err))
        return false;
    std::fprintf(stderr, "[webrtc] signaling on tcp/%u\n", signaling_port);
    return true;
}

void WebRtcServer::stop() {
    signaling_.stop();
    std::lock_guard<std::mutex> lk(peers_mu_);
    for (auto& p : peers_) {
        if (p && p->pc) p->pc->close();
    }
    peers_.clear();
}

int WebRtcServer::peer_count() const {
    std::lock_guard<std::mutex> lk(peers_mu_);
    return static_cast<int>(peers_.size());
}

void WebRtcServer::on_signaling_message(const signal::Message& m) {
    if (m.type == signal::Type::kOffer) {
        std::string answer_sdp;
        auto peer = create_peer(m.body, answer_sdp);
        if (!peer) {
            std::fprintf(stderr,
                         "[webrtc] failed to negotiate peer — dropping\n");
            signaling_.send({signal::Type::kBye, "", ""});
            return;
        }
        signal::Message ans{signal::Type::kAnswer, answer_sdp, ""};
        signaling_.send(ans);

        std::lock_guard<std::mutex> lk(peers_mu_);
        peers_.push_back(std::move(peer));
        std::fprintf(stderr, "[webrtc] peer negotiated (%zu total)\n",
                     peers_.size());
    } else if (m.type == signal::Type::kBye) {
        std::fprintf(stderr, "[webrtc] client disconnected\n");
        // The PeerConnection will be cleaned up when its state transitions
        // to Failed/Closed; we proactively drop dead peers on the next
        // broadcast sweep.
    }
}

std::shared_ptr<WebRtcServer::Peer>
WebRtcServer::create_peer(const std::string& offer_sdp,
                          std::string& answer_sdp) {
    rtc::Configuration config;
    // LAN-only — no STUN/TURN. ICE still runs over UDP on the local network.
    config.iceServers.clear();

    auto peer = std::make_shared<Peer>();
    peer->pc = std::make_shared<rtc::PeerConnection>(config);

    // Capture answer SDP synchronously. libdatachannel fires
    // onLocalDescription asynchronously after setRemoteDescription(offer).
    std::mutex ans_mu;
    std::condition_variable ans_cv;
    bool got_answer = false;

    peer->pc->onLocalDescription([&](rtc::Description d) {
        {
            std::lock_guard<std::mutex> lk(ans_mu);
            answer_sdp = std::string(d);
            got_answer = true;
        }
        ans_cv.notify_one();
    });
    peer->pc->onLocalCandidate([this](rtc::Candidate c) {
        signal::Message m{signal::Type::kIce, std::string(c.candidate()),
                          std::string(c.mid())};
        signaling_.send(m);
    });
    peer->pc->onStateChange([](rtc::PeerConnection::State s) {
        std::fprintf(stderr, "[webrtc] pc state: %d\n", static_cast<int>(s));
    });

    // Add local SENDONLY tracks BEFORE applying the remote offer.
    // libdatachannel matches them against the offer's m-sections by mid.
    {
        rtc::Description::Video v("video",
                                  rtc::Description::Direction::SendOnly);
        if (video_.codec == proto::Codec::kH265) {
            v.addH265Codec(kVideoPayloadType);
        } else {
            v.addH264Codec(kVideoPayloadType);
        }
        const std::uint32_t video_ssrc = next_ssrc_.fetch_add(1);
        v.addSSRC(video_ssrc, std::nullopt);
        auto track = peer->pc->addTrack(v);

        peer->video_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            video_ssrc, "metashare-video", kVideoPayloadType, kVideoClockRate);

        std::shared_ptr<rtc::RtpPacketizer> packetizer;
        if (video_.codec == proto::Codec::kH265) {
            packetizer = std::make_shared<rtc::H265RtpPacketizer>(
                rtc::NalUnit::Separator::LongStartSequence, peer->video_cfg);
        } else {
            packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::LongStartSequence, peer->video_cfg);
        }
        track->setMediaHandler(packetizer);
        peer->video_track = track;
    }

    if (enable_sys_) {
        rtc::Description::Audio a("audio-sys",
                                  rtc::Description::Direction::SendOnly);
        a.addOpusCodec(kOpusPayloadTypeSys);
        const std::uint32_t ssrc = next_ssrc_.fetch_add(1);
        a.addSSRC(ssrc, std::nullopt);
        auto track = peer->pc->addTrack(a);

        peer->audio_sys_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "metashare-audio-sys", kOpusPayloadTypeSys, kOpusClockRate);
        auto packetizer =
            std::make_shared<rtc::RtpPacketizer>(peer->audio_sys_cfg);
        track->setMediaHandler(packetizer);
        peer->audio_sys_track = track;
    }
    if (enable_mic_) {
        rtc::Description::Audio a("audio-mic",
                                  rtc::Description::Direction::SendOnly);
        a.addOpusCodec(kOpusPayloadTypeMic);
        const std::uint32_t ssrc = next_ssrc_.fetch_add(1);
        a.addSSRC(ssrc, std::nullopt);
        auto track = peer->pc->addTrack(a);

        peer->audio_mic_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "metashare-audio-mic", kOpusPayloadTypeMic, kOpusClockRate);
        auto packetizer =
            std::make_shared<rtc::RtpPacketizer>(peer->audio_mic_cfg);
        track->setMediaHandler(packetizer);
        peer->audio_mic_track = track;
    }

    // Apply the offer — this triggers answer generation onLocalDescription.
    peer->pc->setRemoteDescription(
        rtc::Description(offer_sdp, rtc::Description::Type::Offer));

    // Wait for the answer (with a generous timeout for slow ICE gathers).
    {
        std::unique_lock<std::mutex> lk(ans_mu);
        if (!ans_cv.wait_for(lk, std::chrono::seconds(5),
                             [&] { return got_answer; })) {
            std::fprintf(stderr, "[webrtc] timed out waiting for answer\n");
            return nullptr;
        }
    }
    return peer;
}

void WebRtcServer::broadcast_video(const std::uint8_t* data, std::size_t size,
                                   std::int64_t pts_usec, bool keyframe) {
    (void)keyframe;
    std::lock_guard<std::mutex> lk(peers_mu_);
    if (peers_.empty()) return;
    rtc::binary frame(reinterpret_cast<const rtc::byte*>(data),
                      reinterpret_cast<const rtc::byte*>(data + size));
    for (auto& p : peers_) {
        if (p && p->video_track && p->video_track->isOpen()) {
            if (p->video_cfg) {
                p->video_cfg->timestamp = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(pts_usec) * kVideoClockRate) /
                    1'000'000);
            }
            try {
                p->video_track->send(frame);
            } catch (const std::exception& e) {
                // Peer gone or send failed; will be swept on next iteration.
            }
        }
    }
}

void WebRtcServer::broadcast_audio(std::uint32_t channel_id,
                                   const std::uint8_t* data, std::size_t size,
                                   std::int64_t pts_usec) {
    std::lock_guard<std::mutex> lk(peers_mu_);
    if (peers_.empty()) return;
    rtc::binary packet(reinterpret_cast<const rtc::byte*>(data),
                       reinterpret_cast<const rtc::byte*>(data + size));
    for (auto& p : peers_) {
        std::shared_ptr<rtc::Track> t;
        std::shared_ptr<rtc::RtpPacketizationConfig> cfg;
        if (channel_id == 1) {  // kChannelAudioSystem
            t = p->audio_sys_track;
            cfg = p->audio_sys_cfg;
        } else if (channel_id == 2) {  // kChannelAudioMic
            t = p->audio_mic_track;
            cfg = p->audio_mic_cfg;
        }
        if (!t || !t->isOpen()) continue;
        if (cfg) {
            cfg->timestamp = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(pts_usec) * kOpusClockRate) /
                1'000'000);
        }
        try {
            t->send(packet);
        } catch (const std::exception& e) {
            // Peer gone or send failed; will be swept on next iteration.
        }
    }
}

}  // namespace metashare
