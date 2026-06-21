#include "mutter_screencast.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sys/stat.h>
#include <thread>

#include <sdbus-c++/sdbus-c++.h>

namespace metashare {

namespace {

constexpr const char* kMutterService = "org.gnome.Mutter.ScreenCast";
constexpr const char* kMutterPath = "/org/gnome/Mutter/ScreenCast";
constexpr const char* kMutterIface = "org.gnome.Mutter.ScreenCast";
constexpr const char* kMutterSessionIface =
    "org.gnome.Mutter.ScreenCast.Session";
constexpr const char* kMutterStreamIface = "org.gnome.Mutter.ScreenCast.Stream";

using VarMap = std::map<std::string, sdbus::Variant>;

// 0 = HIDDEN, 1 = EMBEDDED (baked into framebuffer), 2 = METADATA.
// EMBEDDED is right for view-only streaming: the cursor is in the pixels.
constexpr std::uint32_t kCursorEmbedded = 1;

}  // namespace

MutterScreenCastSession::MutterScreenCastSession() = default;
MutterScreenCastSession::~MutterScreenCastSession() { close(); }

bool MutterScreenCastSession::check_version(std::string& err) {
    // Use Properties.GetAll instead of Properties.Get — sdbus-c++ v1's
    // storeResultsTo(Variant) mis-deserializes the Get reply's (v) wrapper
    // ("Failed to enter a variant"). GetAll returns a{sv} which deserializes
    // cleanly into std::map<std::string, sdbus::Variant>.
    try {
        auto proxy = sdbus::createProxy(*conn_, std::string{kMutterService},
                                        std::string{kMutterPath});
        std::map<std::string, sdbus::Variant> all;
        proxy->callMethod("GetAll")
            .onInterface("org.freedesktop.DBus.Properties")
            .withArguments(std::string{kMutterIface})
            .storeResultsTo(all);
        auto it = all.find("Version");
        if (it == all.end()) {
            err = "Mutter ScreenCast has no Version property — too old?";
            return false;
        }
        const std::uint32_t version = it->second.get<std::uint32_t>();
        if (version < 4) {
            err = "Mutter ScreenCast API version " + std::to_string(version) +
                  " is too old. MetaShare requires Mutter API v4 (GNOME 44+ "
                  "for the direct PipeWire connection flow).";
            return false;
        }
        std::fprintf(stderr, "[mutter] ScreenCast API version %u\n", version);
        return true;
    } catch (const sdbus::Error& e) {
        err = std::string("cannot read Mutter ScreenCast version: ") + e.what();
        return false;
    }
}

bool MutterScreenCastSession::open(std::string& err) {
    // Pre-flight: we need a session D-Bus. Same check as the portal source.
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        bool bus_socket_present = false;
        if (xdg) {
            std::string sock = std::string(xdg) + "/bus";
            struct stat st;
            bus_socket_present = (::stat(sock.c_str(), &st) == 0);
        }
        if (!bus_socket_present) {
            err = "no D-Bus session bus. Run this from inside a GNOME "
                  "Wayland session.";
            return false;
        }
    }

    try {
        conn_ = sdbus::createSessionBusConnection();
    } catch (const sdbus::Error& e) {
        err = std::string("cannot connect to session bus: ") + e.what();
        return false;
    }
    conn_->enterEventLoopAsync();

    // We skip a version probe here: sdbus-c++ v1's Variant deserializer has
    // trouble with Mutter's Properties.GetAll reply on some installs (errors
    // out with "Failed to enter a variant"). If Mutter is too old to support
    // RecordVirtual (introduced in API v3 / GNOME 41), the RecordVirtual call
    // below will return a clear "UnknownMethod" D-Bus error. The GNOME 44+
    // PipeWire-flow requirement is enforced later when we wait for the
    // PipeWireStreamAdded signal.

    // CreateSession({} -> o session_path)
    try {
        VarMap opts;  // empty options; we don't disable animations
        sdbus::ObjectPath path;
        auto proxy = sdbus::createProxy(*conn_, std::string{kMutterService},
                                        std::string{kMutterPath});
        proxy->callMethod("CreateSession")
            .onInterface(kMutterIface)
            .withArguments(opts)
            .storeResultsTo(path);
        session_path_ = path;
    } catch (const sdbus::Error& e) {
        err = std::string("Mutter CreateSession failed: ") + e.what() +
              " (are you running under GNOME Wayland?)";
        return false;
    }

    open_ = true;
    std::fprintf(stderr, "[mutter] session %s\n", session_path_.c_str());
    return true;
}

int MutterScreenCastSession::add_virtual_monitor(int width, int height,
                                                  std::string& err) {
    if (!open_) { err = "session not open"; return -1; }
    if (width <= 0 || height <= 0) { err = "invalid monitor size"; return -1; }

    const int idx = static_cast<int>(monitors_.size());

    // RecordVirtual properties (a{sv}):
    //   "cursor-mode"  : u  = EMBEDDED
    //   "is-platform"  : b  = true   (treat as a real platform monitor:
    //                                  panel, fullscreen windows, etc.)
    //   "modes"        : aa{sv}      one mode with the requested size
    //
    // Each mode dict:
    //   "size"          : (uu)        required
    //   "is-preferred"  : b           exactly one preferred
    //   "refresh-rate"  : d           optional; we advertise 60 Hz
    VarMap mode;
    mode["size"] = sdbus::Variant(sdbus::Struct<std::uint32_t, std::uint32_t>{
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height)});
    mode["is-preferred"] = sdbus::Variant(true);
    mode["refresh-rate"] = sdbus::Variant(60.0);
    std::vector<VarMap> modes = {mode};

    VarMap props;
    props["cursor-mode"] = sdbus::Variant(kCursorEmbedded);
    props["is-platform"] = sdbus::Variant(true);
    props["modes"] = sdbus::Variant(modes);

    try {
        sdbus::ObjectPath stream_path;
        auto proxy = sdbus::createProxy(*conn_, std::string{kMutterService},
                                        session_path_);
        proxy->callMethod("RecordVirtual")
            .onInterface(kMutterSessionIface)
            .withArguments(props)
            .storeResultsTo(stream_path);

        MonitorInfo m;
        m.width = width;
        m.height = height;
        m.stream_path = stream_path;
        monitors_.push_back(std::move(m));
        std::fprintf(stderr, "[mutter] virtual monitor %d: %dx%d -> %s\n",
                     idx, width, height, monitors_.back().stream_path.c_str());
        return idx;
    } catch (const sdbus::Error& e) {
        err = std::string("Mutter RecordVirtual failed: ") + e.what();
        return -1;
    }
}

bool MutterScreenCastSession::start(std::string& err, int timeout_s) {
    if (!open_) { err = "session not open"; return false; }
    if (monitors_.empty()) { err = "no virtual monitors added"; return false; }

    // Subscribe to PipeWireStreamAdded on each stream BEFORE calling Start().
    // Mutter emits the signal as streams come up after Start(); we must be
    // listening first.
    pending_count_ = static_cast<int>(monitors_.size());
    for (int i = 0; i < static_cast<int>(monitors_.size()); ++i) {
        const int idx = i;  // capture by value
        auto proxy = sdbus::createProxy(*conn_, std::string{kMutterService},
                                        monitors_[i].stream_path);
        proxy->uponSignal("PipeWireStreamAdded")
            .onInterface(kMutterStreamIface)
            .call([this, idx](const std::uint32_t& node_id) {
                std::lock_guard<std::mutex> lk(mu_);
                if (monitors_[idx].node_id == 0) {
                    monitors_[idx].node_id = node_id;
                    --pending_count_;
                    std::fprintf(stderr,
                                 "[mutter] monitor %d: PipeWire node_id=%u\n",
                                 idx, node_id);
                }
            });
        proxy->finishRegistration();
    }

    // Start the session.
    try {
        auto proxy = sdbus::createProxy(*conn_, std::string{kMutterService},
                                        session_path_);
        proxy->callMethod("Start").onInterface(kMutterSessionIface);
    } catch (const sdbus::Error& e) {
        err = std::string("Mutter session.Start failed: ") + e.what();
        return false;
    }

    // Wait for all PipeWireStreamAdded signals.
    for (int i = 0; i < timeout_s * 10; ++i) {
        if (pending_count_.load() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    err = "timed out waiting for PipeWireStreamAdded ("
          + std::to_string(pending_count_.load()) + " of "
          + std::to_string(monitors_.size()) + " pending). "
          + "This usually means Mutter is older than GNOME 44 — the direct "
            "PipeWire connection flow requires GNOME 44+.";
    return false;
}

void MutterScreenCastSession::close() {
    if (!open_) return;
    open_ = false;
    if (!session_path_.empty()) {
        try {
            auto proxy = sdbus::createProxy(*conn_, std::string{kMutterService},
                                            session_path_);
            proxy->callMethod("Stop").onInterface(kMutterSessionIface);
        } catch (...) { /* best effort */ }
    }
    session_path_.clear();
    monitors_.clear();
    conn_.reset();
}

}  // namespace metashare
