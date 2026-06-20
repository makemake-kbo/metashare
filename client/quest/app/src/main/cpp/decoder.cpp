#include "decoder.hpp"

#include <cstring>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

namespace metashare {

Decoder::~Decoder() { close(); }

bool Decoder::open(int width, int height, ANativeWindow* surface,
                   std::string& err) {
    codec_ = AMediaCodec_createDecoderByType("video/avc");
    if (!codec_) {
        err = "AMediaCodec_createDecoderByType(video/avc) failed";
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    // Low-latency decode where the device supports it (Quest 3 does).
    AMediaFormat_setInt32(fmt, "low-latency", 1);

    // Decode straight onto the SurfaceTexture-backed window.
    media_status_t st =
        AMediaCodec_configure(codec_, fmt, surface, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        err = "AMediaCodec_configure failed";
        return false;
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        err = "AMediaCodec_start failed";
        return false;
    }
    started_ = true;
    return true;
}

bool Decoder::feed(const std::uint8_t* data, std::size_t size,
                   std::uint64_t pts_usec, bool keyframe) {
    if (!started_) return false;
    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 10'000);  // 10 ms
    if (idx < 0) return false;  // no input buffer free yet; caller retries
    std::size_t cap = 0;
    std::uint8_t* buf = AMediaCodec_getInputBuffer(codec_, idx, &cap);
    if (!buf || cap < size) {
        AMediaCodec_queueInputBuffer(codec_, idx, 0, 0, pts_usec, 0);
        return false;
    }
    std::memcpy(buf, data, size);
    const uint32_t flags = keyframe ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
    AMediaCodec_queueInputBuffer(codec_, idx, 0, size, pts_usec, flags);
    return true;
}

bool Decoder::drain_to_surface() {
    if (!started_) return false;
    bool rendered = false;
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t out = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (out >= 0) {
            // render=true hands the frame to the Surface (SurfaceTexture).
            AMediaCodec_releaseOutputBuffer(codec_, out, /*render=*/true);
            rendered = true;
        } else if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            break;
        } else {
            // OUTPUT_FORMAT_CHANGED / OUTPUT_BUFFERS_CHANGED: just continue.
            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) continue;
            break;
        }
    }
    return rendered;
}

void Decoder::close() {
    if (codec_) {
        if (started_) AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    started_ = false;
}

}  // namespace metashare
