// Low-latency video encoder built on libavcodec.
//
// Codec selection happens at open() time: by default we prefer hardware HEVC
// (VAAPI on Intel/AMD, NVENC on NVIDIA) and fall back to software H.264
// (libx264) so the streamer works on any host with a usable ffmpeg. The chosen
// codec is reported via codec() / codec_name() and is what gets written into
// the StreamHeader on the wire.
//
// Input frames may be any pixel format/size; they are converted to the codec's
// native format (NV12 for HEVC encoders, YUV420P for libx264) with libswscale,
// and for VAAPI uploaded to a hardware surface before encoding. SPS/PPS/VPS
// headers are kept in-band and repeated before each keyframe so a client can
// join the TCP stream at any keyframe boundary.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include "protocol.hpp"

namespace metashare {

struct EncoderConfig {
    int width = 0;
    int height = 0;
    int fps_num = 60;
    int fps_den = 1;
    int bitrate_kbps = 15000;
    int keyint_seconds = 2;
    bool prefer_hardware = true;  // try VAAPI/NVENC before software
    // What the streamer would *like* to send. Hardware HEVC is attempted first;
    // if every candidate at or above this codec fails we fall through to the
    // universal software H.264 (libx264) path.
    proto::Codec preferred_codec = proto::Codec::kH265;
};

// Called for each encoded access unit. data is valid only for the call.
using PacketSink =
    std::function<void(const std::uint8_t* data, std::size_t size,
                       std::int64_t pts_usec, bool keyframe)>;

class Encoder {
  public:
    Encoder() = default;
    ~Encoder();

    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    bool open(const EncoderConfig& cfg, std::string& err);
    void close();

    // Encode one frame (any format); emits zero or more packets to sink.
    bool encode(AVFrame* frame, std::int64_t pts_usec, const PacketSink& sink,
                std::string& err);

    // Flush buffered packets at shutdown.
    void flush(const PacketSink& sink);

    // What we actually opened. codec() feeds the StreamHeader on the wire.
    proto::Codec codec() const { return chosen_codec_; }
    const char* codec_name() const { return chosen_name_; }
    bool using_hardware() const { return chosen_hardware_; }

  private:
    bool drain(const PacketSink& sink, std::string& err);

    EncoderConfig cfg_{};
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* conv_ = nullptr;  // sw intermediate (NV12 or YUV420P)
    AVPacket* pkt_ = nullptr;
    AVBufferRef* hw_ = nullptr;         // VAAPI device ctx, if active
    AVBufferRef* hw_frames_ = nullptr;  // pool for uploading sw frames to VAAPI
    int sws_src_fmt_ = -1;              // last swscale input format negotiated
    AVPixelFormat conv_fmt_ = AV_PIX_FMT_YUV420P;
    proto::Codec chosen_codec_ = proto::Codec::kH264;
    const char* chosen_name_ = "(none)";
    bool chosen_hardware_ = false;
};

}  // namespace metashare
