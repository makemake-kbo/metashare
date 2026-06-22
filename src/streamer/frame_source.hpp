// Abstract source of raw video frames feeding the encoder.
//
// Two concrete backends exist:
//   * TestPatternSource    — synthetic animated frames; needs no desktop, used
//                            to validate the encode/network/decode pipeline.
//   * PortalPipeWireSource — real Wayland capture via xdg-desktop-portal.
//
// Frames are delivered as FFmpeg AVFrames so the encoder can hand them straight
// to swscale / libavcodec without an extra copy.

#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/frame.h>
}

namespace metashare {

struct SourceFormat {
    int width = 0;
    int height = 0;
    int fps_num = 60;
    int fps_den = 1;
};

class FrameSource {
  public:
    virtual ~FrameSource() = default;

    // Negotiate/begin capture. Returns false on fatal error; err is filled in.
    virtual bool start(std::string& err) = 0;

    // Stop capture and release resources. Safe to call multiple times.
    virtual void stop() = 0;

    // Format becomes valid only after a successful start().
    virtual SourceFormat format() const = 0;

    // Block until the next frame is available and fill *out (a borrowed frame
    // owned by the source, valid until the next call). Returns:
    //   1  -> frame produced
    //   0  -> no frame yet / transient; caller should retry
    //  -1  -> stream ended or fatal error
    virtual int next_frame(AVFrame** out, std::int64_t& pts_usec) = 0;
};

}  // namespace metashare
