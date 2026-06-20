// MetaShare Quest 3 client — OpenXR entry point.
//
// This is the device-specific glue and is built only against the Meta OpenXR
// Mobile SDK + Android NDK. It outlines the full lifecycle and the per-frame
// flow; the EGL/GLES swapchain blit and SurfaceTexture creation are marked
// TODO — those depend on headers that ship with the Meta SDK and are wired up
// in Android Studio.
//
// Flow per frame:
//   1. NetStream delivers encoded H.264 access units (background thread).
//   2. Decoder feeds them to MediaCodec, which renders onto a SurfaceTexture.
//   3. We updateTexImage() and blit the external texture into a quad-layer
//      swapchain image.
//   4. Submit one XrCompositionLayerQuad placed ~1.5 m in front of the user —
//      the floating window.

#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <memory>

#include "decoder.hpp"
#include "net_stream.hpp"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MetaShare", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MetaShare", __VA_ARGS__)

namespace metashare {

// The floating window: a quad in front of the user. 1.6 m wide at 1.5 m away is
// a comfortable virtual monitor; height is derived from the stream aspect.
struct QuadWindow {
    XrPosef pose{};         // identity orientation, z = -1.5
    XrExtent2Df size{};     // meters
};

class QuestClient {
public:
    void run(android_app* app);

private:
    bool init_openxr(android_app* app);
    bool init_session_and_swapchain(int tex_w, int tex_h);
    void main_loop(android_app* app);
    void submit_quad_frame(XrTime predicted_display_time);

    // OpenXR handles.
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;       // LOCAL reference space
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    int32_t sw_w_ = 0, sw_h_ = 0;
    bool session_running_ = false;

    QuadWindow window_;

    NetStream net_;
    Decoder decoder_;

    // GL/decode glue (created in Android Studio against the Meta SDK):
    //   GLuint  external_tex_;   // GL_TEXTURE_EXTERNAL_OES MediaCodec renders to
    //   jobject surface_texture_; ANativeWindow* decode_window_;
    //   GLuint  blit_program_;    // samples external_tex_ into the swapchain image
};

bool QuestClient::init_openxr(android_app* app) {
    // Required on Android: initialize the loader with the JavaVM + activity.
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(
                              &xrInitializeLoaderKHR));
    if (xrInitializeLoaderKHR) {
        XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        init.applicationVM = app->activity->vm;
        init.applicationContext = app->activity->clazz;
        xrInitializeLoaderKHR(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init));
    }

    const char* extensions[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };
    XrInstanceCreateInfoAndroidKHR android_info{
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = app->activity->vm;
    android_info.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android_info;
    ci.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    ci.enabledExtensionNames = extensions;
    std::snprintf(ci.applicationInfo.applicationName,
                  sizeof(ci.applicationInfo.applicationName), "MetaShare");
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    if (xrCreateInstance(&ci, &instance_) != XR_SUCCESS) {
        LOGE("xrCreateInstance failed");
        return false;
    }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (xrGetSystem(instance_, &sgi, &system_) != XR_SUCCESS) {
        LOGE("xrGetSystem failed");
        return false;
    }
    LOG("OpenXR instance + system ready");
    return true;
}

bool QuestClient::init_session_and_swapchain(int tex_w, int tex_h) {
    // TODO(device): create an EGL context, then an XrGraphicsBindingOpenGLESAndroidKHR
    // referencing it, and pass it as XrSessionCreateInfo::next. Create a LOCAL
    // XrReferenceSpace for placing the quad. Create an XrSwapchain sized to the
    // stream (tex_w x tex_h) with format GL_SRGB8_ALPHA8 / GL_RGBA8.
    //
    // Also size the floating window from the stream aspect ratio:
    const float width_m = 1.6f;
    window_.size = XrExtent2Df{width_m, width_m * tex_h / float(tex_w)};
    window_.pose.orientation = XrQuaternionf{0, 0, 0, 1};
    window_.pose.position = XrVector3f{0, 0, -1.5f};
    sw_w_ = tex_w;
    sw_h_ = tex_h;
    LOG("quad window %.2fm x %.2fm, swapchain %dx%d", window_.size.width,
        window_.size.height, sw_w_, sw_h_);
    return true;  // stubbed; see Android Studio integration
}

void QuestClient::submit_quad_frame(XrTime predicted_display_time) {
    // 1. Acquire/wait a swapchain image.
    // 2. Blit external_tex_ (MediaCodec output) into it with blit_program_.
    // 3. Release the swapchain image.
    // 4. Compose one quad layer:
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = space_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.pose = window_.pose;
    quad.size = window_.size;
    quad.subImage.swapchain = swapchain_;
    quad.subImage.imageRect =
        XrRect2Di{{0, 0}, {sw_w_, sw_h_}};
    // 5. xrEndFrame with &quad as the single layer.
    (void)predicted_display_time;
}

void QuestClient::main_loop(android_app* app) {
    // Connect to the streamer (discovery first, then TCP).
    std::string err;
    if (!net_.discover(3000)) {
        LOGE("discovery failed; retry or hardcode a host in net_stream");
        return;
    }
    if (!net_.connect_and_run(err)) {
        LOGE("connect failed: %s", err.c_str());
        return;
    }
    const auto& h = net_.header();
    LOG("stream %ux%u", h.width, h.height);

    // TODO(device): create SurfaceTexture + ANativeWindow (decode_window_),
    // then: decoder_.open(h.width, h.height, decode_window_, err);
    init_session_and_swapchain(h.width, h.height);

    while (!app->destroyRequested) {
        // Pump Android events.
        int events;
        android_poll_source* src;
        while (ALooper_pollAll(0, nullptr, &events,
                               reinterpret_cast<void**>(&src)) >= 0)
            if (src) src->process(app, src);

        // Pull encoded frames and feed the decoder.
        EncodedFrame f;
        while (net_.pop_frame(f, 0))
            decoder_.feed(f.data.data(), f.data.size(), f.pts_usec, f.keyframe);
        decoder_.drain_to_surface();  // renders onto the SurfaceTexture

        // TODO(device): xrWaitFrame / xrBeginFrame, updateTexImage(), then:
        //   submit_quad_frame(frameState.predictedDisplayTime);
    }
    net_.stop();
    decoder_.close();
}

void QuestClient::run(android_app* app) {
    if (!init_openxr(app)) return;
    main_loop(app);
    if (instance_) xrDestroyInstance(instance_);
}

}  // namespace metashare

// NativeActivity entry point.
extern "C" void android_main(android_app* app) {
    metashare::QuestClient client;
    client.run(app);
}
