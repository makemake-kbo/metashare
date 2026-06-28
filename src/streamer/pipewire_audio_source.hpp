// Real PipeWire audio capture.
//
// Two modes, mirroring the channels exposed in the wire protocol:
//   * SystemSinkMonitor — capture the default audio output's monitor stream
//                         (everything the desktop is playing). Uses
//                         PW_KEY_STREAM_CAPTURE_SINK so PipeWire auto-connects
//                         us to the monitor of the default sink.
//   * Microphone        — capture the default source (the user's mic).
//
// Both produce interleaved s16 stereo @ 48 kHz (the Opus encoder's native
// rate, so no resampling is needed on the encode side). Backed by a
// pw_thread_loop with a single stream; chunks are drained in on_process() and
// handed out via next_chunk().
//
// Built only when -Dportal=enabled or -Dmutter=enabled (i.e. libpipewire is
// already a hard requirement). The streamer can be built without audio by
// passing --audio none at runtime; in that case this TU is still linked but
// never instantiated.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio_source.hpp"

#include <spa/utils/hook.h>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_pod;

namespace metashare {

class PipeWireAudioSource final : public AudioSource {
  public:
    enum class Mode {
        kSystemSinkMonitor,
        kMicrophone,
    };

    explicit PipeWireAudioSource(Mode mode, AudioFormat fmt = {48000, 2});
    ~PipeWireAudioSource() override;

    bool start(std::string& err) override;
    void stop() override;
    AudioFormat format() const override { return fmt_; }
    int next_chunk(const std::int16_t** out, std::int64_t& pts_usec) override;

    // Called from the PipeWire thread (public so the C callbacks can reach in).
    void on_process();

  private:
    Mode mode_;
    AudioFormat fmt_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    struct spa_hook stream_listener_;

    // Ring-style back buffer: PipeWire writes back_, next_chunk() drains it.
    // loan_ holds the slice currently handed to the encoder so subsequent
    // on_process() appends can't invalidate the borrowed pointer.
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::int16_t> buffer_;
    std::vector<std::int16_t> loan_;
    std::int64_t pts_usec_ = 0;
    std::atomic<bool> running_{false};
};

}  // namespace metashare
