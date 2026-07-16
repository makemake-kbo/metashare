#include "portal_pipewire_source.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/props.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace metashare {

// ===========================================================================
//  Portal (xdg-desktop-portal ScreenCast) over D-Bus
// ===========================================================================
//
// sdbus-c++ is the most version-sensitive dependency here. This targets the
// v1.x string-based API (the version in current nixpkgs): plain-string
// method/interface names and an explicit finishRegistration() after signal
// registration. sdbus-c++ v2 replaced these with strong types (ServiceName,
// InterfaceName, MethodName) and dropped finishRegistration().

struct PortalState {
    std::unique_ptr<sdbus::IConnection> conn;
    // Kept alive for the session's lifetime so remote-input Notify* calls can
    // reuse it (creating a proxy per pointer move would be absurd).
    std::unique_ptr<sdbus::IProxy> portal;
    std::string session_handle;
};

namespace {

constexpr const char* kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCastIface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRemoteDesktopIface =
    "org.freedesktop.portal.RemoteDesktop";
constexpr const char* kRequestIface = "org.freedesktop.portal.Request";

std::string random_token(const char* prefix) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 0x7fffffff);
    return std::string(prefix) + std::to_string(dist(rng));
}

// /org/freedesktop/portal/desktop/request/<sanitized unique name>/<token>
std::string request_path(const std::string& unique_name,
                         const std::string& token) {
    std::string sender = unique_name;
    if (!sender.empty() && sender[0] == ':') sender.erase(0, 1);
    for (char& c : sender)
        if (c == '.') c = '_';
    return "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
}

using VarMap = std::map<std::string, sdbus::Variant>;

// Issue a portal request and block until its Response signal arrives.
// `invoke` performs the actual method call (it receives the option map already
// seeded with handle_token and must call the portal method). Returns false on
// failure or non-zero portal response code.
bool portal_request(sdbus::IConnection& conn, const std::string& unique_name,
                    VarMap options, const std::function<void(VarMap&)>& invoke,
                    VarMap& results, std::string& err, int timeout_s = 120) {
    const std::string token = random_token("ms");
    const std::string rpath = request_path(unique_name, token);
    options["handle_token"] = sdbus::Variant(token);

    auto req = sdbus::createProxy(conn, std::string{kPortalService}, rpath);

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::uint32_t code = 0;
    VarMap out;

    req->uponSignal("Response")
        .onInterface(kRequestIface)
        .call([&](const std::uint32_t& response, const VarMap& r) {
            std::lock_guard<std::mutex> lk(m);
            code = response;
            out = r;
            done = true;
            cv.notify_all();
        });
    req->finishRegistration();

    try {
        invoke(options);
    } catch (const sdbus::Error& e) {
        err = std::string("portal call failed: ") + e.what();
        return false;
    }

    std::unique_lock<std::mutex> lk(m);
    if (!cv.wait_for(lk, std::chrono::seconds(timeout_s),
                     [&] { return done; })) {
        err = "portal request timed out";
        return false;
    }
    if (code != 0) {
        err = "portal request rejected (code " + std::to_string(code) + ")";
        return false;
    }
    results = std::move(out);
    return true;
}

}  // namespace

bool PortalPipeWireSource::run_portal(int& pw_fd, std::uint32_t& node_id,
                                      std::string& err) {
    if (opts_.enable_input) {
        // Try the combined RemoteDesktop+ScreenCast session first — same
        // capture, plus input injection. Portals without RemoteDesktop (or a
        // user denying the extra permission) drop us to capture-only below.
        std::string input_err;
        if (run_portal_session(true, pw_fd, node_id, input_err)) {
            input_ready_ = true;
            std::fprintf(stderr,
                         "[portal] remote input enabled (RemoteDesktop "
                         "session, stream node %u)\n",
                         node_id);
            return true;
        }
        std::fprintf(stderr,
                     "[portal] RemoteDesktop session unavailable (%s) — "
                     "falling back to capture-only\n",
                     input_err.c_str());
        portal_.reset();
    }
    return run_portal_session(false, pw_fd, node_id, err);
}

bool PortalPipeWireSource::run_portal_session(bool with_input, int& pw_fd,
                                              std::uint32_t& node_id,
                                              std::string& err) {
    portal_ = std::make_unique<PortalState>();

    // The portal source requires a D-Bus session bus (it talks to
    // xdg-desktop-portal over it). Pre-flight the env so we can give an
    // actionable error instead of the cryptic sdbus "ENOMEDIUM" message.
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        bool bus_socket_present = false;
        if (xdg) {
            std::string sock = std::string(xdg) + "/bus";
            struct stat st;
            bus_socket_present = (::stat(sock.c_str(), &st) == 0);
        }
        if (!bus_socket_present) {
            err = "no D-Bus session bus available. The portal source needs "
                  "xdg-desktop-portal over D-Bus. Either run from a graphical "
                  "session (GNOME/KDE/Sway/etc.), or prefix your command with "
                  "'dbus-run-session' (and make sure xdg-desktop-portal is "
                  "actually running in that bus).";
            return false;
        }
    }

    try {
        portal_->conn = sdbus::createSessionBusConnection();
    } catch (const sdbus::Error& e) {
        err = std::string("cannot connect to session bus: ") + e.what() +
              " (check that dbus-daemon is running and "
              "DBUS_SESSION_BUS_ADDRESS is set)";
        return false;
    }
    portal_->conn->enterEventLoopAsync();
    sdbus::IConnection& conn = *portal_->conn;
    const std::string unique = conn.getUniqueName();

    portal_->portal = sdbus::createProxy(conn, std::string{kPortalService},
                                         std::string{kPortalPath});
    auto& portal = portal_->portal;

    // Combined sessions are created (and started) through the RemoteDesktop
    // interface; ScreenCast methods then operate on the same session handle.
    const char* session_iface =
        with_input ? kRemoteDesktopIface : kScreenCastIface;

    // 1) CreateSession
    {
        VarMap opts;
        opts["session_handle_token"] = sdbus::Variant(random_token("mssess"));
        VarMap res;
        auto call = [&](VarMap& options) {
            sdbus::ObjectPath handle;
            portal->callMethod("CreateSession")
                .onInterface(session_iface)
                .withArguments(options)
                .storeResultsTo(handle);
        };
        if (!portal_request(conn, unique, opts, call, res, err)) return false;
        auto it = res.find("session_handle");
        if (it == res.end()) {
            err = "portal: no session_handle in response";
            return false;
        }
        portal_->session_handle = it->second.get<std::string>();
    }

    const sdbus::ObjectPath session{portal_->session_handle};

    // 1b) SelectDevices — ask for keyboard (1) + pointer (2) injection.
    if (with_input) {
        VarMap opts;
        opts["types"] = sdbus::Variant(std::uint32_t{3});
        VarMap res;
        auto call = [&](VarMap& options) {
            sdbus::ObjectPath handle;
            portal->callMethod("SelectDevices")
                .onInterface(kRemoteDesktopIface)
                .withArguments(session, options)
                .storeResultsTo(handle);
        };
        if (!portal_request(conn, unique, opts, call, res, err)) return false;
    }

    // 2) SelectSources — configurable via opts_. Default is MONITOR + single +
    // METADATA cursor (so the cursor comes as PipeWire stream metadata, not
    // baked into pixels). The VIRTUAL type lets the portal create a brand-new
    // virtual monitor under Mutter (which is the only way to create virtual
    // monitor space on a stock GNOME user session — Mutter blocks direct
    // RecordVirtual calls there).
    {
        VarMap opts;
        opts["types"] = sdbus::Variant(opts_.source_types);
        opts["multiple"] = sdbus::Variant(opts_.multiple);
        opts["cursor_mode"] = sdbus::Variant(opts_.cursor_mode);
        VarMap res;
        auto call = [&](VarMap& options) {
            sdbus::ObjectPath handle;
            portal->callMethod("SelectSources")
                .onInterface(kScreenCastIface)
                .withArguments(session, options)
                .storeResultsTo(handle);
        };
        if (!portal_request(conn, unique, opts, call, res, err)) return false;
    }

    // 3) Start — shows the picker dialog; user chooses a monitor. On a
    // combined session Start must go through RemoteDesktop; the response
    // still carries the ScreenCast streams.
    VarMap start_res;
    {
        VarMap opts;
        auto call = [&](VarMap& options) {
            sdbus::ObjectPath handle;
            portal->callMethod("Start")
                .onInterface(session_iface)
                .withArguments(session, std::string{""}, options)
                .storeResultsTo(handle);
        };
        if (!portal_request(conn, unique, opts, call, start_res, err))
            return false;
    }

    // streams: a(ua{sv}) — pick the first node id.
    {
        auto it = start_res.find("streams");
        if (it == start_res.end()) {
            err = "portal: no streams returned";
            return false;
        }
        using Stream = sdbus::Struct<std::uint32_t, VarMap>;
        auto streams = it->second.get<std::vector<Stream>>();
        if (streams.empty()) {
            err = "portal: empty stream list";
            return false;
        }
        node_id = std::get<0>(streams[0]);
        node_id_ = node_id;  // absolute pointer moves target this stream
    }

    // 4) OpenPipeWireRemote -> file descriptor.
    {
        VarMap opts;
        sdbus::UnixFd fd;
        try {
            portal->callMethod("OpenPipeWireRemote")
                .onInterface(kScreenCastIface)
                .withArguments(session, opts)
                .storeResultsTo(fd);
        } catch (const sdbus::Error& e) {
            err = std::string("OpenPipeWireRemote failed: ") + e.what();
            return false;
        }
        pw_fd = ::dup(fd.get());  // we own this copy; pipewire closes it
        if (pw_fd < 0) {
            err = "dup(pipewire fd) failed";
            return false;
        }
    }
    return true;
}

// ===========================================================================
//  Remote-input injection (org.freedesktop.portal.RemoteDesktop)
// ===========================================================================
//
// Called from the signaling thread while the D-Bus event loop runs async —
// the same pattern the portal negotiation above already relies on. Notify*
// methods return nothing, so dontExpectReply() keeps them fire-and-forget.

InputSink* PortalPipeWireSource::input_sink() {
    return input_ready_ ? static_cast<InputSink*>(this) : nullptr;
}

void PortalPipeWireSource::input_error(const char* what) {
    const int n = input_errors_.fetch_add(1) + 1;
    if (n == 1)
        std::fprintf(stderr, "[portal] input injection error: %s\n", what);
    if (n == 16) {
        std::fprintf(stderr,
                     "[portal] repeated injection errors — remote input "
                     "disabled\n");
        input_ready_ = false;
    }
}

void PortalPipeWireSource::pointer_motion(double nx, double ny) {
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        w = fmt_.width;
        h = fmt_.height;
    }
    if (w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(input_mu_);
    if (!input_ready_ || !portal_ || !portal_->portal) return;
    try {
        portal_->portal->callMethod("NotifyPointerMotionAbsolute")
            .onInterface(kRemoteDesktopIface)
            .withArguments(sdbus::ObjectPath{portal_->session_handle}, VarMap{},
                           node_id_, nx * w, ny * h)
            .dontExpectReply();
    } catch (const sdbus::Error& e) { input_error(e.what()); }
}

void PortalPipeWireSource::pointer_button(int evdev_button, bool pressed) {
    std::lock_guard<std::mutex> lk(input_mu_);
    if (!input_ready_ || !portal_ || !portal_->portal) return;
    try {
        portal_->portal->callMethod("NotifyPointerButton")
            .onInterface(kRemoteDesktopIface)
            .withArguments(sdbus::ObjectPath{portal_->session_handle}, VarMap{},
                           static_cast<std::int32_t>(evdev_button),
                           std::uint32_t{pressed ? 1u : 0u})
            .dontExpectReply();
    } catch (const sdbus::Error& e) { input_error(e.what()); }
}

void PortalPipeWireSource::pointer_scroll(int v_steps, int h_steps) {
    std::lock_guard<std::mutex> lk(input_mu_);
    if (!input_ready_ || !portal_ || !portal_->portal) return;
    // axis: 0 = vertical, 1 = horizontal; steps positive = down / right.
    const std::pair<std::uint32_t, int> axes[] = {{0u, v_steps}, {1u, h_steps}};
    for (const auto& [axis, steps] : axes) {
        if (steps == 0) continue;
        try {
            portal_->portal->callMethod("NotifyPointerAxisDiscrete")
                .onInterface(kRemoteDesktopIface)
                .withArguments(sdbus::ObjectPath{portal_->session_handle},
                               VarMap{}, axis, static_cast<std::int32_t>(steps))
                .dontExpectReply();
        } catch (const sdbus::Error& e) { input_error(e.what()); }
    }
}

void PortalPipeWireSource::key(std::uint32_t keysym, bool pressed) {
    std::lock_guard<std::mutex> lk(input_mu_);
    if (!input_ready_ || !portal_ || !portal_->portal) return;
    try {
        portal_->portal->callMethod("NotifyKeyboardKeysym")
            .onInterface(kRemoteDesktopIface)
            .withArguments(sdbus::ObjectPath{portal_->session_handle}, VarMap{},
                           static_cast<std::int32_t>(keysym),
                           std::uint32_t{pressed ? 1u : 0u})
            .dontExpectReply();
    } catch (const sdbus::Error& e) { input_error(e.what()); }
}

// ===========================================================================
//  PipeWire stream
// ===========================================================================

namespace {

AVPixelFormat spa_to_av(std::uint32_t spa_fmt) {
    switch (spa_fmt) {
    case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
    case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
    case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
    case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
    default: return AV_PIX_FMT_NONE;
    }
}

void cb_param_changed(void* data, std::uint32_t id,
                      const struct spa_pod* param) {
    if (id != SPA_PARAM_Format || param == nullptr) return;
    static_cast<PortalPipeWireSource*>(data)->on_param_changed(param);
}
void cb_process(void* data) {
    static_cast<PortalPipeWireSource*>(data)->on_process();
}
void cb_state_changed(void* data, enum pw_stream_state old,
                      enum pw_stream_state state, const char* error) {
    std::fprintf(stderr, "[pipewire] state: %s%s%s\n",
                 pw_stream_state_as_string(state), error ? " - " : "",
                 error ? error : "");
}

static struct pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = cb_state_changed,
    .param_changed = cb_param_changed,
    .process = cb_process,
};

}  // namespace

void PortalPipeWireSource::on_param_changed(const struct spa_pod* param) {
    struct spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_video ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

    const int w = info.info.raw.size.width;
    const int h = info.info.raw.size.height;
    const int new_fmt = spa_to_av(info.info.raw.format);
    if (new_fmt == AV_PIX_FMT_NONE) {
        std::fprintf(stderr, "[pipewire] unsupported format %u\n",
                     info.info.raw.format);
        return;
    }

    // Skip reallocation if nothing changed — PipeWire re-sends the same format
    // repeatedly on virtual monitors, and each realloc causes a brief dropout
    // (flicker) plus a use-after-free race with the capture thread.
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (negotiated_fmt_ == new_fmt && fmt_.width == w && fmt_.height == h &&
            front_ && back_) {
            negotiated_fmt_ = new_fmt;
            // Still need to reply with buffer params.
            goto reply_params;
        }
    }

    negotiated_fmt_ = new_fmt;

    {
        std::lock_guard<std::mutex> lk(mu_);
        fmt_.width = w;
        fmt_.height = h;
        for (AVFrame** f : {&front_, &back_}) {
            if (*f) av_frame_free(f);
            *f = av_frame_alloc();
            (*f)->format = negotiated_fmt_;
            (*f)->width = w;
            (*f)->height = h;
            av_frame_get_buffer(*f, 32);
        }
    }
    std::fprintf(stderr, "[pipewire] negotiated %dx%d fmt=%d\n", w, h,
                 negotiated_fmt_);

reply_params:;

    // Reply with buffer params: request CPU-mappable buffers.
    std::uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[1];
    params[0] = reinterpret_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) |
                                 (1 << SPA_DATA_MemFd))));
    pw_stream_update_params(stream_, params, 1);
}

void PortalPipeWireSource::on_process() {
    struct pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (!b) return;
    struct spa_buffer* sbuf = b->buffer;
    if (sbuf->n_datas >= 1 && sbuf->datas[0].data && negotiated_fmt_ >= 0) {
        std::lock_guard<std::mutex> lk(mu_);
        if (back_) {
            const auto& d = sbuf->datas[0];
            const int src_stride =
                d.chunk->stride ? d.chunk->stride : back_->linesize[0];
            const auto* src =
                static_cast<const std::uint8_t*>(d.data) + d.chunk->offset;
            av_image_copy_plane(
                back_->data[0], back_->linesize[0], src, src_stride,
                std::min(src_stride, back_->linesize[0]), back_->height);
            const auto now =
                std::chrono::steady_clock::now().time_since_epoch();
            pts_usec_ =
                std::chrono::duration_cast<std::chrono::microseconds>(now)
                    .count();
            std::swap(front_, back_);
            have_new_ = true;
            cv_.notify_one();
        }
    }
    pw_stream_queue_buffer(stream_, b);
}

// ===========================================================================
//  FrameSource interface
// ===========================================================================

PortalPipeWireSource::PortalPipeWireSource(int fps_hint)
    : opts_{/*fps_hint=*/fps_hint} {
    fmt_.fps_num = opts_.fps_hint;
    fmt_.fps_den = 1;
}

PortalPipeWireSource::PortalPipeWireSource(const PortalOptions& opts)
    : opts_(opts) {
    fmt_.fps_num = opts_.fps_hint;
    fmt_.fps_den = 1;
}

PortalPipeWireSource::~PortalPipeWireSource() { stop(); }

bool PortalPipeWireSource::start(std::string& err) {
    int pw_fd = -1;
    std::uint32_t node_id = 0;
    if (!run_portal(pw_fd, node_id, err)) return false;
    pw_fd_ = pw_fd;

    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("metashare-capture", nullptr);
    if (!loop_) {
        err = "pw_thread_loop_new failed";
        return false;
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        err = "pw_context_new failed";
        return false;
    }

    pw_thread_loop_lock(loop_);
    if (pw_thread_loop_start(loop_) < 0) {
        pw_thread_loop_unlock(loop_);
        err = "pw_thread_loop_start failed";
        return false;
    }
    core_ = pw_context_connect_fd(context_, pw_fd_, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        err = "pw_context_connect_fd failed";
        return false;
    }

    struct pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                          "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    stream_ = pw_stream_new(core_, "metashare-capture", props);

    pw_stream_add_listener(stream_, &stream_listener_, &kStreamEvents, this);

    std::uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    auto rect = SPA_RECTANGLE(1920, 1080);
    auto rmin = SPA_RECTANGLE(1, 1);
    auto rmax = SPA_RECTANGLE(8192, 8192);
    auto frate = SPA_FRACTION(static_cast<std::uint32_t>(opts_.fps_hint), 1);
    auto fmin = SPA_FRACTION(0, 1);
    auto fmax = SPA_FRACTION(240, 1);
    const struct spa_pod* params[1];
    params[0] = reinterpret_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx,
                               SPA_VIDEO_FORMAT_RGBx),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&rect, &rmin, &rmax),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&frate, &fmin, &fmax)));

    if (pw_stream_connect(
            stream_, PW_DIRECTION_INPUT, node_id,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                         PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1) < 0) {
        pw_thread_loop_unlock(loop_);
        err = "pw_stream_connect failed";
        return false;
    }
    pw_thread_loop_unlock(loop_);
    running_ = true;

    // Wait (briefly) for format negotiation so format() is valid on return.
    for (int i = 0; i < 200 && fmt_.width == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (fmt_.width == 0) {
        err = "timed out waiting for PipeWire format negotiation";
        return false;
    }
    return true;
}

void PortalPipeWireSource::stop() {
    running_ = false;
    if (loop_) pw_thread_loop_lock(loop_);
    if (stream_) {
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (loop_) pw_thread_loop_unlock(loop_);
    if (core_) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (loop_) { pw_thread_loop_stop(loop_); }
    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (front_) av_frame_free(&front_);
        if (back_) av_frame_free(&back_);
        if (out_) av_frame_free(&out_);
    }
    // Injection (signaling thread) uses the proxy under input_mu_; taking it
    // here makes teardown safe regardless of pipeline stop order.
    std::lock_guard<std::mutex> ilk(input_mu_);
    input_ready_ = false;
    portal_.reset();  // closes the D-Bus connection + session
}

int PortalPipeWireSource::deliver_front_locked(AVFrame** out,
                                               std::int64_t& pts_usec) {
    if (!front_) return 0;
    // Copy into out_ (which on_process() never touches) so the returned frame
    // stays valid until the next call, per the FrameSource contract — the
    // PipeWire thread may swap and overwrite front_/back_ right after we return.
    if (!out_ || out_->width != front_->width ||
        out_->height != front_->height || out_->format != front_->format) {
        if (out_) av_frame_free(&out_);
        out_ = av_frame_alloc();
        if (!out_) return 0;
        out_->format = front_->format;
        out_->width = front_->width;
        out_->height = front_->height;
        if (av_frame_get_buffer(out_, 32) < 0) {
            av_frame_free(&out_);
            return 0;
        }
    }
    if (av_frame_make_writable(out_) < 0) return 0;
    av_frame_copy(out_, front_);
    *out = out_;
    pts_usec = pts_usec_;
    return 1;
}

int PortalPipeWireSource::next_frame(AVFrame** out, std::int64_t& pts_usec) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(500),
                      [&] { return have_new_ || !running_; }))
        return 0;
    if (!running_) return -1;
    have_new_ = false;
    return deliver_front_locked(out, pts_usec);
}

int PortalPipeWireSource::latest_frame(AVFrame** out, std::int64_t& pts_usec) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return -1;
    // No wait: if nothing has been captured since the last pull, tell the caller
    // to re-encode its previous frame. This is what decouples the encode cadence
    // from the compositor's bursty delivery — the caller ticks on its own clock.
    if (!have_new_) return 0;
    have_new_ = false;
    return deliver_front_locked(out, pts_usec);
}

}  // namespace metashare
