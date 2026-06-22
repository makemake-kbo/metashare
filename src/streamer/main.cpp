// MetaShare streamer CLI.
//
// Pipeline (per monitor):
//   FrameSource -> Encoder (HEVC HW, H.264 SW fallback) -> NetServer (TCP)
// Multiple monitors run as parallel pipelines, each on its own TCP port
// (base, base+1, …) and its own capture thread.
// DiscoveryResponder (UDP) lets clients find us with no config.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "discovery.hpp"
#include "encoder.hpp"
#include "frame_source.hpp"
#include "net_server.hpp"
#include "protocol.hpp"
#include "test_pattern_source.hpp"

#ifdef METASHARE_HAVE_PORTAL
#include "portal_pipewire_source.hpp"
#endif

#ifdef METASHARE_HAVE_MUTTER
#include "mutter_screencast.hpp"
#include "mutter_virtual_source.hpp"
#endif

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
    int fps = 60;
    int bitrate_kbps = 15000;
    std::string codec = "hevc";
    bool hardware = true;
    std::uint16_t port = proto::kStreamPort;
    bool discovery = true;
    int monitors = 1;
};

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
        "  --fps <n>              frame rate (default 60)\n"
        "  --bitrate <kbps>       encoder bitrate (default 15000)\n"
        "  --codec <hevc|h264>    preferred codec (default hevc)\n"
        "  --no-hw                skip VAAPI/NVENC; force software encoding\n"
        "  --port <n>             TCP base port (monitor i → port+i)\n"
        "  --no-discovery         disable UDP discovery responder\n"
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
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return false;
        }
    }
    if (o.monitors < 1) o.monitors = 1;
    if (o.monitors > 8) o.monitors = 8;
    return true;
}

// One complete capture→encode→serve pipeline per monitor.
struct Pipeline {
    int index = 0;
    std::unique_ptr<FrameSource> source;
    std::unique_ptr<Encoder> encoder;
    std::unique_ptr<NetServer> server;
    std::thread thread;
    std::atomic<bool> running{false};
};

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
        if (o.monitors > 1) {
            PortalOptions opts;
            opts.source_types = 4;  // VIRTUAL
            opts.fps_hint = o.fps;
            return std::make_unique<PortalPipeWireSource>(opts);
        }
        return std::make_unique<PortalPipeWireSource>(o.fps);
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

    std::vector<std::unique_ptr<Pipeline>> pipelines;

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
        std::fprintf(stderr, "[monitor %d] encoder: %s (%s)%s\n", i,
                     p->encoder->codec_name(),
                     p->encoder->codec() == proto::Codec::kH265 ? "HEVC"
                                                                : "H.264",
                     p->encoder->using_hardware() ? " hw" : " sw");

        // --- server ---
        p->server = std::make_unique<NetServer>();
        proto::StreamHeader sh{};
        std::memcpy(sh.magic, proto::kStreamMagic, sizeof(sh.magic));
        sh.version = proto::kProtocolVersion;
        sh.codec = static_cast<std::uint32_t>(p->encoder->codec());
        sh.width = static_cast<std::uint32_t>(fmt.width);
        sh.height = static_cast<std::uint32_t>(fmt.height);
        sh.fps_num = static_cast<std::uint32_t>(fmt.fps_num);
        sh.fps_den = static_cast<std::uint32_t>(fmt.fps_den);
        p->server->set_stream_header(sh);
        const std::uint16_t port = static_cast<std::uint16_t>(opt.port + i);
        if (!p->server->start(port, err)) {
            std::fprintf(stderr, "[monitor %d] server start failed: %s\n", i,
                         err.c_str());
            return 1;
        }
        std::fprintf(stderr, "[monitor %d] serving on tcp/%u\n", i,
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
                proto::FrameHeader fh{};
                fh.payload_size = static_cast<std::uint32_t>(size);
                fh.flags = key ? proto::kFrameKeyframe : 0u;
                fh.pts_usec = static_cast<std::uint64_t>(pts_usec);
                raw->server->broadcast(fh, data);
            };
            std::string thread_err;
            while (raw->running && !g_stop) {
                AVFrame* frame = nullptr;
                std::int64_t pts = 0;
                int r = raw->source->next_frame(&frame, pts);
                if (r == 0) continue;
                if (r < 0) {
                    std::fprintf(stderr, "[monitor %d] source ended\n",
                                 raw->index);
                    break;
                }
                // Take our own reference: next_frame() returns a pointer to the
                // source's internal front_ buffer, which on_param_changed() may
                // free/realloc while we're encoding. av_frame_ref shares the
                // underlying data safely.
                AVFrame* safe = av_frame_alloc();
                if (!safe || av_frame_ref(safe, frame) < 0) {
                    if (safe) av_frame_free(&safe);
                    continue;
                }
                bool ok = raw->encoder->encode(safe, pts, sink, thread_err);
                av_frame_free(&safe);
                if (!ok) {
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
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::fprintf(stderr, "\n[streamer] shutting down\n");
    discovery.stop();

    for (auto& p : pipelines) {
        p->running = false;
        p->source->stop();
        if (p->thread.joinable()) p->thread.join();
        p->server->stop();
    }
    return 0;
}
