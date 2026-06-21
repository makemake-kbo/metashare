#include "encoder.hpp"

#include <cstring>
#include <vector>

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

// One candidate codec the encoder will try to open, in priority order.
struct Candidate {
    const char* name;             // ffmpeg encoder name
    proto::Codec codec;           // wire codec it produces
    AVHWDeviceType hw_type;       // AV_HWDEVICE_TYPE_NONE if no device needed
    bool hardware;                // true if a HW accelerator (for reporting)
};

// Build the ordered list to probe for a given config. Software H.264 is always
// appended last as the universal fallback so open() can never totally fail on a
// working ffmpeg.
std::vector<Candidate> build_candidates(const EncoderConfig& cfg) {
    std::vector<Candidate> list;
    if (cfg.preferred_codec == proto::Codec::kH265) {
        if (cfg.prefer_hardware) {
            // VAAPI: needs a hw device + surface upload.
            list.push_back({"hevc_vaapi", proto::Codec::kH265,
                            AV_HWDEVICE_TYPE_VAAPI, true});
            // NVENC manages the GPU internally; standard NV12 input is fine.
            list.push_back({"hevc_nvenc", proto::Codec::kH265,
                            AV_HWDEVICE_TYPE_NONE, true});
        }
        // (No libx265 in the chain — by policy we fall back to software H.264
        // rather than software HEVC, since x264 is faster and universally
        // available in our build.)
    }
    list.push_back({"libx264", proto::Codec::kH264, AV_HWDEVICE_TYPE_NONE, false});
    return list;
}

// Software pixel format we feed into the encoder (or upload from, for VAAPI).
AVPixelFormat conv_fmt_for(const char* name) {
    if (std::strstr(name, "nvenc") || std::strstr(name, "vaapi"))
        return AV_PIX_FMT_NV12;
    return AV_PIX_FMT_YUV420P;
}

}  // namespace

Encoder::~Encoder() { close(); }

bool Encoder::open(const EncoderConfig& cfg, std::string& err) {
    cfg_ = cfg;

    for (const Candidate& c : build_candidates(cfg)) {
        const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
        if (!codec) continue;  // not compiled into this ffmpeg; try next

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;

        ctx->width = cfg.width;
        ctx->height = cfg.height;
        // Drive timestamps in microseconds so source pts pass straight through.
        ctx->time_base = AVRational{1, 1'000'000};
        ctx->framerate = AVRational{cfg.fps_num, cfg.fps_den};
        ctx->bit_rate = static_cast<std::int64_t>(cfg.bitrate_kbps) * 1000;
        ctx->gop_size = cfg.keyint_seconds * cfg.fps_num /
                        (cfg.fps_den ? cfg.fps_den : 1);
        ctx->max_b_frames = 0;
        // Keep SPS/PPS/VPS in-band (repeated before keyframes) so late joiners
        // can decode without any out-of-band header exchange.
        ctx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;

        AVBufferRef* hw = nullptr;
        AVBufferRef* hw_frames = nullptr;
        bool ok = true;
        std::string step_err;

        if (c.hw_type != AV_HWDEVICE_TYPE_NONE) {
            int rc = av_hwdevice_ctx_create(&hw, c.hw_type, nullptr, nullptr, 0);
            if (rc < 0) {
                step_err = std::string("hwdevice_ctx_create: ") + av_err(rc);
                ok = false;
            } else {
                ctx->hw_device_ctx = av_buffer_ref(hw);
                // Modern ffmpeg requires the frames pool to be set on the
                // context before open() for VAAPI encoders ("A hardware frames
                // reference is required to associate the encoding device").
                hw_frames = av_hwframe_ctx_alloc(hw);
                if (!hw_frames) {
                    step_err = "av_hwframe_ctx_alloc failed";
                    ok = false;
                } else {
                    auto* fc = reinterpret_cast<AVHWFramesContext*>(
                        hw_frames->data);
                    fc->format = AV_PIX_FMT_VAAPI;
                    fc->sw_format = AV_PIX_FMT_NV12;
                    fc->width = cfg.width;
                    fc->height = cfg.height;
                    rc = av_hwframe_ctx_init(hw_frames);
                    if (rc < 0) {
                        step_err =
                            std::string("av_hwframe_ctx_init: ") + av_err(rc);
                        av_buffer_unref(&hw_frames);
                        ok = false;
                    } else {
                        ctx->hw_frames_ctx = av_buffer_ref(hw_frames);
                    }
                }
                // The only hw_type we use today is VAAPI, which feeds
                // AV_PIX_FMT_VAAPI surfaces directly to the encoder.
                ctx->pix_fmt = AV_PIX_FMT_VAAPI;
            }
        } else {
            ctx->pix_fmt = conv_fmt_for(c.name);
        }

        // Codec-specific low-latency tuning.
        if (ok) {
            if (std::strcmp(c.name, "libx264") == 0) {
                av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
            } else if (std::strcmp(c.name, "hevc_nvenc") == 0) {
                av_opt_set(ctx->priv_data, "preset", "p1", 0);  // fastest
                av_opt_set(ctx->priv_data, "tune", "ull", 0);   // ultra-low-lat
                av_opt_set(ctx->priv_data, "rc", "cbr", 0);
            } else if (std::strcmp(c.name, "hevc_vaapi") == 0) {
                av_opt_set(ctx->priv_data, "rc_mode", "CBR", 0);
                av_opt_set(ctx->priv_data, "quality", "realtime", 0);
            }

            int rc = avcodec_open2(ctx, codec, nullptr);
            if (rc < 0) {
                step_err = std::string("avcodec_open2: ") + av_err(rc);
                ok = false;
            }
        }

        if (!ok) {
            // Try the next candidate.
            if (hw_frames) av_buffer_unref(&hw_frames);
            if (hw) av_buffer_unref(&hw);
            avcodec_free_context(&ctx);
            err += std::string("'") + c.name + "': " + step_err + "; ";
            continue;
        }

        // Commit.
        codec_ = codec;
        ctx_ = ctx;
        hw_ = hw;
        hw_frames_ = hw_frames;
        chosen_name_ = c.name;
        chosen_codec_ = c.codec;
        chosen_hardware_ = c.hardware;
        conv_fmt_ = conv_fmt_for(c.name);

        conv_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        if (!conv_ || !pkt_) {
            err = "scratch frame/packet alloc failed";
            close();
            return false;
        }
        conv_->format = conv_fmt_;
        conv_->width = cfg.width;
        conv_->height = cfg.height;
        if (av_frame_get_buffer(conv_, 32) < 0) {
            err = "conv frame alloc failed";
            close();
            return false;
        }
        return true;
    }

    err = "no encoder could be opened; tried: " + err +
          "(build ffmpeg with libx264 at minimum)";
    return false;
}

void Encoder::close() {
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (conv_) av_frame_free(&conv_);
    if (pkt_) av_packet_free(&pkt_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (hw_frames_) av_buffer_unref(&hw_frames_);
    if (hw_) av_buffer_unref(&hw_);
    codec_ = nullptr;
    sws_src_fmt_ = -1;
    chosen_name_ = "(none)";
    chosen_codec_ = proto::Codec::kH264;
    chosen_hardware_ = false;
}

bool Encoder::encode(AVFrame* frame, std::int64_t pts_usec,
                     const PacketSink& sink, std::string& err) {
    AVFrame* enc_in = frame;

    // Convert to the codec's input format unless the source already matches.
    const bool needs_convert =
        frame->format != conv_fmt_ ||
        frame->width != cfg_.width ||
        frame->height != cfg_.height;
    if (needs_convert) {
        if (!sws_ || sws_src_fmt_ != frame->format) {
            if (sws_) sws_freeContext(sws_);
            sws_ = sws_getContext(
                frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), cfg_.width,
                cfg_.height, conv_fmt_, SWS_BILINEAR, nullptr, nullptr, nullptr);
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

    // VAAPI path: upload the sw frame into a pooled hw surface. We allocate a
    // fresh AVFrame per call (cheap relative to encoding) so we don't have to
    // fight av_frame_unref clearing the hw_frames_ctx back-reference.
    AVFrame* hw_up = nullptr;
    if (hw_frames_) {
        hw_up = av_frame_alloc();
        if (!hw_up) { err = "hw frame alloc failed"; return false; }
        hw_up->format = AV_PIX_FMT_VAAPI;
        hw_up->width = cfg_.width;
        hw_up->height = cfg_.height;
        hw_up->hw_frames_ctx = av_buffer_ref(hw_frames_);
        int rc = av_hwframe_get_buffer(hw_frames_, hw_up, 0);
        if (rc < 0) {
            err = "av_hwframe_get_buffer: " + av_err(rc);
            av_frame_free(&hw_up);
            return false;
        }
        rc = av_hwframe_transfer_data(hw_up, enc_in, 0);
        if (rc < 0) {
            err = "av_hwframe_transfer_data: " + av_err(rc);
            av_frame_free(&hw_up);
            return false;
        }
        hw_up->pts = pts_usec;
        enc_in = hw_up;
    }

    int rc = avcodec_send_frame(ctx_, enc_in);
    if (hw_up) av_frame_free(&hw_up);
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
