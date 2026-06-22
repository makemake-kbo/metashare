// Synthetic animated frame source: scrolling color bars + a moving box and a
// frame counter region. Lets us exercise the full pipeline with no desktop,
// portal, or PipeWire present.

#pragma once

#include <chrono>

#include "frame_source.hpp"

namespace metashare {

class TestPatternSource final : public FrameSource {
  public:
    explicit TestPatternSource(SourceFormat fmt, int monitor_index = 0);
    ~TestPatternSource() override;

    bool start(std::string& err) override;
    void stop() override;
    SourceFormat format() const override { return fmt_; }
    int next_frame(AVFrame** out, std::int64_t& pts_usec) override;

  private:
    SourceFormat fmt_;
    int monitor_index_;
    AVFrame* frame_ = nullptr;
    std::int64_t frame_index_ = 0;
    std::chrono::steady_clock::time_point start_time_{};
};

}  // namespace metashare
