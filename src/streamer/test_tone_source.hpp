// Synthetic 440 Hz stereo sine-wave source for testing the audio pipeline end
// to end without any desktop audio stack. Equivalent to TestPatternSource on
// the video side.

#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "audio_source.hpp"

namespace metashare {

class TestToneSource final : public AudioSource {
  public:
    explicit TestToneSource(AudioFormat fmt, double frequency = 440.0);
    ~TestToneSource() override;

    bool start(std::string& err) override;
    void stop() override;
    AudioFormat format() const override { return fmt_; }
    int next_chunk(const std::int16_t** out, std::int64_t& pts_usec) override;

  private:
    AudioFormat fmt_;
    double frequency_;
    // 20 ms worth of s16 samples — matches one Opus frame at 48 kHz so the
    // encoder drains a full packet every call without internal buffering.
    std::vector<std::int16_t> buffer_;
    std::int64_t frame_index_ = 0;
    std::chrono::steady_clock::time_point start_time_{};
};

}  // namespace metashare
