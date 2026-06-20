// MetaShare streamer CLI.
//
// Pipeline:  FrameSource -> Encoder (H.264) -> NetServer (TCP) -> client(s)
//            DiscoveryResponder (UDP) lets clients find us with no config.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "discovery.hpp"
#include "encoder.hpp"
#include "frame_source.hpp"
#include "net_server.hpp"
#include "protocol.hpp"
#include "test_pattern_source.hpp"

#ifdef METASHARE_HAVE_PORTAL
#include "portal_pipewire_source.hpp"
#endif

using namespace metashare;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Options {
    std::string source =
#ifdef METASHARE_HAVE_PORTAL
        "portal";
#else
        "test";
#endif
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrate_kbps = 15000;
    std::uint16_t port = proto::kStreamPort;
    bool discovery = true;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
        "MetaShare streamer — mirror a Wayland session to a Quest 3 client.\n\n"
        "Usage: %s [options]\n"
        "  --source <test|portal>  capture backend (default depends on build)\n"
        "  --width <px>            test-source width (default 1920)\n"
        "  --height <px>           test-source height (default 1080)\n"
        "  --fps <n>              frame rate (default 60)\n"
        "  --bitrate <kbps>       encoder bitrate (default 15000)\n"
        "  --port <n>             TCP stream port (default %u)\n"
        "  --no-discovery         disable UDP discovery responder\n"
        "  -h, --help             this help\n",
        argv0, static_cast<unsigned>(proto::kStreamPort));
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](int& dst) {
            if (i + 1 >= argc) return false;
            dst = std::atoi(argv[++i]);
            return true;
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
        else if (a == "--source" && i + 1 < argc) o.source = argv[++i];
        else if (a == "--width") { if (!val(o.width)) return false; }
        else if (a == "--height") { if (!val(o.height)) return false; }
        else if (a == "--fps") { if (!val(o.fps)) return false; }
        else if (a == "--bitrate") { if (!val(o.bitrate_kbps)) return false; }
        else if (a == "--port") { int p; if (!val(p)) return false; o.port = static_cast<std::uint16_t>(p); }
        else if (a == "--no-discovery") o.discovery = false;
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return false; }
    }
    return true;
}

std::unique_ptr<FrameSource> make_source(const Options& o, std::string& err) {
    if (o.source == "test") {
        SourceFormat f{o.width, o.height, o.fps, 1};
        return std::make_unique<TestPatternSource>(f);
    }
    if (o.source == "portal") {
#ifdef METASHARE_HAVE_PORTAL
        return std::make_unique<PortalPipeWireSource>(o.fps);
#else
        err = "this build has no portal support; rebuild with portal deps or "
              "use --source test";
        return nullptr;
#endif
    }
    err = "unknown source: " + o.source;
    return nullptr;
}
}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) { usage(argv[0]); return 2; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string err;
    auto source = make_source(opt, err);
    if (!source) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (!source->start(err)) {
        std::fprintf(stderr, "source start failed: %s\n", err.c_str());
        return 1;
    }
    const SourceFormat fmt = source->format();
    std::fprintf(stderr, "[streamer] capture %dx%d @ %d/%d fps\n", fmt.width,
                 fmt.height, fmt.fps_num, fmt.fps_den);

    Encoder enc;
    EncoderConfig ecfg;
    ecfg.width = fmt.width;
    ecfg.height = fmt.height;
    ecfg.fps_num = fmt.fps_num;
    ecfg.fps_den = fmt.fps_den;
    ecfg.bitrate_kbps = opt.bitrate_kbps;
    if (!enc.open(ecfg, err)) {
        std::fprintf(stderr, "encoder open failed: %s\n", err.c_str());
        return 1;
    }

    NetServer server;
    proto::StreamHeader sh{};
    std::memcpy(sh.magic, proto::kStreamMagic, sizeof(sh.magic));
    sh.version = proto::kProtocolVersion;
    sh.codec = static_cast<std::uint32_t>(enc.codec());
    sh.width = static_cast<std::uint32_t>(fmt.width);
    sh.height = static_cast<std::uint32_t>(fmt.height);
    sh.fps_num = static_cast<std::uint32_t>(fmt.fps_num);
    sh.fps_den = static_cast<std::uint32_t>(fmt.fps_den);
    server.set_stream_header(sh);
    if (!server.start(opt.port, err)) {
        std::fprintf(stderr, "server start failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[streamer] serving on tcp/%u\n",
                 static_cast<unsigned>(opt.port));

    DiscoveryResponder discovery;
    if (opt.discovery) {
        if (!discovery.start(opt.port, err))
            std::fprintf(stderr, "warning: discovery disabled: %s\n",
                         err.c_str());
        else
            std::fprintf(stderr, "[streamer] discovery on udp/%u\n",
                         static_cast<unsigned>(proto::kDiscoveryPort));
    }

    auto sink = [&](const std::uint8_t* data, std::size_t size,
                    std::int64_t pts_usec, bool key) {
        proto::FrameHeader fh{};
        fh.payload_size = static_cast<std::uint32_t>(size);
        fh.flags = key ? proto::kFrameKeyframe : 0u;
        fh.pts_usec = static_cast<std::uint64_t>(pts_usec);
        server.broadcast(fh, data);
    };

    std::fprintf(stderr, "[streamer] running — Ctrl-C to stop\n");
    while (!g_stop) {
        AVFrame* frame = nullptr;
        std::int64_t pts = 0;
        int r = source->next_frame(&frame, pts);
        if (r == 0) continue;
        if (r < 0) { std::fprintf(stderr, "[streamer] source ended\n"); break; }
        if (!enc.encode(frame, pts, sink, err)) {
            std::fprintf(stderr, "encode error: %s\n", err.c_str());
            break;
        }
    }

    std::fprintf(stderr, "\n[streamer] shutting down\n");
    enc.flush(sink);
    discovery.stop();
    server.stop();
    source->stop();
    return 0;
}
