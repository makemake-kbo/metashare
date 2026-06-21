// Standalone Mutter ScreenCast PoC for MetaShare Phase 0.
//
// Creates a virtual monitor and captures one frame from it, writing the result
// as a PPM file. Verifies Mutter + PipeWire + capture works end-to-end.
//
// Two modes:
//
//   --mode portal-virtual   (default, works on any GNOME 44+ user session)
//       Uses xdg-desktop-portal ScreenCast with the VIRTUAL source type. The
//       portal shows a one-time dialog asking the user to confirm creating a
//       virtual monitor. Under the hood, the portal calls Mutter's
//       RecordVirtual — same API as mutter-direct below — but the portal is
//       trusted by Mutter so it bypasses Mutter's "session creation inhibited"
//       policy that blocks external apps from calling ScreenCast directly on
//       user sessions.
//
//   --mode mutter-direct
//       Talks to org.gnome.Mutter.ScreenCast directly. Cleaner API, no dialog,
//       supports N virtual monitors in one session. BUT: on a stock GNOME user
//       session, GNOME Shell sets session_creation_inhibited=TRUE, forcing
//       apps through the portal. This mode only works in:
//         * gnome-remote-desktop system / headless mode (its own Mutter)
//         * A patched Mutter / GNOME Shell that doesn't inhibit
//         * Future Mutter versions if the policy changes
//
//   --mode portal-monitor
//       Pick an existing monitor via the portal dialog (no virtual monitor
//       created). Useful for sanity-checking capture without virtual-monitor
//       creation.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "frame_source.hpp"
#include "mutter_screencast.hpp"
#include "mutter_virtual_source.hpp"
#include "portal_pipewire_source.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

using namespace metashare;

namespace {

// xdg-desktop-portal ScreenCast AvailableSourceTypes bitmask:
//   1 = MONITOR, 2 = WINDOW, 4 = VIRTUAL.
constexpr std::uint32_t kSourceMonitor = 1;
constexpr std::uint32_t kSourceVirtual = 4;

bool write_ppm(const char* path, int w, int h, const std::uint8_t* data,
               int stride, AVPixelFormat fmt) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    std::vector<std::uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    const bool bgr_order = (fmt == AV_PIX_FMT_BGRA || fmt == AV_PIX_FMT_BGR0);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = data + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* px = row + x * 4;
            std::uint8_t r, g, b;
            if (bgr_order) { b = px[0]; g = px[1]; r = px[2]; }
            else           { r = px[0]; g = px[1]; b = px[2]; }
            auto* o = &rgb[(static_cast<size_t>(y) * w + x) * 3];
            o[0] = r; o[1] = g; o[2] = b;
        }
    }
    f.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
    return f.good();
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "MetaShare Mutter virtual-monitor dump — Phase 0 PoC.\n\n"
        "Creates one virtual monitor (or captures an existing one) and dumps\n"
        "a single frame as a PPM file.\n\n"
        "Usage: %s [options]\n"
        "  --mode <name>        portal-virtual | portal-monitor | mutter-direct\n"
        "                       (default: portal-virtual)\n"
        "  --output <path>      output PPM path (default: metashare-dump.ppm)\n"
        "  --fps <n>            capture framerate hint (default 60)\n"
        "  --width <px>         mutter-direct virtual monitor width (default 1280)\n"
        "  --height <px>        mutter-direct virtual monitor height (default 720)\n"
        "  -h, --help           this help\n\n"
        "Requires GNOME 44+. portal-* modes show a one-time confirmation dialog.\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "portal-virtual";
    std::string out_path = "metashare-dump.ppm";
    int fps = 60;
    int mon_w = 1280, mon_h = 720;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "--mode") { if (auto* v = next("--mode")) mode = v; else return 2; }
        else if (a == "--output") { if (auto* v = next("--output")) out_path = v; else return 2; }
        else if (a == "--fps") { if (auto* v = next("--fps")) fps = std::atoi(v); else return 2; }
        else if (a == "--width") { if (auto* v = next("--width")) mon_w = std::atoi(v); else return 2; }
        else if (a == "--height") { if (auto* v = next("--height")) mon_h = std::atoi(v); else return 2; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    std::fprintf(stderr, "[dump] mode=%s output=%s\n", mode.c_str(), out_path.c_str());

    std::unique_ptr<FrameSource> source;
    std::unique_ptr<MutterScreenCastSession> mutter_session;  // keeps session alive

    std::string err;
    if (mode == "portal-virtual") {
        PortalOptions opts;
        opts.fps_hint = fps;
        opts.source_types = kSourceVirtual;
        opts.multiple = false;
        opts.cursor_mode = 1;  // EMBEDDED so the cursor is in the dumped pixels
        source = std::make_unique<PortalPipeWireSource>(opts);
    } else if (mode == "portal-monitor") {
        PortalOptions opts;
        opts.fps_hint = fps;
        opts.source_types = kSourceMonitor;
        opts.multiple = false;
        opts.cursor_mode = 1;  // EMBEDDED
        source = std::make_unique<PortalPipeWireSource>(opts);
    } else if (mode == "mutter-direct") {
        mutter_session = std::make_unique<MutterScreenCastSession>();
        if (!mutter_session->open(err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (mutter_session->add_virtual_monitor(mon_w, mon_h, err) < 0) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (!mutter_session->start(err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        source = std::make_unique<MutterVirtualSource>(*mutter_session, 0, fps);
    } else {
        std::fprintf(stderr, "unknown --mode: %s\n", mode.c_str());
        usage(argv[0]);
        return 2;
    }

    if (!source->start(err)) {
        std::fprintf(stderr, "source start failed: %s\n", err.c_str());
        return 1;
    }
    const SourceFormat fmt = source->format();
    std::fprintf(stderr, "[dump] capture %dx%d @ %d/%d fps\n",
                 fmt.width, fmt.height, fmt.fps_num, fmt.fps_den);

    // Wait for the first frame (up to ~10 s).
    AVFrame* frame = nullptr;
    std::int64_t pts = 0;
    for (int tries = 0; tries < 20 && !frame; ++tries) {
        int r = source->next_frame(&frame, pts);
        if (r < 0) {
            std::fprintf(stderr, "[dump] source ended before a frame arrived\n");
            source->stop();
            return 1;
        }
    }
    if (!frame) {
        std::fprintf(stderr, "[dump] timed out waiting for a frame\n");
        source->stop();
        return 1;
    }

    const bool ok = write_ppm(out_path.c_str(), frame->width, frame->height,
                              frame->data[0], frame->linesize[0],
                              static_cast<AVPixelFormat>(frame->format));
    if (ok) {
        std::fprintf(stderr, "[dump] wrote %s (%dx%d fmt=%d)\n",
                     out_path.c_str(), frame->width, frame->height,
                     frame->format);
    } else {
        std::fprintf(stderr, "[dump] write failed: %s\n", out_path.c_str());
    }

    source->stop();
    if (mutter_session) mutter_session->close();
    std::fprintf(stderr, "[dump] done\n");
    return ok ? 0 : 1;
}
