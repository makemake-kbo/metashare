#include "test_pattern_source.hpp"

#include <thread>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace metashare {

namespace {

// 5-pixel-wide font for digits 0-9 and 'M'. Each entry is 7 rows of 5 bits.
// Bit (1 << (4-x)) set = pixel on.
struct Glyph { std::uint8_t rows[7]; };

// fmt-check: unused-function, keep it even if compiler thinks otherwise.
static const Glyph kFont[] = {
    ['0' - '0'] = {{0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    ['1' - '0'] = {{0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    ['2' - '0'] = {{0b01110, 0b10001, 0b00001, 0b00110, 0b01000, 0b10000, 0b11111}},
    ['3' - '0'] = {{0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    ['4' - '0'] = {{0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    ['5' - '0'] = {{0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    ['6' - '0'] = {{0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    ['7' - '0'] = {{0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    ['8' - '0'] = {{0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    ['9' - '0'] = {{0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
};

void draw_label(std::uint8_t* base, int stride, int w, int h, int mon) {
    char text[8];
    std::snprintf(text, sizeof(text), "M%d", mon);

    const int scale = std::max(4, std::min(w, h) / 80);  // pixel size per dot
    const int gw = 5 * scale;      // glyph width
    const int gh = 7 * scale;      // glyph height
    const int gap = 2 * scale;     // gap between glyphs
    const int margin = std::max(8, w / 40);

    int x0 = margin;
    int y0 = margin;

    // Background rectangle.
    int total_w = 0;
    for (char* p = text; *p; ++p) total_w += gw + gap;
    total_w -= gap;
    for (int y = y0 - scale; y < y0 + gh + scale && y < h; ++y) {
        for (int x = x0 - scale; x < x0 + total_w + scale && x < w; ++x) {
            if (x < 0 || y < 0) continue;
            auto* px = base + (std::size_t)y * stride + (std::size_t)x * 4;
            px[0] = 0; px[1] = 0; px[2] = 0; px[3] = 255;
        }
    }

    for (char* p = text; *p; ++p) {
        char c = *p;
        const Glyph* g = nullptr;
        if (c == 'M') {
            // Custom M
            static const Glyph M = {{0b10001, 0b11011, 0b10101, 0b10001, 0b10001, 0b10001, 0b10001}};
            g = &M;
        } else if (c >= '0' && c <= '9') {
            g = &kFont[c - '0'];
        }
        if (!g) { x0 += gw + gap; continue; }
        for (int gy = 0; gy < 7; ++gy) {
            for (int gx = 0; gx < 5; ++gx) {
                if (!(g->rows[gy] & (1 << (4 - gx)))) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        int px_x = x0 + gx * scale + dx;
                        int px_y = y0 + gy * scale + dy;
                        if (px_x < 0 || px_x >= w || px_y < 0 || px_y >= h) continue;
                        auto* px = base + (std::size_t)px_y * stride + (std::size_t)px_x * 4;
                        px[0] = 0;   // B
                        px[1] = 255; // G
                        px[2] = 255; // R - cyan
                        px[3] = 255;
                    }
                }
            }
        }
        x0 += gw + gap;
    }
}

// Paint one BGRA frame: vertical color bars scrolling horizontally over time,
// plus a box bouncing across the surface so motion (and thus inter-frame delta)
// is visible on the client.  A large monitor index label is drawn top-left so
// multiple test streams are visually distinguishable.
void paint(AVFrame* f, std::int64_t idx, int mon) {
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

    // Each monitor gets a different base hue.
    const int hue = mon % 6;

    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = base + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const int bar = ((x + scroll) / 64) % 6;
            std::uint8_t r = 0, g = 0, b = 0;
            // Blend the per-monitor hue into the color bar pattern.
            const int effbar = (bar + hue) % 6;
            switch (effbar) {
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

    // Draw a large monitor label "M0", "M1", etc. top-left.
    draw_label(base, stride, w, h, mon);
}
}  // namespace

TestPatternSource::TestPatternSource(SourceFormat fmt, int monitor_index)
    : fmt_(fmt), monitor_index_(monitor_index) {}

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
    paint(frame_, frame_index_, monitor_index_);

    pts_usec = static_cast<std::int64_t>(frame_index_ * 1'000'000.0 / fps);
    frame_->pts = pts_usec;
    *out = frame_;
    ++frame_index_;
    return 1;
}

}  // namespace metashare
