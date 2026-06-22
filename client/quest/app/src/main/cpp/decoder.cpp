#include "decoder.hpp"

#include <android/log.h>
#include <cstring>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MetaShare", __VA_ARGS__)

namespace metashare {

Decoder::~Decoder() { close(); }

bool Decoder::open(int width, int height, bool hevc, ANativeWindow* surface,
                   std::string& err) {
    hevc_ = hevc;
    const char* mime = hevc ? "video/hevc" : "video/avc";
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        err =
            std::string("AMediaCodec_createDecoderByType(") + mime + ") failed";
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    // Low-latency decode where the device supports it (Quest 3 does).
    AMediaFormat_setInt32(fmt, "low-latency", 1);

    // Decode straight onto the SurfaceTexture-backed window.
    media_status_t st = AMediaCodec_configure(codec_, fmt, surface, nullptr, 0);
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
    LOG("decoder opened: %s %dx%d", mime, width, height);
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
    // No KEY_FRAME buffer flag in the NDK; the codec infers it from the stream.
    (void)keyframe;
    AMediaCodec_queueInputBuffer(codec_, idx, 0, size, pts_usec, 0);
    if (fed_count_ == 0) {
        LOG("decoder accepted first %s frame (%zu bytes)",
            keyframe ? "key" : "delta", size);
    }
    ++fed_count_;
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
            if (rendered_count_ == 0) {
                LOG("decoder produced first output frame");
            }
            ++rendered_count_;
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
    hevc_ = false;
    fed_count_ = 0;
    rendered_count_ = 0;
}

}  // namespace metashare
