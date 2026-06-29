// MetaShare desktop test client.
//
// Mirrors what the Quest client must do, minus OpenXR: discover the streamer
// on the LAN (or use a host given on the CLI), negotiate a WebRTC connection
// over our tiny TCP signaling protocol, decode the H.265 video stream, and
// play the Opus audio. Used to validate the streamer end-to-end without Quest
// hardware.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include <SDL2/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <rtc/candidate.hpp>
#include <rtc/configuration.hpp>
#include <rtc/description.hpp>
#include <rtc/global.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/nalunit.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtp.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/track.hpp>

#include "base64.hpp"
#include "protocol.hpp"
#include "signaling.hpp"
#include "signaling_client.hpp"

using namespace metashare;

namespace {

// ---------------------------------------------------------------------------
//  Opus decoder + SDL_audio playback for one audio channel
// ---------------------------------------------------------------------------
//
// libavcodec's libopus decoder may produce AV_SAMPLE_FMT_FLTP (planar float)
// or AV_SAMPLE_FMT_S16 (interleaved) depending on the ffmpeg build. We use
// libswresample to convert whatever it gives us into interleaved s16 at the
// streamer's announced rate/channels, then hand the bytes to SDL_QueueAudio.
struct AudioPlayer {
    bool init(int sample_rate, int channels, std::string& err);
    void enqueue_pcm(const std::int16_t* data, std::size_t sample_count);
    void close();

    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    int out_channels = 0;
    SDL_AudioDeviceID dev = 0;
};

bool AudioPlayer::init(int sample_rate, int channels, std::string& err) {
    out_channels = channels;
    const AVCodec* codec = avcodec_find_decoder_by_name("libopus");
    if (!codec) {
        err = "libopus decoder not available in this ffmpeg build";
        return false;
    }
    dec = avcodec_alloc_context3(codec);
    if (!dec) return false;
    dec->sample_rate = sample_rate;
    av_channel_layout_default(&dec->ch_layout, channels);
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        return false;
    }
    pkt = av_packet_alloc();
    frame = av_frame_alloc();

    SDL_AudioSpec want{}, have{};
    want.freq = sample_rate;
    want.format = AUDIO_S16LSB;
    want.channels = static_cast<Uint8>(channels);
    want.samples = 960;
    want.callback = nullptr;
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                              SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (!dev) {
        err = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
        return false;
    }
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void AudioPlayer::enqueue_pcm(const std::int16_t* data,
                              std::size_t sample_count) {
    if (!dev) return;
    SDL_QueueAudio(dev, data,
                   static_cast<Uint32>(sample_count * sizeof(std::int16_t)));
}

void AudioPlayer::close() {
    if (dev) {
        SDL_CloseAudioDevice(dev);
        dev = 0;
    }
    if (swr) swr_free(&swr);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
}

static bool ensure_swr(AudioPlayer& p, std::string& err) {
    if (p.swr && p.frame->format == AV_SAMPLE_FMT_S16 &&
        p.frame->ch_layout.nb_channels == p.out_channels &&
        p.frame->sample_rate == p.dec->sample_rate) {
        return true;
    }
    if (p.swr) swr_free(&p.swr);
    AVChannelLayout in_layout = {}, out_layout = {};
    av_channel_layout_default(&in_layout, p.frame->ch_layout.nb_channels);
    av_channel_layout_default(&out_layout, p.out_channels);
    int rc = swr_alloc_set_opts2(&p.swr, &out_layout, AV_SAMPLE_FMT_S16,
                                 p.dec->sample_rate, &in_layout,
                                 static_cast<AVSampleFormat>(p.frame->format),
                                 p.frame->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (rc < 0 || (rc = swr_init(p.swr)) < 0) {
        err = "swr init failed";
        return false;
    }
    return true;
}

void feed_opus(AudioPlayer& p, const std::uint8_t* data, int size) {
    if (!p.dec) return;
    p.pkt->data = const_cast<std::uint8_t*>(data);
    p.pkt->size = size;
    if (avcodec_send_packet(p.dec, p.pkt) < 0) return;
    while (avcodec_receive_frame(p.dec, p.frame) == 0) {
        std::string err;
        if (!ensure_swr(p, err)) continue;
        const int out_max =
            swr_get_out_samples(p.swr, p.frame->nb_samples) + 256;
        std::vector<std::int16_t> pcm(
            static_cast<std::size_t>(std::max(0, out_max)) * p.out_channels);
        std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(pcm.data());
        int converted = swr_convert(
            p.swr, &dst, p.frame->nb_samples,
            const_cast<const std::uint8_t**>(p.frame->extended_data),
            p.frame->nb_samples);
        if (converted < 0) continue;
        pcm.resize(static_cast<std::size_t>(converted) * p.out_channels);
        p.enqueue_pcm(pcm.data(), pcm.size());
    }
}

// ---------------------------------------------------------------------------
//  HEVC/H.264 video decoder + frame handoff to the SDL render thread
// ---------------------------------------------------------------------------
struct VideoDecoder {
    bool init(bool hevc, int width, int height, std::string& err);
    void feed(const std::uint8_t* data, int size);
    void close();

    // Latest decoded frame, swapped under lock so the SDL main thread can
    // pick it up at its own cadence.
    std::mutex frame_mu_;
    AVFrame* latest_ = nullptr;  // protected by frame_mu_

    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
};

bool VideoDecoder::init(bool hevc, int /*width*/, int /*height*/,
                        std::string& err) {
    const AVCodec* codec =
        avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) {
        err =
            hevc ? "HEVC decoder not available" : "H.264 decoder not available";
        return false;
    }
    dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_open2(dec, codec, nullptr) < 0) {
        err = "video decoder open failed";
        avcodec_free_context(&dec);
        return false;
    }
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    latest_ = av_frame_alloc();
    return true;
}

void VideoDecoder::feed(const std::uint8_t* data, int size) {
    if (!dec) return;
    pkt->data = const_cast<std::uint8_t*>(data);
    pkt->size = size;
    if (avcodec_send_packet(dec, pkt) < 0) return;
    while (avcodec_receive_frame(dec, frame) == 0) {
        std::lock_guard<std::mutex> lk(frame_mu_);
        av_frame_unref(latest_);
        av_frame_ref(latest_, frame);
    }
}

void VideoDecoder::close() {
    if (latest_) {
        std::lock_guard<std::mutex> lk(frame_mu_);
        av_frame_free(&latest_);
    }
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
}

// ---------------------------------------------------------------------------
//  Discovery
// ---------------------------------------------------------------------------
bool discover(std::string& host, std::uint16_t& port, int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000L};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    proto::DiscoveryProbe probe{};
    std::memcpy(probe.magic, proto::kDiscoveryMagic, sizeof(probe.magic));
    probe.version = proto::kProtocolVersion;
    probe.client_caps = proto::kCapH264 | proto::kCapH265;

    std::fprintf(stderr, "[client] broadcasting discovery probe...\n");
    auto send_to = [&](in_addr_t ip) {
        sockaddr_in d{};
        d.sin_family = AF_INET;
        d.sin_addr.s_addr = ip;
        d.sin_port = htons(proto::kDiscoveryPort);
        ::sendto(fd, &probe, sizeof(probe), 0, reinterpret_cast<sockaddr*>(&d),
                 sizeof(d));
    };
    send_to(htonl(INADDR_BROADCAST));
    send_to(htonl(INADDR_LOOPBACK));
    ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) == 0) {
        for (ifaddrs* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_BROADCAST) ||
                (p->ifa_flags & IFF_LOOPBACK))
                continue;
            if (p->ifa_broadaddr)
                send_to(reinterpret_cast<sockaddr_in*>(p->ifa_broadaddr)
                            ->sin_addr.s_addr);
        }
        ::freeifaddrs(ifa);
    }

    proto::DiscoveryOffer offer{};
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    ssize_t n = ::recvfrom(fd, &offer, sizeof(offer), 0,
                           reinterpret_cast<sockaddr*>(&from), &flen);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(offer))) return false;
    if (std::memcmp(offer.magic, proto::kDiscoveryMagic, sizeof(offer.magic)) !=
        0)
        return false;

    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    host = ip;
    port = offer.signaling_port;
    std::fprintf(stderr, "[client] found '%s' at %s:%u (signaling)\n",
                 offer.host_name, ip, static_cast<unsigned>(port));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host;
    std::uint16_t port = signal::kDefaultSignalingPort;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                         "Usage: %s [--host <ip>] [--port <n>]\n"
                         "  With no --host, discovers the streamer via UDP.\n",
                         argv[0]);
            return 0;
        }
    }

    if (host.empty()) {
        if (!discover(host, port, 3000)) {
            std::fprintf(stderr,
                         "[client] discovery failed; pass --host <ip>\n");
            return 1;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[client] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // Placeholder window — resized once the stream's actual size is known
    // (from the first decoded frame).
    SDL_Window* win = SDL_CreateWindow("MetaShare", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1280, 720,
                                       SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = nullptr;

    VideoDecoder video;
    AudioPlayer audio;
    std::atomic<bool> audio_inited{false};
    std::atomic<int> stream_width{0}, stream_height{0};

    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Configuration rtc_cfg;
    rtc_cfg.iceServers.clear();
    auto pc = std::make_shared<rtc::PeerConnection>(rtc_cfg);

    std::atomic<bool> got_answer{false};
    std::mutex answer_mu;
    std::condition_variable answer_cv;

    std::string offer_sdp;
    std::mutex offer_mu;
    std::condition_variable offer_cv;
    bool offer_ready = false;

    pc->onLocalDescription([&](rtc::Description d) {
        std::fprintf(stderr, "[client] local description (%s)\n",
                     d.typeString().c_str());
        if (d.type() == rtc::Description::Type::Offer) {
            std::lock_guard<std::mutex> lk(offer_mu);
            offer_sdp = std::string(d);
            offer_ready = true;
            offer_cv.notify_one();
        }
    });
    pc->onLocalCandidate([&](rtc::Candidate c) {
        std::fprintf(stderr, "[client] local ICE: %s (mid=%s)\n",
                     c.candidate().c_str(), c.mid().c_str());
    });
    pc->onStateChange([](rtc::PeerConnection::State s) {
        std::fprintf(stderr, "[client] pc state: %d\n", static_cast<int>(s));
    });

    // libdatachannel's PeerConnection stores only weak_ptr<Track> internally,
    // so we must hold strong refs to every track we create for as long as we
    // want it to survive.
    std::vector<std::shared_ptr<rtc::Track>> keep_alive;

    // Add RECVONLY video track. We parse H.265 RTP packets manually in the
    // onMessage callback (libdatachannel's H265RtpDepacketizer has been
    // unreliable in testing). The wire format is RFC 7798:
    //   * Single NAL Unit Packet (type < 48)  — payload IS the NAL.
    //   * Fragmentation Unit (type 49)        — payload is NAL hdr (2 bytes)
    //                                            + FU header (1 byte:
    //                                            S,E,FuType)
    //                                            + NAL fragment data.
    //   * Aggregation Packet (type 48)        — multiple NALs in one packet.
    // We reassemble FUs into full NALs and feed each to the decoder as soon
    // as it's complete (start-code prefix + NAL bytes).
    {
        rtc::Description::Video v("0", rtc::Description::Direction::RecvOnly);
        v.addH265Codec(96);
        auto track = pc->addTrack(v);
        // No media handler — we want raw RTP packets.
        auto session = std::make_shared<rtc::RtcpReceivingSession>();
        track->setMediaHandler(session);

        // Persistent reassembly state — lives as long as the track.
        auto fu_buf = std::make_shared<std::vector<std::uint8_t>>();
        track->onMessage(
            [fu_buf, &video](rtc::binary msg) {
                if (msg.size() < 12) return;
                const auto* base =
                    reinterpret_cast<const std::uint8_t*>(msg.data());
                const auto* rh =
                    reinterpret_cast<const rtc::RtpHeader*>(msg.data());
                std::size_t off = rh->getSize();
                if (msg.size() <= off) return;
                const std::size_t psize = msg.size() - off;
                const auto* p = base + off;
                if (psize < 2) return;
                const unsigned nalu_type = (p[0] >> 1) & 0x3F;

                if (nalu_type == 49) {
                    // FU. 3rd byte is FU header: S(7) E(6) Type(5-0).
                    if (psize < 3) return;
                    const bool fu_s = (p[2] & 0x80) != 0;
                    const bool fu_e = (p[2] & 0x40) != 0;
                    const unsigned orig_type = p[2] & 0x3F;
                    if (fu_s) {
                        // Start a new NAL: Annex B start code + reassembled
                        // 2-byte NAL header (replace type bits with orig).
                        fu_buf->clear();
                        fu_buf->push_back(0);
                        fu_buf->push_back(0);
                        fu_buf->push_back(0);
                        fu_buf->push_back(1);
                        // Reconstruct NAL header from FU indicator + type.
                        std::uint8_t h0 = (p[0] & 0x81) | (orig_type << 1);
                        fu_buf->push_back(h0);
                        fu_buf->push_back(p[1]);
                    }
                    // Append fragment body (skip 3-byte FU hdr).
                    fu_buf->insert(fu_buf->end(), p + 3, p + psize);
                    if (fu_e) {
                        video.feed(fu_buf->data(),
                                   static_cast<int>(fu_buf->size()));
                        fu_buf->clear();
                    }
                } else if (nalu_type < 48) {
                    // Single NAL Unit Packet.
                    static const std::uint8_t sc[] = {0, 0, 0, 1};
                    std::vector<std::uint8_t> frame;
                    frame.insert(frame.end(), sc, sc + 4);
                    frame.insert(frame.end(), p, p + psize);
                    video.feed(frame.data(), static_cast<int>(frame.size()));
                }
                // Type 48 (AP) and >= 48 reserved: ignored for now.
            },
            nullptr);
        keep_alive.push_back(track);
    }
    // RECVONLY Opus audio track (system audio). Opus packets fit in a single
    // RTP packet, so no depacketizer is needed — we parse the RTP header
    // inline to pull the payload.
    {
        rtc::Description::Audio a("1", rtc::Description::Direction::RecvOnly);
        a.addOpusCodec(111);
        auto track = pc->addTrack(a);
        track->onMessage(
            [&](rtc::binary msg) {
                if (msg.size() < 12) return;
                const auto* h =
                    reinterpret_cast<const rtc::RtpHeader*>(msg.data());
                std::size_t payload_off = h->getSize();
                if (msg.size() <= payload_off) return;
                if (!audio_inited.load()) {
                    std::string aerr;
                    if (audio.init(48000, 2, aerr)) audio_inited = true;
                }
                feed_opus(audio,
                          reinterpret_cast<const std::uint8_t*>(msg.data()) +
                              payload_off,
                          static_cast<int>(msg.size() - payload_off));
            },
            nullptr);
        keep_alive.push_back(track);
    }

    pc->setLocalDescription();

    // Hook the signaling client.
    signal::Client sig;
    std::string sig_err;

    // Wait briefly for the local offer SDP (synchronous in practice since
    // libdatachannel fires onLocalDescription synchronously when no ICE
    // gathering is needed for the initial m-section construction).
    {
        std::unique_lock<std::mutex> lk(offer_mu);
        if (!offer_cv.wait_for(lk, std::chrono::seconds(5),
                               [&] { return offer_ready; })) {
            std::fprintf(stderr, "[client] failed to gather local offer\n");
            return 1;
        }
    }

    if (!sig.connect(
            host, port,
            [&](const signal::Message& m) {
                if (m.type == signal::Type::kAnswer) {
                    pc->setRemoteDescription(rtc::Description(
                        m.body, rtc::Description::Type::Answer));
                    got_answer = true;
                    answer_cv.notify_one();
                } else if (m.type == signal::Type::kIce) {
                    try {
                        pc->addRemoteCandidate(rtc::Candidate(m.body, m.mid));
                    } catch (const std::exception& e) {
                        // Malformed candidate — libdatachannel throws on bad
                        // syntax. Ignore and wait for the next one.
                    }
                }
            },
            sig_err)) {
        std::fprintf(stderr, "[client] signaling connect failed: %s\n",
                     sig_err.c_str());
        return 1;
    }

    // Send our offer, then pump local ICE candidates to the server via
    // signaling (the callback was set above; we re-bind it now that sig is
    // alive).
    pc->onLocalCandidate([&](rtc::Candidate c) {
        sig.send({signal::Type::kIce, std::string(c.candidate()),
                  std::string(c.mid())});
    });

    sig.send({signal::Type::kOffer, offer_sdp, ""});

    // Wait for the answer.
    {
        std::unique_lock<std::mutex> lk(answer_mu);
        if (!answer_cv.wait_for(lk, std::chrono::seconds(10),
                                [&] { return got_answer.load(); })) {
            std::fprintf(stderr, "[client] timed out waiting for answer\n");
            return 1;
        }
    }

    // Main render loop.
    std::fprintf(stderr, "[client] connected — rendering\n");
    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                quit = true;
        }

        // Lazy-create the SDL texture on first frame.
        {
            std::lock_guard<std::mutex> lk(video.frame_mu_);
            if (video.latest_ && video.latest_->data[0]) {
                int w = video.latest_->width;
                int h = video.latest_->height;
                if (w != stream_width.load() || h != stream_height.load()) {
                    stream_width = w;
                    stream_height = h;
                    if (tex) SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV,
                                            SDL_TEXTUREACCESS_STREAMING, w, h);
                    SDL_SetWindowSize(win, w, h);
                }
                if (tex) {
                    SDL_UpdateYUVTexture(
                        tex, nullptr, video.latest_->data[0],
                        video.latest_->linesize[0], video.latest_->data[1],
                        video.latest_->linesize[1], video.latest_->data[2],
                        video.latest_->linesize[2]);
                    SDL_RenderClear(ren);
                    SDL_RenderCopy(ren, tex, nullptr, nullptr);
                    SDL_RenderPresent(ren);
                }
            }
        }

        // Don't busy-loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::fprintf(stderr, "[client] stream closed\n");
    sig.disconnect();
    audio.close();
    video.close();
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
