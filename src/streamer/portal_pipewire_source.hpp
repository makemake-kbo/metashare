// Real Wayland capture.
//
//   1. xdg-desktop-portal ScreenCast (D-Bus): CreateSession -> SelectSources ->
//      Start -> OpenPipeWireRemote. The user picks a monitor/window in the
//      portal dialog once; we get back a PipeWire fd + node id.
//   2. PipeWire: connect to that fd, attach a stream to the node, and receive
//      raw frames (negotiated to a packed RGB/BGR format via shared memory).
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

class PortalPipeWireSource final : public FrameSource {
public:
    explicit PortalPipeWireSource(int fps_hint);
    ~PortalPipeWireSource() override;

    bool start(std::string& err) override;
    void stop() override;
    SourceFormat format() const override { return fmt_; }
    int next_frame(AVFrame** out, std::int64_t& pts_usec) override;

    // --- called from the PipeWire thread (public so C callbacks can reach) ---
    void on_param_changed(const struct spa_pod* param);
    void on_process();

private:
    // Drives the xdg-desktop-portal ScreenCast dialog; returns a PipeWire fd
    // (owned by caller) and the node id to attach to.
    bool run_portal(int& pw_fd, std::uint32_t& node_id, std::string& err);

    SourceFormat fmt_{};
    int fps_hint_ = 60;
    std::unique_ptr<PortalState> portal_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    int pw_fd_ = -1;

    int negotiated_fmt_ = -1;  // AVPixelFormat once a format is negotiated
    int stride_ = 0;

    // Double buffer: PipeWire fills back_, next_frame() consumes front_.
    std::mutex mu_;
    std::condition_variable cv_;
    AVFrame* front_ = nullptr;
    AVFrame* back_ = nullptr;
    bool have_new_ = false;
    std::atomic<bool> running_{false};
    std::int64_t pts_usec_ = 0;
};

}  // namespace metashare
