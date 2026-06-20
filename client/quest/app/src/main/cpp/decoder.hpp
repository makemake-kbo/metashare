// Hardware H.264 decode via the Android NDK MediaCodec API.
//
// MediaCodec decodes directly onto an ANativeWindow (from a SurfaceTexture),
// so decoded frames stay on the GPU as a GL_TEXTURE_EXTERNAL_OES texture that
// the OpenXR renderer samples into the quad-layer swapchain image.
//
// Builds against the Android NDK only (media/NDKMediaCodec.h).

#pragma once

#include <cstdint>
#include <string>

struct AMediaCodec;
struct ANativeWindow;

namespace metashare {

class Decoder {
public:
    Decoder() = default;
    ~Decoder();

    // surface: ANativeWindow from the SurfaceTexture the renderer reads from.
    bool open(int width, int height, ANativeWindow* surface, std::string& err);
    void close();

    // Queue one encoded access unit (Annex-B). pts in microseconds.
    bool feed(const std::uint8_t* data, std::size_t size, std::uint64_t pts_usec,
              bool keyframe);

    // Release any decoded output buffers to the surface; returns true if a new
    // frame was rendered (the SurfaceTexture now has fresh content).
    bool drain_to_surface();

private:
    AMediaCodec* codec_ = nullptr;
    bool started_ = false;
};

}  // namespace metashare
