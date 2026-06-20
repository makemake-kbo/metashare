#include "encoder.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace metashare {

namespace {
std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}
}  // namespace

Encoder::~Encoder() { close(); }

bool Encoder::open(const EncoderConfig& cfg, std::string& err) {
    cfg_ = cfg;

    // Software H.264 (libx264). The hardware (VAAPI) path requires uploading
    // frames to GPU surfaces and is tracked as follow-up work; we expose the
    // preference but currently always use the portable software encoder.
    codec_ = avcodec_find_encoder_by_name("libx264");
    if (!codec_) codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec_) {
        err = "no H.264 encoder available (build ffmpeg with libx264)";
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) {
        err = "avcodec_alloc_context3 failed";
        return false;
    }

    ctx_->width = cfg.width;
    ctx_->height = cfg.height;
    // Drive timestamps in microseconds so source pts pass straight through.
    ctx_->time_base = AVRational{1, 1'000'000};
    ctx_->framerate = AVRational{cfg.fps_num, cfg.fps_den};
    ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx_->bit_rate = static_cast<std::int64_t>(cfg.bitrate_kbps) * 1000;
    ctx_->gop_size =
        cfg.keyint_seconds * cfg.fps_num / (cfg.fps_den ? cfg.fps_den : 1);
    ctx_->max_b_frames = 0;
    // Keep SPS/PPS in-band (repeated before keyframes) so late joiners decode.
    ctx_->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);

    int rc = avcodec_open2(ctx_, codec_, nullptr);
    if (rc < 0) {
        err = "avcodec_open2: " + av_err(rc);
        return false;
    }

    conv_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!conv_ || !pkt_) {
        err = "frame/packet alloc failed";
        return false;
    }
    conv_->format = AV_PIX_FMT_YUV420P;
    conv_->width = cfg.width;
    conv_->height = cfg.height;
    if (av_frame_get_buffer(conv_, 32) < 0) {
        err = "conv frame alloc failed";
        return false;
    }
    return true;
}

void Encoder::close() {
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (conv_) av_frame_free(&conv_);
    if (pkt_) av_packet_free(&pkt_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (hw_) av_buffer_unref(&hw_);
    codec_ = nullptr;
    sws_src_fmt_ = -1;
}

bool Encoder::encode(AVFrame* frame, std::int64_t pts_usec,
                     const PacketSink& sink, std::string& err) {
    AVFrame* enc_in = frame;

    // Convert to YUV420P unless the source already provides it at our size.
    const bool needs_convert = frame->format != AV_PIX_FMT_YUV420P ||
                               frame->width != cfg_.width ||
                               frame->height != cfg_.height;
    if (needs_convert) {
        if (!sws_ || sws_src_fmt_ != frame->format) {
            if (sws_) sws_freeContext(sws_);
            sws_ = sws_getContext(
                frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), cfg_.width,
                cfg_.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
                nullptr);
            if (!sws_) {
                err = "sws_getContext failed";
                return false;
            }
            sws_src_fmt_ = frame->format;
        }
        if (av_frame_make_writable(conv_) < 0) {
            err = "conv frame not writable";
            return false;
        }
        sws_scale(sws_, frame->data, frame->linesize, 0, frame->height,
                  conv_->data, conv_->linesize);
        enc_in = conv_;
    }

    enc_in->pts = pts_usec;
    int rc = avcodec_send_frame(ctx_, enc_in);
    if (rc < 0) {
        err = "avcodec_send_frame: " + av_err(rc);
        return false;
    }
    return drain(sink, err);
}

bool Encoder::drain(const PacketSink& sink, std::string& err) {
    for (;;) {
        int rc = avcodec_receive_packet(ctx_, pkt_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) {
            err = "avcodec_receive_packet: " + av_err(rc);
            return false;
        }
        const bool key = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        sink(pkt_->data, static_cast<std::size_t>(pkt_->size),
             pkt_->pts, key);
        av_packet_unref(pkt_);
    }
}

void Encoder::flush(const PacketSink& sink) {
    if (!ctx_) return;
    avcodec_send_frame(ctx_, nullptr);
    std::string err;
    drain(sink, err);
}

}  // namespace metashare
