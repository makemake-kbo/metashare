// MetaShare streamer CLI.
//
// Pipeline (per monitor):
//   FrameSource -> Encoder (HEVC HW, H.264 SW fallback) -> RtpServer (UDP)
// Multiple monitors run as parallel pipelines, each on its own signaling port
// (base, base+1, …) and its own capture thread.
// DiscoveryResponder (UDP) lets clients find us with no config.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "discovery.hpp"
#include "encoder.hpp"
#include "frame_source.hpp"
#include "input_event.hpp"
#include "input_sink.hpp"
#include "pacer.hpp"
#include "rtp_server.hpp"
#include "signaling.hpp"

#include <mutex>

#ifdef METASHARE_HAVE_PORTAL
#include "portal_pipewire_source.hpp"
#endif

#ifdef METASHARE_HAVE_MUTTER
#include "mutter_screencast.hpp"
#include "mutter_virtual_source.hpp"
#endif

#if defined(METASHARE_HAVE_PORTAL) || defined(METASHARE_HAVE_MUTTER)
#include "audio_encoder.hpp"
#include "audio_source.hpp"
#include "pipewire_audio_source.hpp"
#endif

#include "test_pattern_source.hpp"
#include "test_tone_source.hpp"

using namespace metashare;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Options {
    std::string source =
#ifdef METASHARE_HAVE_MUTTER
        "mutter";
#elif defined(METASHARE_HAVE_PORTAL)
        "portal";
#else
        "test";
#endif
    int width = 1920;
    int height = 1080;
    // Default matches the Quest's 72 Hz refresh so the client's composite-to-
    // vsync step lands on a whole frame instead of beating against a 60 Hz grid.
    int fps = 72;
    // Ceiling for the loss-based bitrate controller. 8 Mbps is comfortable
    // for 1080p60 HEVC and leaves WiFi headroom for multiple monitors.
    int bitrate_kbps = 8000;
    std::string codec = "hevc";
    bool hardware = true;
    std::uint16_t port = signal::kDefaultSignalingPort;
    bool discovery = true;
    int monitors = 1;
    // Comma-separated list of audio channels to stream. "none" disables audio
    // entirely; recognised names are "system" (desktop output loopback),
    // "mic" (host microphone), "test" (synthetic 440 Hz tone). Defaults to
    // "system,mic" when audio is built in and the source is real (i.e. not
    // the test pattern source); empty when the audio TU isn't linked.
    std::string audio = "";
    int audio_bitrate_kbps = 96;
    // Remote input (Quest pointer/keyboard -> host). Portal capture sessions
    // are then created via the RemoteDesktop portal; if the desktop refuses,
    // the source falls back to capture-only by itself.
    bool input = true;
};

// Wire-level channel ids (kept as bare constants; they only select which audio
// capture pipeline feeds the single audio SSRC on the raw-RTP transport).
constexpr std::uint32_t kChannelAudioSystem = 1;
constexpr std::uint32_t kChannelAudioMic = 2;

void usage(const char* argv0) {
    std::fprintf(
        stderr,
        "MetaShare streamer — mirror a Wayland session to a Quest 3 client.\n\n"
        "Usage: %s [options]\n"
        "  --source <test|portal|mutter>  capture backend (default depends on "
        "build)\n"
        "                                test:   synthetic test patterns\n"
        "                                portal: xdg-desktop-portal (any "
        "compositor)\n"
        "                                        window 0 = real physical "
        "monitor,\n"
        "                                        windows 1..N-1 = virtual "
        "monitors\n"
        "                                        (shown in the app only when "
        "selected)\n"
        "                                mutter: GNOME direct — physical "
        "monitor\n"
        "                                        for window 0 (via portal) "
        "plus\n"
        "                                        N-1 Mutter virtual monitors\n"
        "                                        (is-platform=true, apps can "
        "be\n"
        "                                        moved onto them)\n"
        "  --monitors <n>          number of monitors (default 1)\n"
        "  --width <px>            virtual-monitor width (default 1920)\n"
        "  --height <px>           virtual-monitor height (default 1080)\n"
        "  --fps <n>              frame rate (default 72)\n"
        "  --bitrate <kbps>       max encoder bitrate; adapts down under "
        "loss\n"
        "                         (default 8000)\n"
        "  --codec <hevc|h264>    preferred codec (default hevc)\n"
        "  --no-hw                skip VAAPI/NVENC; force software encoding\n"
        "  --port <n>             signaling TCP base port (monitor i -> "
        "port+i)\n"
        "  --no-discovery         disable UDP discovery responder\n"
        "  --audio <list>         comma-separated audio channels to stream:\n"
        "                            system  desktop output loopback\n"
        "                            mic     host microphone\n"
        "                            test    synthetic 440 Hz tone\n"
        "                            none    disable audio entirely\n"
        "                          (default: system,mic for portal/mutter,\n"
        "                           none for test)\n"
        "  --audio-bitrate <kbps> Opus target bitrate (default 96)\n"
        "  --no-input             don't request remote-input injection (the\n"
        "                         client's pointer/keyboard are then ignored)\n"
        "  -h, --help             this help\n",
        argv0);
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](int& dst) {
            if (i + 1 >= argc) return false;
            dst = std::atoi(argv[++i]);
            return true;
        };
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (a == "--source" && i + 1 < argc) {
            o.source = argv[++i];
            if (o.source != "test" && o.source != "portal" &&
                o.source != "mutter") {
                std::fprintf(stderr, "unknown source: %s\n", o.source.c_str());
                return false;
            }
#if !defined(METASHARE_HAVE_MUTTER)
            if (o.source == "mutter") {
                std::fprintf(stderr, "mutter source not built in\n");
                return false;
            }
#endif
#if !defined(METASHARE_HAVE_PORTAL)
            if (o.source == "portal") {
                std::fprintf(stderr, "portal source not built in\n");
                return false;
            }
#endif
        } else if (a == "--monitors") {
            if (!val(o.monitors)) return false;
        } else if (a == "--width") {
            if (!val(o.width)) return false;
        } else if (a == "--height") {
            if (!val(o.height)) return false;
        } else if (a == "--fps") {
            if (!val(o.fps)) return false;
        } else if (a == "--bitrate") {
            if (!val(o.bitrate_kbps)) return false;
        } else if (a == "--codec" && i + 1 < argc) {
            o.codec = argv[++i];
            if (o.codec != "hevc" && o.codec != "h264") return false;
        } else if (a == "--no-hw")
            o.hardware = false;
        else if (a == "--port") {
            int p;
            if (!val(p)) return false;
            o.port = static_cast<std::uint16_t>(p);
        } else if (a == "--no-discovery")
            o.discovery = false;
        else if (a == "--audio" && i + 1 < argc) {
            o.audio = argv[++i];
        } else if (a == "--audio-bitrate") {
            if (!val(o.audio_bitrate_kbps)) return false;
        } else if (a == "--no-input") {
            o.input = false;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
    }
    if (o.monitors < 1) o.monitors = 1;
    if (o.monitors > 8) o.monitors = 8;

    // Apply the default audio mode if the user didn't say. We pick a sensible
    // default based on the active video source: real Wayland capture gets the
    // real desktop + mic, the test-pattern source gets a sine tone (so the
    // audio pipeline still exercises).
    if (o.audio.empty()) {
#if defined(METASHARE_HAVE_PORTAL) || defined(METASHARE_HAVE_MUTTER)
        o.audio = (o.source == "test") ? "test" : "system,mic";
#else
        o.audio = "none";
#endif
    }
    return true;
}

// One complete capture→encode→serve pipeline per monitor.
struct Pipeline {
    int index = 0;
    std::unique_ptr<FrameSource> source;
    std::unique_ptr<Encoder> encoder;
    std::unique_ptr<RtpServer> server;
    std::thread thread;
    std::atomic<bool> running{false};

    // Loss-based bitrate adaptation, driven by the client's RTCP receiver
    // reports (runs on the server's RTCP thread).
    std::mutex abr_mu;
    int abr_kbps = 0;  // current encoder target
    std::chrono::steady_clock::time_point abr_last_down{};
    std::chrono::steady_clock::time_point abr_clean_since{};
};

// Bitrate adaptation bounds/steps. On loss we cut hard (multiplicative
// decrease), on a sustained clean link we creep back up toward the configured
// maximum — classic AIMD, tuned slow so it never fights the jitter buffer.
constexpr int kAbrMinKbps = 2000;
constexpr float kAbrCutFactor = 0.6f;
constexpr float kAbrGrowFactor = 1.08f;
constexpr auto kAbrDownCooldown = std::chrono::seconds(2);
constexpr auto kAbrGrowAfterClean = std::chrono::seconds(4);

#if defined(METASHARE_HAVE_PORTAL) || defined(METASHARE_HAVE_MUTTER)
// One capture→encode pipeline per audio channel. Encoded packets are fanned
// out to *every* video pipeline's NetServer so a client that connected to any
// monitor port gets the same audio. Bandwidth cost is tiny (~96 kbps/channel).
struct AudioChannel {
    std::uint32_t channel_id = 0;
    std::unique_ptr<AudioSource> source;
    std::unique_ptr<AudioEncoder> encoder;
    std::thread thread;
    std::atomic<bool> running{false};
};

// Parse the comma-separated --audio list into channel ids. Recognised tokens:
// "none", "system", "mic", "test". Unknown tokens are an error.
std::vector<std::uint32_t> parse_audio_list(const std::string& s,
                                            std::string& err) {
    std::vector<std::uint32_t> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // Trim whitespace.
        auto p = tok.find_first_not_of(" \t");
        if (p != std::string::npos) tok.erase(0, p);
        auto q = tok.find_last_not_of(" \t");
        if (q != std::string::npos) tok.erase(q + 1);
        if (tok.empty()) continue;
        if (tok == "none") {
            out.clear();
            return out;
        }
        // Map the token to its wire channel id. "test" re-uses the system
        // channel slot so a client expecting desktop audio picks the synthetic
        // tone up transparently; there is no separate test channel on the wire.
        std::uint32_t id = 0;
        bool recognized = true;
        if (tok == "system" || tok == "test") {
            id = kChannelAudioSystem;
        } else if (tok == "mic") {
            id = kChannelAudioMic;
        } else {
            recognized = false;
        }
        if (!recognized) {
            err = "unknown --audio token: " + tok;
            return {};
        }
        out.push_back(id);
    }
    // De-dup (e.g. "system,system").
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
#endif

std::unique_ptr<FrameSource>
make_source(const Options& o, int monitor_index,
#ifdef METASHARE_HAVE_MUTTER
            MutterScreenCastSession* mutter_session,
#else
            void* /*mutter_session*/,
#endif
            std::string& err) {
    if (o.source == "test") {
        SourceFormat f{o.width, o.height, o.fps, 1};
        return std::make_unique<TestPatternSource>(f, monitor_index);
    }
    if (o.source == "portal") {
#ifdef METASHARE_HAVE_PORTAL
        // Monitor 0: the user's real physical display (source_type=MONITOR
        // brings up the standard GNOME picker once for the actual screen).
        // Monitors 1..N-1: virtual monitors (source_type=VIRTUAL) that the
        // client only opens when selected in the app's monitor menu.
        PortalOptions opts;
        opts.source_types =
            (monitor_index == 0) ? 1u : 4u;  // MONITOR : VIRTUAL
        opts.fps_hint = o.fps;
        opts.enable_input = o.input;
        return std::make_unique<PortalPipeWireSource>(opts);
#else
        err = "this build has no portal support; rebuild with portal deps or "
              "use --source test";
        return nullptr;
#endif
    }
    if (o.source == "mutter") {
        // Monitor 0: physical monitor, captured via the portal (so the user
        // gets the standard GNOME picker once for the real display).
        if (monitor_index == 0) {
#ifdef METASHARE_HAVE_PORTAL
            PortalOptions opts;
            opts.source_types = 1;  // MONITOR
            opts.fps_hint = o.fps;
            opts.enable_input = o.input;
            return std::make_unique<PortalPipeWireSource>(opts);
#else
            err = "mutter mode needs the portal source for monitor 0; "
                  "rebuild with portal support";
            return nullptr;
#endif
        }
        // Monitors 1..N-1: virtual monitors. If the Mutter direct session is
        // active (METASHARE_MUTTER_DIRECT env set + it actually started), use
        // MutterVirtualSource bound to it. Otherwise fall back to the portal's
        // VIRTUAL source type, which creates a real virtual monitor via
        // Mutter's RecordVirtual under the hood.
#ifdef METASHARE_HAVE_MUTTER
        if (mutter_session) {
            return std::make_unique<MutterVirtualSource>(
                *mutter_session, monitor_index - 1, o.fps);
        }
#endif
#ifdef METASHARE_HAVE_PORTAL
        {
            PortalOptions opts;
            opts.source_types = 4;  // VIRTUAL
            opts.fps_hint = o.fps;
            opts.enable_input = o.input;
            return std::make_unique<PortalPipeWireSource>(opts);
        }
#else
        err = "mutter mode needs portal or mutter support for virtual monitors";
        return nullptr;
#endif
    }
    err = "unknown source: " + o.source;
    return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // The Mutter session must outlive every MutterVirtualSource that references
    // it, so we declare it before the pipelines vector. C++ destroys locals in
    // reverse order, so this is torn down only after all pipelines are gone.
    // NOTE: the Mutter direct API (org.gnome.Mutter.ScreenCast) is plumbed in
    // mutter_screencast.cpp, but on stock GNOME user sessions Mutter's internal
    // PipeWire stream never reaches PW_STREAM_STATE_PAUSED, so
    // PipeWireStreamAdded is never emitted. The portal path
    // (xdg-desktop-portal ScreenCast with source_type=VIRTUAL) is the trusted
    // path that actually works on user sessions — it calls Mutter's
    // RecordVirtual under the hood from the portal's trusted process. So the
    // "mutter" source mode uses the portal for both physical and virtual
    // monitors. The Mutter direct code stays in the tree for future use on
    // system/headless Mutter where session creation isn't inhibited.
    std::string err;
#ifdef METASHARE_HAVE_MUTTER
    std::unique_ptr<MutterScreenCastSession> mutter_session;
    if (opt.source == "mutter" && opt.monitors > 1 &&
        std::getenv("METASHARE_MUTTER_DIRECT")) {
        std::fprintf(
            stderr,
            "[mutter] attempting direct Mutter ScreenCast (experimental;\n"
            "  works on system/headless Mutter, usually inhibited on user "
            "sessions).\n");
        mutter_session = std::make_unique<MutterScreenCastSession>();
        if (!mutter_session->open(err)) {
            std::fprintf(
                stderr,
                "[mutter] session open failed: %s — falling back to portal.\n",
                err.c_str());
            mutter_session.reset();
        } else {
            for (int i = 1; i < opt.monitors; ++i) {
                const int idx = mutter_session->add_virtual_monitor(
                    opt.width, opt.height, err);
                if (idx < 0) {
                    std::fprintf(stderr,
                                 "[mutter] add_virtual_monitor(%d) failed: %s\n"
                                 "  — falling back to portal.\n",
                                 i, err.c_str());
                    mutter_session.reset();
                    break;
                }
            }
            if (mutter_session) {
                if (!mutter_session->start(err)) {
                    std::fprintf(stderr,
                                 "[mutter] session start failed: %s\n"
                                 "  — falling back to portal.\n",
                                 err.c_str());
                    mutter_session.reset();
                } else {
                    std::fprintf(
                        stderr,
                        "[mutter] %d virtual monitor(s) ready via direct API\n",
                        opt.monitors - 1);
                }
            }
        }
    }
#endif

    // Host-wide send pacer shared by every pipeline (declared before the
    // pipelines so it outlives their server threads).
    Pacer pacer;

    std::vector<std::unique_ptr<Pipeline>> pipelines;

#if defined(METASHARE_HAVE_PORTAL) || defined(METASHARE_HAVE_MUTTER)
    // Resolve the --audio list up front. Audio starts *after* the video
    // pipelines are up so its fan-out list is non-empty when its threads
    // come online.
    std::string audio_err;
    std::vector<std::uint32_t> audio_channels =
        parse_audio_list(opt.audio, audio_err);
    if (!audio_err.empty()) {
        std::fprintf(stderr, "audio: %s\n", audio_err.c_str());
        return 2;
    }
    bool want_sys = false, want_test_tone = false;
    for (std::uint32_t id : audio_channels) {
        if (id == kChannelAudioSystem) want_sys = true;
    }
    if (opt.audio.find("test") != std::string::npos) {
        want_test_tone = true;
        want_sys = true;  // test tone replaces system audio
    }
    const AudioFormat audio_fmt{/*sample_rate=*/48000, /*channels=*/2};
#else
    if (!opt.audio.empty() && opt.audio != "none") {
        std::fprintf(stderr,
                     "audio: this build has no PipeWire support; rebuild with "
                     "-Dportal=enabled or pass --audio none\n");
        return 2;
    }
#endif

    for (int i = 0; i < opt.monitors; ++i) {
        auto p = std::make_unique<Pipeline>();
        p->index = i;

        // --- source ---
#ifdef METASHARE_HAVE_MUTTER
        p->source = make_source(opt, i, mutter_session.get(), err);
#else
        p->source = make_source(opt, i, nullptr, err);
#endif
        if (!p->source) {
            std::fprintf(stderr, "[monitor %d] error: %s\n", i, err.c_str());
            return 1;
        }
        if (!p->source->start(err)) {
            std::fprintf(stderr, "[monitor %d] source start failed: %s\n", i,
                         err.c_str());
            return 1;
        }
        const SourceFormat fmt = p->source->format();
        std::fprintf(stderr, "[monitor %d] capture %dx%d @ %d/%d fps\n", i,
                     fmt.width, fmt.height, fmt.fps_num, fmt.fps_den);

        // --- encoder ---
        p->encoder = std::make_unique<Encoder>();
        EncoderConfig ecfg;
        ecfg.width = fmt.width;
        ecfg.height = fmt.height;
        ecfg.fps_num = fmt.fps_num;
        ecfg.fps_den = fmt.fps_den;
        ecfg.bitrate_kbps = opt.bitrate_kbps;
        ecfg.prefer_hardware = opt.hardware;
        ecfg.preferred_codec =
            opt.codec == "hevc" ? proto::Codec::kH265 : proto::Codec::kH264;
        if (!p->encoder->open(ecfg, err)) {
            std::fprintf(stderr, "[monitor %d] encoder open failed: %s\n", i,
                         err.c_str());
            return 1;
        }
        const bool is_h265 = (p->encoder->codec() == proto::Codec::kH265);
        std::fprintf(stderr, "[monitor %d] encoder: %s (%s)%s\n", i,
                     p->encoder->codec_name(), is_h265 ? "HEVC" : "H.264",
                     p->encoder->using_hardware() ? " hw" : " sw");

        // --- RTP server (signaling + raw UDP media transport) ---
        p->server = std::make_unique<RtpServer>();
        p->server->set_id(i);
        VideoFormat vfmt;
        vfmt.width = fmt.width;
        vfmt.height = fmt.height;
        vfmt.fps_num = fmt.fps_num;
        vfmt.fps_den = fmt.fps_den;
        vfmt.codec = is_h265 ? proto::Codec::kH265 : proto::Codec::kH264;
        p->server->set_video_format(vfmt);
#if defined(METASHARE_HAVE_PORTAL) || defined(METASHARE_HAVE_MUTTER)
        // Raw-RTP transport carries a single audio stream; it lives on
        // monitor 0's server (audio is system-wide, so one feed per machine).
        if (i == 0) {
            p->server->set_audio_format(audio_fmt.sample_rate,
                                        audio_fmt.channels);
        }
#endif
        const std::uint16_t port = static_cast<std::uint16_t>(opt.port + i);
        if (!p->server->start(port, err)) {
            std::fprintf(stderr, "[monitor %d] server start failed: %s\n", i,
                         err.c_str());
            return 1;
        }
        // Service client PLI / keyframe requests by forcing the encoder's next
        // frame to be a keyframe.
        p->server->on_keyframe_request = [enc = p->encoder.get()] {
            if (enc) enc->force_keyframe();
        };

        // Remote input: route this monitor's INPUT events into its own capture
        // session, so pointer coordinates land on the surface the client sees.
        // Sources without an injection backend leave on_input unset and the
        // client's input is dropped. (The sink stays valid until source->stop()
        // and guards itself against post-stop calls.)
        if (InputSink* sink = p->source->input_sink()) {
            p->server->on_input = [sink](const input::Event& e) {
                switch (e.kind) {
                case input::Event::Kind::kMove:
                    sink->pointer_motion(e.x, e.y);
                    break;
                case input::Event::Kind::kButton:
                    sink->pointer_button(e.button, e.pressed);
                    break;
                case input::Event::Kind::kScroll:
                    sink->pointer_scroll(e.scroll_v, e.scroll_h);
                    break;
                case input::Event::Kind::kKey:
                    sink->key(e.keysym, e.pressed);
                    break;
                }
            };
            std::fprintf(stderr, "[monitor %d] remote input: enabled\n", i);
        } else if (opt.input) {
            std::fprintf(stderr,
                         "[monitor %d] remote input: unavailable for this "
                         "source\n",
                         i);
        }

        // Pace all video sends through the shared bucket, and adapt the
        // encoder bitrate to the loss the client reports (RTCP RRs).
        p->server->set_pacer(&pacer);
        p->abr_kbps = opt.bitrate_kbps;
        p->abr_last_down = std::chrono::steady_clock::now();
        p->abr_clean_since = p->abr_last_down;
        pacer.set_stream_rate(i, opt.bitrate_kbps);
        p->server->on_loss_report = [raw = p.get(), &pacer,
                                     max_kbps = opt.bitrate_kbps](float lost) {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(raw->abr_mu);
            int next = raw->abr_kbps;
            if (lost > 0.02f) {
                raw->abr_clean_since = now;
                if (now - raw->abr_last_down >= kAbrDownCooldown) {
                    next =
                        std::max(kAbrMinKbps, static_cast<int>(raw->abr_kbps *
                                                               kAbrCutFactor));
                    raw->abr_last_down = now;
                }
            } else if (lost < 0.005f) {
                if (now - raw->abr_clean_since >= kAbrGrowAfterClean &&
                    raw->abr_kbps < max_kbps) {
                    next = std::min(max_kbps, static_cast<int>(raw->abr_kbps *
                                                               kAbrGrowFactor));
                    raw->abr_clean_since = now;
                }
            } else {
                // Moderate loss: hold and reset the clean-link timer.
                raw->abr_clean_since = now;
            }
            if (next != raw->abr_kbps) {
                std::fprintf(stderr,
                             "[monitor %d] bitrate %d -> %d kbps "
                             "(loss %.1f%%)\n",
                             raw->index, raw->abr_kbps, next,
                             static_cast<double>(lost) * 100.0);
                raw->abr_kbps = next;
                raw->encoder->set_bitrate(next);
                pacer.set_stream_rate(raw->index, next);
            }
        };
        std::fprintf(stderr, "[monitor %d] signaling on tcp/%u\n", i,
                     static_cast<unsigned>(port));

        pipelines.push_back(std::move(p));
    }

    // --- discovery ---
    DiscoveryResponder discovery;
    if (opt.discovery) {
        if (!discovery.start(opt.port, static_cast<std::uint16_t>(opt.monitors),
                             err))
            std::fprintf(stderr, "warning: discovery disabled: %s\n",
                         err.c_str());
        else
            std::fprintf(
                stderr, "[streamer] discovery on udp/%u (%d monitors)\n",
                static_cast<unsigned>(proto::kDiscoveryPort), opt.monitors);
    }

    // --- capture threads ---
    for (auto& p : pipelines) {
        p->running = true;
        auto* raw = p.get();
        raw->thread = std::thread([raw] {
            auto sink = [raw](const std::uint8_t* data, std::size_t size,
                              std::int64_t pts_usec, bool key) {
                raw->server->broadcast_video(data, size, pts_usec, key);
            };
            std::string thread_err;
            // Pointer to the most recent frame. next_frame() guarantees the
            // returned frame stays valid until the next call, so we can hold it
            // across iterations and re-encode it when the source goes idle.
            AVFrame* last = nullptr;
            std::int64_t last_pts = 0;

            // Steady send cadence, decoupled from the compositor's delivery.
            // Mutter's virtual monitors are damage-driven: on_process() fires
            // in tight bursts (sub-millisecond spacing) then goes quiet for
            // hundreds of milliseconds, whereas the physical monitor is sampled
            // at a steady vsync. Both used to stamp PTS with
            // steady_clock::now() at delivery time and encode on every buffer,
            // so the bursty virtual cadence flowed straight through to the RTP
            // clock and the client showed it as heavy jitter.
            //
            // We now tick at a steady 1/fps and always encode the most recently
            // captured frame, mirroring TestPatternSource's pacing. Sleeping
            // before each pull means burst frames that arrive between ticks are
            // naturally dropped by the source's double buffer — we always grab
            // the latest on wake, so no intermediate content is missed, only
            // the irregular delivery timing is smoothed away.
            const SourceFormat fmt = raw->source->format();
            // Microseconds per frame, rounded from the source's fps rational.
            // Integer math avoids float-rounding drift and keeps the pacing
            // clock exact.
            const std::int64_t period_usec =
                (1'000'000LL * (fmt.fps_den ? fmt.fps_den : 1) +
                 (fmt.fps_num ? fmt.fps_num : 60) / 2) /
                (fmt.fps_num ? fmt.fps_num : 60);
            const auto period = std::chrono::microseconds(period_usec);
            auto next_tick = std::chrono::steady_clock::now() + period;
            // One-shot startup line so it is unambiguous from stderr that this
            // paced capture loop (not an older build) is the code running.
            std::fprintf(stderr,
                         "[monitor %d] pacer: period=%lldus (%.3f fps)\n",
                         raw->index, static_cast<long long>(period_usec),
                         1'000'000.0 / static_cast<double>(period_usec));

            // Non-blocking pull is what makes the pacing real. latest_frame()
            // returns immediately with either a fresh frame or "nothing new"
            // (in which case we re-encode `last`), so the loop period is set
            // solely by sleep_until(next_tick) — never dragged by the
            // compositor's bursty delivery the way the blocking next_frame()
            // was. Every tick encodes something (fresh or repeated) at an even
            // PTS, which also keeps the link alive and lets a PLI-forced
            // keyframe land within one interval on an otherwise idle monitor.

            while (raw->running && !g_stop) {
                std::this_thread::sleep_until(next_tick);
                next_tick += period;
                auto now = std::chrono::steady_clock::now();
                if (next_tick < now) next_tick = now + period;  // fell behind

                AVFrame* frame = nullptr;
                std::int64_t pts = 0;
                const int r = raw->source->latest_frame(&frame, pts);
                if (r < 0) {
                    std::fprintf(stderr, "[monitor %d] source ended\n",
                                 raw->index);
                    break;
                }
                if (r > 0) last = frame;
                if (!last) continue;  // no frame captured yet

                // PTS snapped to absolute period boundaries of the wall clock,
                // so consecutive ticks land on consecutive multiples of
                // period_usec — a perfectly even RTP clock for the client's
                // jitter buffer, regardless of how bursty capture was. `now` is
                // the post-sleep tick time (latest_frame() above doesn't block),
                // so it already sits on the grid.
                const auto now_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch())
                        .count();
                const std::int64_t tick =
                    (now_us + period_usec / 2) / period_usec;
                std::int64_t enc_pts = tick * period_usec;
                if (enc_pts <= last_pts) enc_pts = last_pts + period_usec;
                last_pts = enc_pts;

                if (!raw->encoder->encode(last, enc_pts, sink, thread_err)) {
                    std::fprintf(stderr, "[monitor %d] encode error: %s\n",
                                 raw->index, thread_err.c_str());
                    break;
                }
            }
            raw->encoder->flush(sink);
        });
    }

    std::fprintf(stderr, "[streamer] %d monitor(s) running — Ctrl-C to stop\n",
                 opt.monitors);

#if defined(METASHARE_HAVE_PORTAL) || defined(METASHARE_HAVE_MUTTER)
    // --- audio threads ---
    // Each audio channel captures → encodes → fans the encoded Opus packet out
    // to *every* video pipeline's NetServer (audio is system-wide, not per
    // monitor, so any client gets it regardless of which monitor port they
    // connected to).
    std::vector<std::unique_ptr<AudioChannel>> audio_pipelines;
    if (want_sys || want_test_tone) {
        // Build the list of (channel_id, source) pairs. The raw-RTP transport
        // carries a single audio stream — only the system/test channel is fed.
        struct AudioSpec {
            std::uint32_t channel_id;
            PipeWireAudioSource::Mode mode;
            bool is_test;
        };
        std::vector<AudioSpec> specs;
        if (want_test_tone) {
            specs.push_back({kChannelAudioSystem, {}, /*is_test=*/true});
        } else {
            for (std::uint32_t id : audio_channels) {
                if (id == kChannelAudioSystem)
                    specs.push_back(
                        {id, PipeWireAudioSource::Mode::kSystemSinkMonitor,
                         /*is_test=*/false});
            }
        }

        for (const auto& spec : specs) {
            auto ac = std::make_unique<AudioChannel>();
            ac->channel_id = spec.channel_id;

            if (spec.is_test) {
                ac->source = std::make_unique<TestToneSource>(audio_fmt);
            } else {
                ac->source =
                    std::make_unique<PipeWireAudioSource>(spec.mode, audio_fmt);
            }
            if (!ac->source->start(err)) {
                std::fprintf(stderr,
                             "[audio:%u] source start failed: %s — disabling\n",
                             spec.channel_id, err.c_str());
                err.clear();
                continue;
            }

            ac->encoder = std::make_unique<AudioEncoder>();
            AudioEncoderConfig acfg;
            acfg.format = audio_fmt;
            acfg.bitrate_kbps = opt.audio_bitrate_kbps;
            acfg.frame_ms = 20;
            if (!ac->encoder->open(acfg, err)) {
                std::fprintf(stderr,
                             "[audio:%u] encoder open failed: %s — disabling\n",
                             spec.channel_id, err.c_str());
                err.clear();
                ac->source->stop();
                continue;
            }

            ac->running = true;
            // Fan audio out to every monitor's RtpServer. Only monitor 0's
            // server has audio configured, but broadcast_audio() is a no-op
            // for servers that never had a client READY on the audio stream.
            std::vector<RtpServer*> sinks;
            sinks.reserve(pipelines.size());
            for (auto& p : pipelines) sinks.push_back(p->server.get());
            const std::uint32_t cid = spec.channel_id;
            auto* raw = ac.get();
            ac->thread = std::thread([raw, sinks, cid] {
                auto sink = [raw, sinks, cid](const std::uint8_t* data,
                                              std::size_t size,
                                              std::int64_t pts_usec) {
                    for (RtpServer* s : sinks)
                        s->broadcast_audio(cid, data, size, pts_usec);
                };
                std::string thread_err;
                while (raw->running && !g_stop) {
                    const std::int16_t* pcm = nullptr;
                    std::int64_t pts = 0;
                    int r = raw->source->next_chunk(&pcm, pts);
                    if (r == 0) continue;
                    if (r < 0) {
                        std::fprintf(stderr, "[audio:%u] source ended\n", cid);
                        break;
                    }
                    if (!raw->encoder->encode(pcm, r, pts, sink, thread_err)) {
                        std::fprintf(stderr, "[audio:%u] encode error: %s\n",
                                     cid, thread_err.c_str());
                        break;
                    }
                }
                raw->encoder->flush(sink);
            });

            const char* kind =
                spec.is_test ? "test-tone"
                : spec.mode == PipeWireAudioSource::Mode::kSystemSinkMonitor
                    ? "system-output"
                    : "microphone";
            std::fprintf(stderr,
                         "[audio:%u] %s capture running (opus %d kbps)\n",
                         spec.channel_id, kind, opt.audio_bitrate_kbps);
            audio_pipelines.push_back(std::move(ac));
        }
    }
#endif

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::fprintf(stderr, "\n[streamer] shutting down\n");
    discovery.stop();

#if defined(METASHARE_HAVE_PORTAL) || defined(METASHARE_HAVE_MUTTER)
    for (auto& a : audio_pipelines) {
        a->running = false;
        a->source->stop();
        if (a->thread.joinable()) a->thread.join();
    }
    audio_pipelines.clear();
#endif

    for (auto& p : pipelines) {
        p->running = false;
        p->source->stop();
        if (p->thread.joinable()) p->thread.join();
        p->server->stop();
    }
    return 0;
}
