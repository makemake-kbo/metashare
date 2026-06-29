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
    std::mutex ice_mu;
    std::vector<signal::Message> pending_ice;
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
        // One client at a time: drop any existing peers so their pending
        // ICE candidates don't get sent to the new client (which would
        // confuse the answer/ICE ordering on the wire).
        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            for (auto& p : peers_) {
                if (p && p->pc) {
                    try { p->pc->close(); } catch (...) {}
                }
            }
            peers_.clear();
        }

        std::string answer_sdp;
        auto peer = create_peer(m.body, answer_sdp);
        if (!peer) {
            std::fprintf(stderr,
                         "[webrtc] failed to negotiate peer — dropping\n");
            signaling_.send({signal::Type::kBye, "", ""});
            return;
        }
        signal::Message ans{signal::Type::kAnswer, answer_sdp, ""};
        std::fprintf(stderr, "[webrtc] sending ANSWER (%zu bytes):\n%s\n",
                     answer_sdp.size(), answer_sdp.c_str());
        signaling_.send(ans);

        // Flush any ICE candidates that were buffered during negotiation.
        {
            std::vector<signal::Message> iced;
            {
                std::lock_guard<std::mutex> lk(peer->ice_mu);
                iced.swap(peer->pending_ice);
            }
            for (const auto& ice : iced) {
                std::fprintf(stderr, "[webrtc] flushing ICE: %s\n",
                             ice.body.c_str());
                signaling_.send(ice);
            }
        }

        std::lock_guard<std::mutex> lk(peers_mu_);
        peers_.push_back(std::move(peer));
        std::fprintf(stderr, "[webrtc] peer negotiated (%zu total)\n",
                     peers_.size());
    } else if (m.type == signal::Type::kIce) {
        // Trickle ICE: add the remote candidate to the current peer.
        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            if (!p || !p->pc) continue;
            try {
                rtc::Candidate c(m.body, m.mid);
                std::fprintf(stderr, "[webrtc] remote candidate: %s (mid=%s)\n",
                             m.body.c_str(), m.mid.c_str());
                p->pc->addRemoteCandidate(c);
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "[webrtc] bad remote candidate: %s\n",
                             e.what());
            }
        }
    } else if (m.type == signal::Type::kBye) {
        std::fprintf(stderr, "[webrtc] client disconnected\n");
        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            if (p && p->pc) {
                try { p->pc->close(); } catch (...) {}
            }
        }
        peers_.clear();
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
    std::vector<signal::Message> pending_ice;  // buffered, sent after ANSWER
    peer->pc->onLocalDescription([&](rtc::Description d) {
        {
            std::lock_guard<std::mutex> lk(ans_mu);
            answer_sdp = std::string(d);
            got_answer = true;
        }
        ans_cv.notify_one();
    });
    // Wire up ICE candidate callback BEFORE setRemoteDescription so we
    // don't miss candidates gathered during negotiation.  They're buffered
    // in peer->pending_ice and flushed after the ANSWER is sent.
    peer->pc->onLocalCandidate([peer](rtc::Candidate c) {
        std::fprintf(stderr, "[webrtc] local candidate: %s (mid=%s)\n",
                     std::string(c.candidate()).c_str(),
                     std::string(c.mid()).c_str());
        std::lock_guard<std::mutex> lk(peer->ice_mu);
        peer->pending_ice.push_back({signal::Type::kIce,
                                     std::string(c.candidate()),
                                     std::string(c.mid())});
    });
    peer->pc->onStateChange([](rtc::PeerConnection::State s) {
        std::fprintf(stderr, "[webrtc] pc state: %d\n",
                     static_cast<int>(s));
    });

    // Parse the offer to find the payload types the client assigned to our
    // codecs.  We MUST use the same PTs in the answer — remapping breaks
    // libwebrtc's RTP demux.
    int video_pt = -1;
    int opus_pt = -1;
    {
        rtc::Description offer(offer_sdp, rtc::Description::Type::Offer);
        for (int i = 0; i < offer.mediaCount(); ++i) {
            auto mv = offer.media(i);
            auto* mp = std::get_if<rtc::Description::Media*>(&mv);
            if (!mp || !*mp) continue;
            auto& media = **mp;
            if (media.type() == "video" && video_pt < 0) {
                const std::string want =
                    (video_.codec == proto::Codec::kH265) ? "H265" : "H264";
                for (int pt : media.payloadTypes()) {
                    auto* rm = media.rtpMap(pt);
                    if (rm && rm->format == want) {
                        video_pt = pt;
                        break;
                    }
                }
            } else if (media.type() == "audio" && opus_pt < 0) {
                for (int pt : media.payloadTypes()) {
                    auto* rm = media.rtpMap(pt);
                    if (rm && rm->format == "opus") {
                        opus_pt = pt;
                        break;
                    }
                }
            }
        }
    }
    if (video_pt < 0) {
        std::fprintf(stderr, "[webrtc] client offer has no %s codec — dropping\n",
                     video_.codec == proto::Codec::kH265 ? "H265" : "H264");
        return nullptr;
    }
    std::fprintf(stderr, "[webrtc] offer PTs: video=%d opus=%d\n",
                 video_pt, opus_pt);

    // Add local SENDONLY tracks BEFORE applying the remote offer.
    // libdatachannel matches them against the offer's m-sections by mid.
    {
        rtc::Description::Video v("0",
                                  rtc::Description::Direction::SendOnly);
        if (video_.codec == proto::Codec::kH265) {
            v.addH265Codec(video_pt);
        } else {
            v.addH264Codec(video_pt);
        }
        const std::uint32_t video_ssrc = next_ssrc_.fetch_add(1);
        v.addSSRC(video_ssrc, std::string("metashare-video"));
        auto track = peer->pc->addTrack(v);

        peer->video_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            video_ssrc, "metashare-video", video_pt, kVideoClockRate);

        std::shared_ptr<rtc::RtpPacketizer> packetizer;
        if (video_.codec == proto::Codec::kH265) {
            packetizer = std::make_shared<rtc::H265RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, peer->video_cfg);
        } else {
            packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, peer->video_cfg);
        }
        track->setMediaHandler(packetizer);

        // Chain RTCP handlers: NACK responder for retransmits,
        // SR reporter for timing sync (libwebrtc needs SR to play video).
        auto nack = std::make_shared<rtc::RtcpNackResponder>();
        auto sr = std::make_shared<rtc::RtcpSrReporter>(peer->video_cfg);
        packetizer->addToChain(nack);
        nack->addToChain(sr);

        peer->video_track = track;
    }

    if (enable_sys_ && opus_pt >= 0) {
        rtc::Description::Audio a("1",
                                  rtc::Description::Direction::SendOnly);
        a.addOpusCodec(opus_pt);
        const std::uint32_t ssrc = next_ssrc_.fetch_add(1);
        a.addSSRC(ssrc, std::string("metashare-audio"));
        auto track = peer->pc->addTrack(a);

        peer->audio_sys_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "metashare-audio-sys", opus_pt, kOpusClockRate);
        auto packetizer =
            std::make_shared<rtc::RtpPacketizer>(peer->audio_sys_cfg);
        track->setMediaHandler(packetizer);
        peer->audio_sys_track = track;
    }
    if (enable_mic_) {
        rtc::Description::Audio a("2",
                                  rtc::Description::Direction::SendOnly);
        a.addOpusCodec(kOpusPayloadTypeMic);
        const std::uint32_t ssrc = next_ssrc_.fetch_add(1);
        a.addSSRC(ssrc, std::string("metashare-audio"));
        auto track = peer->pc->addTrack(a);

        peer->audio_mic_cfg = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "metashare-audio-mic", kOpusPayloadTypeMic, kOpusClockRate);
        auto packetizer =
            std::make_shared<rtc::RtpPacketizer>(peer->audio_mic_cfg);
        track->setMediaHandler(packetizer);
        peer->audio_mic_track = track;
    }

    // Apply the offer — this triggers answer generation onLocalDescription.
    std::fprintf(stderr, "[webrtc] OFFER (%zu bytes):\n%s\n",
                 offer_sdp.size(), offer_sdp.c_str());
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

    // Post-process: libdatachannel generates bare "a=ssrc:N" without cname,
    // which libwebrtc's SDP parser may reject. Add a cname attribute.
    {
        const std::string tag = "a=ssrc:";
        std::string::size_type pos = 0;
        int fixes = 0;
        while ((pos = answer_sdp.find(tag, pos)) != std::string::npos) {
            auto eol = answer_sdp.find('\n', pos);
            if (eol == std::string::npos) break;
            auto line_end = eol;
            if (line_end > pos && answer_sdp[line_end - 1] == '\r') line_end--;
            std::string line(answer_sdp, pos, line_end - pos);
            if (line.find(' ') == std::string::npos) {
                std::string replacement = line + " cname:metashare";
                answer_sdp.replace(pos, line_end - pos, replacement);
                pos += replacement.size();
                fixes++;
            } else {
                pos = eol + 1;
            }
        }
        std::fprintf(stderr, "[webrtc] sdp fixup: added cname to %d ssrc lines\n", fixes);
    }

    return peer;
}

void WebRtcServer::broadcast_video(const std::uint8_t* data, std::size_t size,
                                   std::int64_t pts_usec, bool keyframe) {
    (void)keyframe;
    std::lock_guard<std::mutex> lk(peers_mu_);
    if (peers_.empty()) return;
    static std::atomic<int> vcount{0};
    int n = vcount.fetch_add(1);
    if (n % 30 == 0) {
        std::fprintf(stderr, "[webrtc] video frame #%d (%zu bytes) peers=%zu\n",
                     n, size, peers_.size());
    }
    rtc::binary frame(reinterpret_cast<const rtc::byte*>(data),
                      reinterpret_cast<const rtc::byte*>(data + size));
    for (auto& p : peers_) {
        if (p && p->video_track && p->video_track->isOpen()) {
            if (n % 30 == 0) {
                std::fprintf(stderr, "[webrtc] sending video to peer (track open)\n");
            }
            if (p->video_cfg) {
                p->video_cfg->timestamp = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(pts_usec) * kVideoClockRate) /
                    1'000'000);
            }
            try {
                p->video_track->send(frame);
            } catch (const std::exception& e) {
                static std::atomic<int> ecount{0};
                int en = ecount.fetch_add(1);
                if (en < 5)
                    std::fprintf(stderr,
                                 "[webrtc] video send error #%d: %s\n",
                                 en, e.what());
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
