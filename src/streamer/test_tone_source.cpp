#include "test_tone_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace metashare {

namespace {
constexpr double kAmplitude = 0.2;  // avoid clipping / unpleasantly loud tone
}  // namespace

TestToneSource::TestToneSource(AudioFormat fmt, double frequency)
    : fmt_(fmt), frequency_(frequency) {}

TestToneSource::~TestToneSource() { stop(); }

bool TestToneSource::start(std::string& err) {
    if (fmt_.sample_rate <= 0 || fmt_.channels <= 0) {
        err = "test tone: invalid audio format";
        return false;
    }
    // 20 ms frames: samples per channel = sample_rate / 50. We assume 48 kHz
    // (== 960) but compute it generally so other rates work too.
    const int samples_per_channel = fmt_.sample_rate / 50;
    buffer_.assign(
        static_cast<std::size_t>(samples_per_channel) * fmt_.channels, 0);
    start_time_ = std::chrono::steady_clock::now();
    return true;
}

void TestToneSource::stop() {}

int TestToneSource::next_chunk(const std::int16_t** out,
                               std::int64_t& pts_usec) {
    const double fps = 50.0;  // 20 ms frames
    const auto target =
        start_time_ +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(frame_index_ / fps));
    std::this_thread::sleep_until(target);

    const int samples_per_channel = fmt_.sample_rate / 50;
    const double phase_step = 2.0 * M_PI * frequency_ / fmt_.sample_rate;
    const double t0 = static_cast<double>(frame_index_) * samples_per_channel;
    for (int i = 0; i < samples_per_channel; ++i) {
        const double v = kAmplitude * std::sin((t0 + i) * phase_step);
        const std::int16_t s = static_cast<std::int16_t>(v * 32767.0);
        for (int c = 0; c < fmt_.channels; ++c)
            buffer_[static_cast<std::size_t>(i) * fmt_.channels + c] = s;
    }

    pts_usec = static_cast<std::int64_t>(frame_index_ * 1'000'000.0 / fps);
    *out = buffer_.data();
    ++frame_index_;
    return samples_per_channel * fmt_.channels;
}

}  // namespace metashare
