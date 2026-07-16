// Real Wayland capture (and, optionally, remote-input injection).
//
//   1. xdg-desktop-portal ScreenCast (D-Bus): CreateSession -> SelectSources ->
//      Start -> OpenPipeWireRemote. The user picks a monitor/window in the
//      portal dialog once; we get back a PipeWire fd + node id.
//   2. PipeWire: connect to that fd, attach a stream to the node, and receive
//      raw frames (negotiated to a packed RGB/BGR format via shared memory).
//
// When PortalOptions::enable_input is set, the session is created through the
// org.freedesktop.portal.RemoteDesktop interface instead (with ScreenCast
// combined onto the same session), which additionally lets us inject pointer
// and keyboard events into the captured surface (InputSink). If the desktop's
// portal can't do that, we transparently fall back to a capture-only
// ScreenCast session and input_sink() stays nullptr.
//
// Frames are copied into an AVFrame and handed to the encoder. Only shared-
// memory (MemPtr) buffers are negotiated for now; DMA-BUF zero-copy import is
// future work.
//
// NOTE: the D-Bus portion uses sdbus-c++ and is the most version-sensitive part
// of the codebase. Built only when `-Dportal=enabled` (or auto-detected deps).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "frame_source.hpp"
#include "input_sink.hpp"

#include <spa/utils/hook.h>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_pod;

namespace metashare {

// Holds the live D-Bus connection + session handle (keeps the screencast alive
// for the streamer's lifetime). Defined in the .cpp to keep sdbus-c++ out of
// this header.
struct PortalState;

// Controls what the portal asks the user for in SelectSources.
//   source_types : bitmask of xdg-desktop-portal AvailableSourceTypes
//                  (1=MONITOR, 2=WINDOW, 4=VIRTUAL).
//   multiple     : let the user pick more than one source (only meaningful
//                  when capturing several existing monitors; VIRTUAL typically
//                  yields one stream per session).
//   cursor_mode  : 0=HIDDEN, 1=EMBEDDED, 2=METADATA.
struct PortalOptions {
    int fps_hint = 60;
    std::uint32_t source_types = 1;  // MONITOR by default
    bool multiple = false;
    std::uint32_t cursor_mode = 2;  // METADATA (cursor as PipeWire metadata)
    // Request a RemoteDesktop session (pointer+keyboard injection). Falls back
    // to a plain capture-only ScreenCast session if the portal refuses.
    bool enable_input = false;
};

class PortalPipeWireSource final : public FrameSource, public InputSink {
  public:
    explicit PortalPipeWireSource(int fps_hint);
    explicit PortalPipeWireSource(const PortalOptions& opts);
    ~PortalPipeWireSource() override;

    bool start(std::string& err) override;
    void stop() override;
    SourceFormat format() const override { return fmt_; }
    int next_frame(AVFrame** out, std::int64_t& pts_usec) override;
    int latest_frame(AVFrame** out, std::int64_t& pts_usec) override;

    // Non-null once a RemoteDesktop session was granted; the sink injects into
    // exactly the surface this source captures.
    InputSink* input_sink() override;

    // --- InputSink (safe from the signaling thread; fire-and-forget) --------
    void pointer_motion(double nx, double ny) override;
    void pointer_button(int evdev_button, bool pressed) override;
    void pointer_scroll(int v_steps, int h_steps) override;
    void key(std::uint32_t keysym, bool pressed) override;

    // --- called from the PipeWire thread (public so C callbacks can reach) ---
    void on_param_changed(const struct spa_pod* param);
    void on_process();

  private:
    // Drives the xdg-desktop-portal dialog; returns a PipeWire fd (owned by
    // caller) and the node id to attach to. Tries a RemoteDesktop session
    // first when opts_.enable_input, falling back to capture-only ScreenCast.
    bool run_portal(int& pw_fd, std::uint32_t& node_id, std::string& err);
    // One portal session negotiation, either combined RemoteDesktop+ScreenCast
    // (with_input) or plain ScreenCast.
    bool run_portal_session(bool with_input, int& pw_fd, std::uint32_t& node_id,
                            std::string& err);

    // Count injection failures; disables remote input after repeated errors.
    void input_error(const char* what);

    // Copy the current front_ into the consumer-owned out_ and hand it back.
    // Caller must hold mu_. Returns 1 on success, 0 if there is nothing to
    // deliver yet or the staging copy failed. Shared by next_frame() (after its
    // blocking wait) and latest_frame() (non-blocking).
    int deliver_front_locked(AVFrame** out, std::int64_t& pts_usec);

    SourceFormat fmt_{};
    PortalOptions opts_;
    std::unique_ptr<PortalState> portal_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    struct spa_hook stream_listener_;  // per-instance; must NOT be static
    int pw_fd_ = -1;

    int negotiated_fmt_ = -1;  // AVPixelFormat once a format is negotiated
    int stride_ = 0;

    // Double buffer: PipeWire fills back_, next_frame() consumes front_.
    // out_ is a third, consumer-owned staging buffer that the PipeWire thread
    // never touches: next_frame() copies front_ into it under mu_ and returns
    // it, so the frame handed to the encoder stays stable until the next call
    // even when PipeWire delivers a burst of new frames (as VIRTUAL monitors
    // do). Without it the encoder can read a buffer being overwritten -> tears.
    std::mutex mu_;
    std::condition_variable cv_;
    AVFrame* front_ = nullptr;
    AVFrame* back_ = nullptr;
    AVFrame* out_ = nullptr;
    bool have_new_ = false;
    std::atomic<bool> running_{false};
    std::int64_t pts_usec_ = 0;

    // Remote-input state. input_mu_ guards the portal proxy against teardown
    // racing an injection from the signaling thread (never take mu_ while
    // holding it). node_id_ is the stream targeted by absolute pointer moves.
    std::mutex input_mu_;
    std::atomic<bool> input_ready_{false};
    std::uint32_t node_id_ = 0;
    std::atomic<int> input_errors_{0};
};

}  // namespace metashare
