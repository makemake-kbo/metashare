// Abstract source of raw PCM audio feeding the audio encoder.
//
// Concrete backends:
//   * TestToneSource        — synthetic 440 Hz sine wave; needs no desktop,
//   used
//                            to validate the encode/network/decode pipeline.
//   * PipeWireAudioSource   — real capture via PipeWire. Two modes:
//                            system (default sink monitor / loopback) or
//                            microphone (default source).
//
// Audio is delivered as interleaved signed 16-bit little-endian PCM
// (AV_SAMPLE_FMT_S16) at the negotiated sample rate and channel count. The
// encoder owns resampling / channel layout conversion if needed.

#pragma once

#include <cstdint>
#include <string>

namespace metashare {

struct AudioFormat {
    int sample_rate = 48000;
    int channels = 2;
};

class AudioSource {
  public:
    virtual ~AudioSource() = default;

    // Negotiate/begin capture. Returns false on fatal error; err is filled in.
    virtual bool start(std::string& err) = 0;

    // Stop capture and release resources. Safe to call multiple times.
    virtual void stop() = 0;

    // Format becomes valid only after a successful start().
    virtual AudioFormat format() const = 0;

    // Block until PCM is available, then loan the caller a contiguous chunk of
    // interleaved s16 samples. The pointer is borrowed and valid only until
    // the next call. Returns:
    //   >0 -> number of s16 *samples* (not frames; = frames * channels)
    //   0  -> nothing yet / transient; caller should retry
    //  -1  -> stream ended or fatal error
    virtual int next_chunk(const std::int16_t** out,
                           std::int64_t& pts_usec) = 0;
};

}  // namespace metashare
