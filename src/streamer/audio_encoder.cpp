#include "audio_encoder.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/opt.h>
}

namespace metashare {

namespace {

std::string av_err(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

// Pick the sample format libopus in this ffmpeg build wants. Recent libavcodec
// libopus wrappers accept interleaved s16 or flt; some older builds accept
// fltp. We just take the codec's first listed format.
AVSampleFormat pick_format(const AVCodec* codec) {
    if (codec && codec->sample_fmts) {
        for (const AVSampleFormat* p = codec->sample_fmts;
             *p != AV_SAMPLE_FMT_NONE; ++p) {
            return *p;
        }
    }
    return AV_SAMPLE_FMT_S16;
}

}  // namespace

AudioEncoder::~AudioEncoder() { close(); }

bool AudioEncoder::open(const AudioEncoderConfig& cfg, std::string& err) {
    cfg_ = cfg;

    const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) {
        err = "libopus encoder not available in this ffmpeg build";
        return false;
    }
    const AVSampleFormat enc_fmt = pick_format(codec);

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        err = "avcodec_alloc_context3 failed";
        return false;
    }

    ctx->sample_rate = cfg.format.sample_rate;
    av_channel_layout_default(&ctx->ch_layout, cfg.format.channels);
    ctx->sample_fmt = enc_fmt;
    ctx->bit_rate = static_cast<std::int64_t>(cfg.bitrate_kbps) * 1000;
    // Drive PTS in microseconds to match the rest of MetaShare.
    ctx->time_base = AVRational{1, 1'000'000};
    // VBR is on by default and gives better quality per bit; for tight CBR
    // we'd set AV_CODEC_FLAG_QSCALE here. 96 kbps stereo Opus is transparent
    // for desktop audio in either mode.
    av_opt_set(ctx->priv_data, "vbr", "on", 0);
    // 20 ms frames, no DTX (DTX creates silence-padding artefacts that break
    // MediaCodec's Opus decoder on Android, which doesn't expect gap fills).
    samples_per_frame_ = cfg.format.sample_rate * cfg.frame_ms / 1000;
    av_opt_set(ctx->priv_data, "frame_size",
               std::to_string(samples_per_frame_).c_str(), 0);
    av_opt_set(ctx->priv_data, "application", "audio", 0);

    int rc = avcodec_open2(ctx, codec, nullptr);
    if (rc < 0) {
        err = std::string("avcodec_open2: ") + av_err(rc);
        avcodec_free_context(&ctx);
        return false;
    }

    // Resample only if the encoder insists on a format we don't already feed
    // (the common path is the source producing s16 and the encoder wanting
    // s16, so swr stays nullptr).
    if (enc_fmt != AV_SAMPLE_FMT_S16) {
        SwrContext* swr = nullptr;
        AVChannelLayout in_layout = {};
        AVChannelLayout out_layout = {};
        av_channel_layout_default(&in_layout, cfg.format.channels);
        av_channel_layout_default(&out_layout, cfg.format.channels);
        int src_rc = swr_alloc_set_opts2(
            &swr, &out_layout, enc_fmt, cfg.format.sample_rate, &in_layout,
            AV_SAMPLE_FMT_S16, cfg.format.sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (src_rc < 0 || !swr) {
            err = std::string("swr_alloc_set_opts2: ") + av_err(src_rc);
            avcodec_free_context(&ctx);
            return false;
        }
        rc = swr_init(swr);
        if (rc < 0) {
            err = std::string("swr_init: ") + av_err(rc);
            swr_free(&swr);
            avcodec_free_context(&ctx);
            return false;
        }
        swr_ = swr;
    }

    AVFrame* frame = av_frame_alloc();
    frame->format = enc_fmt;
    frame->sample_rate = cfg.format.sample_rate;
    av_channel_layout_default(&frame->ch_layout, cfg.format.channels);
    frame->nb_samples = samples_per_frame_;
    rc = av_frame_get_buffer(frame, 0);
    if (rc < 0) {
        err = std::string("av_frame_get_buffer: ") + av_err(rc);
        av_frame_free(&frame);
        if (swr_) swr_free(&swr_);
        avcodec_free_context(&ctx);
        return false;
    }

    ctx_ = ctx;
    enc_frame_ = frame;
    pkt_ = av_packet_alloc();
    if (!pkt_) {
        err = "av_packet_alloc failed";
        close();
        return false;
    }
    return true;
}

void AudioEncoder::close() {
    if (pkt_) av_packet_free(&pkt_);
    if (enc_frame_) av_frame_free(&enc_frame_);
    if (swr_) swr_free(&swr_);
    if (ctx_) avcodec_free_context(&ctx_);
    fifo_.clear();
    pts_set_ = false;
    next_pts_usec_ = 0;
}

bool AudioEncoder::encode(const std::int16_t* data, int sample_count,
                          std::int64_t pts_usec, const AudioPacketSink& sink,
                          std::string& err) {
    if (!ctx_) {
        err = "audio encoder not open";
        return false;
    }
    if (!pts_set_) {
        next_pts_usec_ = pts_usec;
        pts_set_ = true;
    }

    fifo_.insert(fifo_.end(), data, data + sample_count);

    const int frame_samples_total = samples_per_frame_ * cfg_.format.channels;
    while (static_cast<int>(fifo_.size()) >= frame_samples_total) {
        if (av_frame_make_writable(enc_frame_) < 0) {
            err = "audio enc_frame not writable";
            return false;
        }
        const std::int16_t* src = fifo_.data();

        if (swr_) {
            // swr_convert takes nb_samples per channel; src is interleaved s16.
            int converted =
                swr_convert(swr_, enc_frame_->data, samples_per_frame_,
                            reinterpret_cast<const std::uint8_t**>(&src),
                            samples_per_frame_);
            if (converted < 0) {
                err = std::string("swr_convert: ") + av_err(converted);
                return false;
            }
        } else {
            // Encoder takes s16 directly: one interleaved plane in data[0].
            std::memcpy(enc_frame_->data[0], src,
                        static_cast<std::size_t>(frame_samples_total) *
                            sizeof(std::int16_t));
        }
        enc_frame_->pts = next_pts_usec_;
        // Advance wall-clock PTS by exactly one frame's worth of audio.
        next_pts_usec_ += static_cast<std::int64_t>(samples_per_frame_) *
                          1'000'000 / cfg_.format.sample_rate;

        int rc = avcodec_send_frame(ctx_, enc_frame_);
        if (rc < 0) {
            err = std::string("avcodec_send_frame: ") + av_err(rc);
            return false;
        }
        if (!emit_one(sink, err)) return false;

        // Drop the consumed head from the FIFO.
        fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(
                                                       frame_samples_total));
    }
    return true;
}

bool AudioEncoder::emit_one(const AudioPacketSink& sink, std::string& err) {
    for (;;) {
        int rc = avcodec_receive_packet(ctx_, pkt_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) {
            err = std::string("avcodec_receive_packet: ") + av_err(rc);
            return false;
        }
        sink(pkt_->data, static_cast<std::size_t>(pkt_->size), pkt_->pts);
        av_packet_unref(pkt_);
    }
}

void AudioEncoder::flush(const AudioPacketSink& sink) {
    if (!ctx_) return;
    avcodec_send_frame(ctx_, nullptr);
    std::string err;
    emit_one(sink, err);
}

}  // namespace metashare
