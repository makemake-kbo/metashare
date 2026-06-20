// Low-latency H.264 encoder built on libavcodec.
//
// Input frames may be any pixel format/size; they are converted to the codec's
// native format (YUV420P) with libswscale as needed. SPS/PPS headers are kept
// in-band and repeated before each keyframe so a client can join the TCP stream
// at any keyframe boundary.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
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
    bool prefer_hardware = true;  // try VAAPI first, fall back to software
};

// Called for each encoded access unit. data is valid only for the call.
using PacketSink = std::function<void(const std::uint8_t* data, std::size_t size,
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

    proto::Codec codec() const { return proto::Codec::kH264; }
    bool using_hardware() const { return hw_ != nullptr; }

private:
    bool drain(const PacketSink& sink, std::string& err);

    EncoderConfig cfg_{};
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* conv_ = nullptr;       // YUV420P scratch frame
    AVPacket* pkt_ = nullptr;
    AVBufferRef* hw_ = nullptr;     // VAAPI device, if hardware path active
    int sws_src_fmt_ = -1;          // last swscale input format negotiated
};

}  // namespace metashare
