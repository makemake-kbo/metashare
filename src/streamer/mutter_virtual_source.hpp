// FrameSource that captures one virtual monitor from a MutterScreenCastSession.
//
// One MutterVirtualSource instance per monitor. Each instance owns its own
// PipeWire thread loop and connects to the default PipeWire socket (the GNOME
// 44+ Mutter flow, where Mutter no longer exposes OpenPipeWireRemote).
//
// The MutterScreenCastSession must outlive every source that references it;
// closing the session tears down all streams on the Mutter side.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "frame_source.hpp"
#include "mutter_screencast.hpp"

#include <spa/utils/hook.h>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_pod;

namespace metashare {

class MutterVirtualSource final : public FrameSource {
  public:
    // session must outlive this source. monitor_idx is the index returned by
    // MutterScreenCastSession::add_virtual_monitor().
    MutterVirtualSource(MutterScreenCastSession& session, int monitor_idx,
                        int fps_hint);
    ~MutterVirtualSource() override;

    MutterVirtualSource(const MutterVirtualSource&) = delete;
    MutterVirtualSource& operator=(const MutterVirtualSource&) = delete;

    bool start(std::string& err) override;
    void stop() override;
    SourceFormat format() const override { return fmt_; }
    int next_frame(AVFrame** out, std::int64_t& pts_usec) override;

    int monitor_idx() const { return monitor_idx_; }

    // --- called from the PipeWire thread (public so C callbacks can reach) ---
    void on_param_changed(const struct spa_pod* param);
    void on_process();

  private:
    MutterScreenCastSession& session_;
    int monitor_idx_;
    int fps_hint_;

    SourceFormat fmt_{};
    int negotiated_fmt_ = -1;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    struct spa_hook stream_listener_;  // per-instance; must NOT be static

    // Double buffer: PipeWire fills back_, next_frame() consumes front_.
    // out_ is a consumer-owned staging buffer the PipeWire thread never
    // touches; next_frame() copies front_ into it so the returned frame stays
    // stable until the next call (see PortalPipeWireSource for the rationale).
    std::mutex mu_;
    std::condition_variable cv_;
    AVFrame* front_ = nullptr;
    AVFrame* back_ = nullptr;
    AVFrame* out_ = nullptr;
    bool have_new_ = false;
    std::atomic<bool> running_{false};
    std::int64_t pts_usec_ = 0;
};

}  // namespace metashare
