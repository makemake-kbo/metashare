// Opus audio encoder built on libavcodec's `libopus` wrapper.
//
// ffmpeg has no HW Opus encoder on any platform we target — Opus is software
// everywhere in practice — so there's no HW/SW fallback ladder here like the
// video Encoder has. We just open `libopus` directly.
//
// Input is interleaved s16 PCM. libswresample converts it only when needed.
// Capture and encoding default to matching 10 ms chunks.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
}

#include "audio_source.hpp"

namespace metashare {

struct AudioEncoderConfig {
    AudioFormat format;
    int bitrate_kbps = 96;  // Opus default; matches the wire protocol default
    int frame_ms = kAudioFrameMs;  // integer durations: 5/10/20/40/60 ms
};

// Called for each encoded Opus packet. data is valid only for the duration of
// the call. pts_usec advances by the configured packet duration.
using AudioPacketSink = std::function<void(
    const std::uint8_t* data, std::size_t size, std::int64_t pts_usec)>;

class AudioEncoder {
  public:
    AudioEncoder() = default;
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    bool open(const AudioEncoderConfig& cfg, std::string& err);
    void close();

    // Encode a chunk of interleaved s16 samples (any size; we internally
    // accumulate and emit one Opus packet per frame_ms worth of audio).
    // `pts_usec` is the timestamp of the *first* sample in `data`.
    bool encode(const std::int16_t* data, int sample_count,
                std::int64_t pts_usec, const AudioPacketSink& sink,
                std::string& err);

    // Flush any buffered samples at shutdown (typically a partial frame).
    void flush(const AudioPacketSink& sink);

    const char* codec_name() const { return "libopus"; }

  private:
    bool emit_one(const AudioPacketSink& sink, std::string& err);

    AudioEncoderConfig cfg_{};
    AVCodecContext* ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* enc_frame_ = nullptr;  // planar float, holds one Opus frame
    AVPacket* pkt_ = nullptr;

    // FIFO of interleaved s16 samples waiting for the next frame boundary.
    std::vector<std::int16_t> fifo_;
    std::int64_t next_pts_usec_ = 0;  // pts of the next Opus frame to emit
    bool pts_set_ = false;
    int samples_per_frame_ = 0;  // per channel (e.g. 960 @ 48 kHz / 20 ms)
};

}  // namespace metashare
