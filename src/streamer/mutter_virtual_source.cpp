#include "mutter_virtual_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/props.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace metashare {

namespace {

AVPixelFormat spa_to_av(std::uint32_t spa_fmt) {
    switch (spa_fmt) {
    case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
    case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
    case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
    case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
    default: return AV_PIX_FMT_NONE;
    }
}

void cb_param_changed(void* data, std::uint32_t id,
                      const struct spa_pod* param) {
    if (id != SPA_PARAM_Format || !param) return;
    static_cast<MutterVirtualSource*>(data)->on_param_changed(param);
}
void cb_process(void* data) {
    static_cast<MutterVirtualSource*>(data)->on_process();
}
void cb_state_changed(void* data, enum pw_stream_state old,
                      enum pw_stream_state state, const char* error) {
    std::fprintf(stderr, "[pw/mutter-%p] state: %s%s%s\n", data,
                 pw_stream_state_as_string(state), error ? " - " : "",
                 error ? error : "");
}

const struct pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = cb_state_changed,
    .param_changed = cb_param_changed,
    .process = cb_process,
};
}  // namespace

void MutterVirtualSource::on_param_changed(const struct spa_pod* param) {
    struct spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_video ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0) return;

    const int w = info.info.raw.size.width;
    const int h = info.info.raw.size.height;
    negotiated_fmt_ = spa_to_av(info.info.raw.format);
    if (negotiated_fmt_ == AV_PIX_FMT_NONE) {
        std::fprintf(stderr, "[mutter-source %d] unsupported SPA format %u\n",
                     monitor_idx_, info.info.raw.format);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        fmt_.width = w;
        fmt_.height = h;
        for (AVFrame** f : {&front_, &back_}) {
            if (*f) av_frame_free(f);
            *f = av_frame_alloc();
            (*f)->format = negotiated_fmt_;
            (*f)->width = w;
            (*f)->height = h;
            av_frame_get_buffer(*f, 32);
        }
    }
    std::fprintf(stderr, "[mutter-source %d] negotiated %dx%d fmt=%d\n",
                 monitor_idx_, w, h, negotiated_fmt_);

    // Reply with buffer params: request CPU-mappable buffers.
    std::uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[1];
    params[0] =
        reinterpret_cast<const struct spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
                                     (1u << SPA_DATA_MemFd))));
    pw_stream_update_params(stream_, params, 1);
}

void MutterVirtualSource::on_process() {
    struct pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (!b) return;
    struct spa_buffer* sbuf = b->buffer;
    if (sbuf->n_datas >= 1 && sbuf->datas[0].data && negotiated_fmt_ >= 0) {
        std::lock_guard<std::mutex> lk(mu_);
        if (back_) {
            const auto& d = sbuf->datas[0];
            const int src_stride =
                d.chunk->stride ? d.chunk->stride : back_->linesize[0];
            const auto* src =
                static_cast<const std::uint8_t*>(d.data) + d.chunk->offset;
            av_image_copy_plane(
                back_->data[0], back_->linesize[0], src, src_stride,
                std::min(src_stride, back_->linesize[0]), back_->height);
            const auto now =
                std::chrono::steady_clock::now().time_since_epoch();
            pts_usec_ =
                std::chrono::duration_cast<std::chrono::microseconds>(now)
                    .count();
            std::swap(front_, back_);
            have_new_ = true;
            cv_.notify_one();
        }
    }
    pw_stream_queue_buffer(stream_, b);
}

// ===========================================================================
//  FrameSource interface
// ===========================================================================

MutterVirtualSource::MutterVirtualSource(MutterScreenCastSession& session,
                                         int monitor_idx, int fps_hint)
    : session_(session), monitor_idx_(monitor_idx), fps_hint_(fps_hint) {
    fmt_.fps_num = fps_hint;
    fmt_.fps_den = 1;
}

MutterVirtualSource::~MutterVirtualSource() { stop(); }

bool MutterVirtualSource::start(std::string& err) {
    if (monitor_idx_ < 0 ||
        monitor_idx_ >= static_cast<int>(session_.monitors().size())) {
        err = "invalid monitor index";
        return false;
    }
    const auto& mon = session_.monitors()[monitor_idx_];
    if (mon.node_id == 0) {
        err = "monitor " + std::to_string(monitor_idx_) +
              " has no PipeWire node_id (was MutterScreenCastSession::start() "
              "called?)";
        return false;
    }
    const std::uint32_t target_node = mon.node_id;

    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("metashare-mutter", nullptr);
    if (!loop_) {
        err = "pw_thread_loop_new failed";
        return false;
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        err = "pw_context_new failed";
        return false;
    }

    pw_thread_loop_lock(loop_);
    if (pw_thread_loop_start(loop_) < 0) {
        pw_thread_loop_unlock(loop_);
        err = "pw_thread_loop_start failed";
        return false;
    }
    // GNOME 44+ flow: connect to the default PipeWire socket directly. No
    // OpenPipeWireRemote, no fd from a portal.
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(loop_);
        err = "pw_context_connect failed (is pipewire running?)";
        return false;
    }

    struct pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                          "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    stream_ = pw_stream_new(core_, "metashare-mutter", props);

    pw_stream_add_listener(stream_, &stream_listener_, &kStreamEvents, this);

    const std::uint32_t w = static_cast<std::uint32_t>(mon.width);
    const std::uint32_t h = static_cast<std::uint32_t>(mon.height);
    std::uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    auto rect = SPA_RECTANGLE(w, h);
    auto rmin = SPA_RECTANGLE(1, 1);
    auto rmax = SPA_RECTANGLE(8192, 8192);
    auto frate = SPA_FRACTION(static_cast<std::uint32_t>(fps_hint_), 1);
    auto fmin = SPA_FRACTION(0, 1);
    auto fmax = SPA_FRACTION(240, 1);
    const struct spa_pod* params[1];
    params[0] =
        reinterpret_cast<const struct spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA,
                                   SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA,
                                   SPA_VIDEO_FORMAT_BGRx,
                                   SPA_VIDEO_FORMAT_RGBx),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(&rect, &rmin, &rmax),
            SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(&frate, &fmin, &fmax)));

    if (pw_stream_connect(
            stream_, PW_DIRECTION_INPUT, target_node,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                         PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1) < 0) {
        pw_thread_loop_unlock(loop_);
        err =
            "pw_stream_connect failed for node " + std::to_string(target_node);
        return false;
    }
    pw_thread_loop_unlock(loop_);
    running_ = true;

    // Wait (briefly) for format negotiation so format() is valid on return.
    for (int i = 0; i < 200 && fmt_.width == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (fmt_.width == 0) {
        err = "timed out waiting for PipeWire format negotiation on monitor " +
              std::to_string(monitor_idx_);
        return false;
    }
    return true;
}

void MutterVirtualSource::stop() {
    running_ = false;
    cv_.notify_all();
    if (loop_) pw_thread_loop_lock(loop_);
    if (stream_) {
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (loop_) pw_thread_loop_unlock(loop_);
    if (core_) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (loop_) { pw_thread_loop_stop(loop_); }
    if (context_) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }

    std::lock_guard<std::mutex> lk(mu_);
    if (front_) av_frame_free(&front_);
    if (back_) av_frame_free(&back_);
    if (out_) av_frame_free(&out_);
}

int MutterVirtualSource::next_frame(AVFrame** out, std::int64_t& pts_usec) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(500),
                      [&] { return have_new_ || !running_; }))
        return 0;
    if (!running_) return -1;
    have_new_ = false;
    if (!front_) return 0;
    // Copy into out_ (never touched by the PipeWire thread) so the returned
    // frame stays valid until the next call even under a burst of new frames.
    if (!out_ || out_->width != front_->width ||
        out_->height != front_->height || out_->format != front_->format) {
        if (out_) av_frame_free(&out_);
        out_ = av_frame_alloc();
        if (!out_) return 0;
        out_->format = front_->format;
        out_->width = front_->width;
        out_->height = front_->height;
        if (av_frame_get_buffer(out_, 32) < 0) {
            av_frame_free(&out_);
            return 0;
        }
    }
    if (av_frame_make_writable(out_) < 0) return 0;
    av_frame_copy(out_, front_);
    *out = out_;
    pts_usec = pts_usec_;
    return 1;
}

}  // namespace metashare
