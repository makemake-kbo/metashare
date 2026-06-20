#include "test_pattern_source.hpp"

#include <thread>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace metashare {

namespace {
// Paint one BGRA frame: vertical color bars scrolling horizontally over time,
// plus a box bouncing across the surface so motion (and thus inter-frame delta)
// is visible on the client.
void paint(AVFrame* f, std::int64_t idx) {
    const int w = f->width;
    const int h = f->height;
    auto* base = f->data[0];
    const int stride = f->linesize[0];

    const int scroll = static_cast<int>(idx * 4);
    const int box = 80;
    const int bx = (w - box) > 0 ? static_cast<int>((idx * 6) % (w - box)) : 0;
    const int by = (h - box) > 0
                       ? static_cast<int>((idx * 4) % (2 * (h - box))) : 0;
    const int byy = by < (h - box) ? by : (2 * (h - box) - by);

    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = base + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const int bar = ((x + scroll) / 64) % 6;
            std::uint8_t r = 0, g = 0, b = 0;
            switch (bar) {
                case 0: r = 255; break;
                case 1: g = 255; break;
                case 2: b = 255; break;
                case 3: r = g = 255; break;
                case 4: g = b = 255; break;
                default: r = b = 255; break;
            }
            // Fade brightness with vertical position for a subtle gradient.
            const int shade = 60 + (y * 195) / (h ? h : 1);
            r = static_cast<std::uint8_t>(r * shade / 255);
            g = static_cast<std::uint8_t>(g * shade / 255);
            b = static_cast<std::uint8_t>(b * shade / 255);

            const bool in_box =
                x >= bx && x < bx + box && y >= byy && y < byy + box;
            if (in_box) { r = g = b = 255; }

            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            px[0] = b;  // BGRA
            px[1] = g;
            px[2] = r;
            px[3] = 255;
        }
    }
}
}  // namespace

TestPatternSource::TestPatternSource(SourceFormat fmt) : fmt_(fmt) {}

TestPatternSource::~TestPatternSource() { stop(); }

bool TestPatternSource::start(std::string& err) {
    if (fmt_.width <= 0 || fmt_.height <= 0) {
        err = "test pattern: invalid dimensions";
        return false;
    }
    frame_ = av_frame_alloc();
    if (!frame_) {
        err = "test pattern: av_frame_alloc failed";
        return false;
    }
    frame_->format = AV_PIX_FMT_BGRA;
    frame_->width = fmt_.width;
    frame_->height = fmt_.height;
    if (av_frame_get_buffer(frame_, 32) < 0) {
        err = "test pattern: av_frame_get_buffer failed";
        av_frame_free(&frame_);
        return false;
    }
    start_time_ = std::chrono::steady_clock::now();
    return true;
}

void TestPatternSource::stop() {
    if (frame_) av_frame_free(&frame_);
}

int TestPatternSource::next_frame(AVFrame** out, std::int64_t& pts_usec) {
    if (!frame_) return -1;

    // Pace to the requested frame rate using a real clock so playback looks
    // natural and we don't busy-spin the encoder.
    const double fps =
        static_cast<double>(fmt_.fps_num) / (fmt_.fps_den ? fmt_.fps_den : 1);
    const auto target =
        start_time_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>(frame_index_ / fps));
    std::this_thread::sleep_until(target);

    if (av_frame_make_writable(frame_) < 0) return -1;
    paint(frame_, frame_index_);

    pts_usec = static_cast<std::int64_t>(frame_index_ * 1'000'000.0 / fps);
    frame_->pts = pts_usec;
    *out = frame_;
    ++frame_index_;
    return 1;
}

}  // namespace metashare
