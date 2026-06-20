// MetaShare Quest 3 client — OpenXR entry point.
//
// Connects the full pipeline end-to-end:
//   1. NetStream (background thread) discovers the host, reads FrameHeader+NALs
//      off the TCP socket and queues encoded access units.
//   2. Decoder feeds them to MediaCodec, which renders onto a SurfaceTexture.
//   3. Renderer::update_texture() pulls the latest decoded buffer into a
//      GL_TEXTURE_EXTERNAL_OES texture and blits it into the OpenXR quad-layer
//      swapchain image.
//   4. One XrCompositionLayerQuad is submitted per frame — the floating window.
//
// Interaction: holding either controller's trigger grabs the window and drags
// it around (translation only; orientation stays fixed) so you can place the
// virtual monitor wherever is comfortable.
//
// Built only against the Meta OpenXR Mobile SDK + Android NDK (see CMakeLists).

#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "decoder.hpp"
#include "egl.hpp"
#include "net_stream.hpp"
#include "renderer.hpp"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MetaShare", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "MetaShare", __VA_ARGS__)

namespace metashare {

namespace {

// A 1.6 m wide panel at 1.5 m is a comfortable "virtual monitor".
constexpr float kDefaultWindowWidth = 1.6f;
constexpr float kDefaultWindowDistance = 1.5f;
constexpr float kGrabThreshold = 0.6f;  // trigger value counted as "held"

XrVector3f add(XrVector3f a, XrVector3f b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
XrVector3f sub(XrVector3f a, XrVector3f b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

std::string xr_result_string(XrInstance instance, XrResult result) {
    char text[XR_MAX_RESULT_STRING_SIZE] = {0};
    if (instance != XR_NULL_HANDLE &&
        xrResultToString(instance, result, text) == XR_SUCCESS) {
        return text;
    }
    char fallback[32] = {0};
    std::snprintf(fallback, sizeof(fallback), "%d", result);
    return fallback;
}

}  // namespace

class QuestClient {
public:
    void run(android_app* app);

private:
    bool init_openxr(android_app* app);
    bool create_session_and_actions();
    bool create_actions();
    bool init_passthrough();
    void destroy_passthrough();
    bool create_stream_assets();  // swapchain + renderer + decoder
    void destroy_stream_assets();
    void poll_xr_events();        // updates session_running_/should_exit_
    void sync_and_handle_grab(XrTime time);
    bool trigger_held(XrPath hand);
    bool locate_hand(XrPath hand, XrTime time, XrPosef& out);
    void render_frame();

    android_app* app_ = nullptr;

    // EGL + OpenXR core.
    EglContext egl_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR_ = nullptr;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;  // LOCAL stage the quad lives in
    bool session_running_ = false;
    bool should_exit_ = false;

    // Optional Quest passthrough background. This keeps the OpenXR app immersive,
    // but makes the quad feel like a monitor in the room instead of a black void.
    bool passthrough_enabled_ = false;
    bool passthrough_ready_ = false;
    XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthrough_layer_ = XR_NULL_HANDLE;
    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
    PFN_xrDestroyPassthroughFB xrDestroyPassthroughFB_ = nullptr;
    PFN_xrPassthroughStartFB xrPassthroughStartFB_ = nullptr;
    PFN_xrPassthroughPauseFB xrPassthroughPauseFB_ = nullptr;
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
    PFN_xrDestroyPassthroughLayerFB xrDestroyPassthroughLayerFB_ = nullptr;
    PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB_ = nullptr;
    PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB_ = nullptr;
    PFN_xrPassthroughLayerSetStyleFB xrPassthroughLayerSetStyleFB_ = nullptr;

    // Quad-layer swapchain.
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> images_;
    int32_t sw_w_ = 0, sw_h_ = 0;

    // The floating window.
    XrPosef window_pose_{{0, 0, 0, 1}, {0, 0, 0}};

    // Controller grab-to-move.
    XrActionSet action_set_ = XR_NULL_HANDLE;
    XrAction pose_action_ = XR_NULL_HANDLE;
    XrAction grab_action_ = XR_NULL_HANDLE;
    XrPath left_hand_ = XR_NULL_PATH, right_hand_ = XR_NULL_PATH;
    XrSpace left_space_ = XR_NULL_HANDLE, right_space_ = XR_NULL_HANDLE;
    enum Grabber { kNone, kLeft, kRight } grabber_ = kNone;
    XrVector3f grab_offset_{};

    // Stream + decode.
    NetStream net_;
    Decoder decoder_;
    Renderer renderer_;
    std::thread connector_;
    std::atomic<bool> stopping_{false};
    bool assets_ready_ = false;
    bool asset_creation_failed_ = false;
};

// -----------------------------------------------------------------------------
// OpenXR bootstrap
// -----------------------------------------------------------------------------

bool QuestClient::init_openxr(android_app* app) {
    // On Android the loader must be initialized with the JVM/activity before
    // any other OpenXR call.
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(
                              &xrInitializeLoaderKHR));
    if (!xrInitializeLoaderKHR) {
        LOGE("xrInitializeLoaderKHR not available");
        return false;
    }
    XrLoaderInitInfoAndroidKHR loader{
        XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader.applicationVM = app->activity->vm;
    loader.applicationContext = app->activity->clazz;
    if (xrInitializeLoaderKHR(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader)) !=
        XR_SUCCESS) {
        LOGE("xrInitializeLoaderKHR failed");
        return false;
    }

    auto extension_available = [](const char* name) {
        uint32_t count = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count,
                                                             nullptr))) {
            return false;
        }
        std::vector<XrExtensionProperties> props(
            count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(
                nullptr, count, &count, props.data()))) {
            return false;
        }
        for (const XrExtensionProperties& prop : props) {
            if (std::strcmp(prop.extensionName, name) == 0) return true;
        }
        return false;
    };

    std::vector<const char*> extensions = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    passthrough_enabled_ =
        extension_available(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (passthrough_enabled_) {
        extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
        LOG("OpenXR passthrough extension available");
    }

    XrInstanceCreateInfoAndroidKHR android_ci{
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_ci.applicationVM = app->activity->vm;
    android_ci.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android_ci;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.enabledExtensionNames = extensions.data();
    std::snprintf(ci.applicationInfo.applicationName,
                  sizeof(ci.applicationInfo.applicationName), "MetaShare");
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    XrResult xr = xrCreateInstance(&ci, &instance_);
    if (XR_FAILED(xr)) {
        LOGE("xrCreateInstance failed: %d", xr);
        return false;
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (xrGetSystem(instance_, &sgi, &system_) != XR_SUCCESS) {
        LOGE("xrGetSystem failed");
        return false;
    }

    xrGetInstanceProcAddr(
        instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(
            &xrGetOpenGLESGraphicsRequirementsKHR_));
    if (xrGetOpenGLESGraphicsRequirementsKHR_) {
        XrGraphicsRequirementsOpenGLESKHR req{
            XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        xrGetOpenGLESGraphicsRequirementsKHR_(instance_, system_, &req);
        LOG("OpenXR GLES req: min %u.%u",
            static_cast<unsigned>(req.minApiVersionSupported >> 48),
            static_cast<unsigned>((req.minApiVersionSupported >> 32) & 0xFFFF));
    }

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, system_, &sp);
    LOG("OpenXR system: %s", sp.systemName);
    return true;
}

bool QuestClient::create_actions() {
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(asi.actionSetName, sizeof(asi.actionSetName), "main");
    std::snprintf(asi.localizedActionSetName, sizeof(asi.localizedActionSetName),
                  "Main");
    if (xrCreateActionSet(instance_, &asi, &action_set_) != XR_SUCCESS) {
        LOGE("xrCreateActionSet failed");
        return false;
    }

    xrStringToPath(instance_, "/user/hand/left", &left_hand_);
    xrStringToPath(instance_, "/user/hand/right", &right_hand_);
    XrPath subs[2] = {left_hand_, right_hand_};

    XrActionCreateInfo paci{XR_TYPE_ACTION_CREATE_INFO};
    paci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::snprintf(paci.actionName, sizeof(paci.actionName), "hand_pose");
    std::snprintf(paci.localizedActionName, sizeof(paci.localizedActionName),
                  "Hand Pose");
    paci.countSubactionPaths = 2;
    paci.subactionPaths = subs;
    if (xrCreateAction(action_set_, &paci, &pose_action_) != XR_SUCCESS) {
        LOGE("create pose action failed");
        return false;
    }

    XrActionCreateInfo gaci{XR_TYPE_ACTION_CREATE_INFO};
    gaci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::snprintf(gaci.actionName, sizeof(gaci.actionName), "grab");
    std::snprintf(gaci.localizedActionName, sizeof(gaci.localizedActionName),
                  "Grab");
    gaci.countSubactionPaths = 2;
    gaci.subactionPaths = subs;
    if (xrCreateAction(action_set_, &gaci, &grab_action_) != XR_SUCCESS) {
        LOGE("create grab action failed");
        return false;
    }

    // Suggest bindings for the Oculus/Meta Touch controller profile.
    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance_, s, &p);
        return p;
    };
    XrActionSuggestedBinding binds[4] = {
        {pose_action_, path("/user/hand/left/input/aim/pose")},
        {pose_action_, path("/user/hand/right/input/aim/pose")},
        {grab_action_, path("/user/hand/left/input/trigger/value")},
        {grab_action_, path("/user/hand/right/input/trigger/value")},
    };
    XrInteractionProfileSuggestedBinding sb{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller",
                   &sb.interactionProfile);
    sb.countSuggestedBindings = 4;
    sb.suggestedBindings = binds;
    xrSuggestInteractionProfileBindings(instance_, &sb);

    // An action space per hand, anchored in the LOCAL stage.
    auto make_space = [&](XrPath hand, XrSpace& out) {
        XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asci.action = pose_action_;
        asci.subactionPath = hand;
        asci.poseInActionSpace.orientation = {0, 0, 0, 1};
        xrCreateActionSpace(session_, &asci, &out);
    };
    make_space(left_hand_, left_space_);
    make_space(right_hand_, right_space_);

    XrSessionActionSetsAttachInfo attach{
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    XrActionSet sets[1] = {action_set_};
    attach.countActionSets = 1;
    attach.actionSets = sets;
    xrAttachSessionActionSets(session_, &attach);
    return true;
}

bool QuestClient::create_session_and_actions() {
    XrGraphicsBindingOpenGLESAndroidKHR gb{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = egl_.display;
    gb.config = egl_.config;
    gb.context = egl_.context;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system_;
    if (xrCreateSession(instance_, &sci, &session_) != XR_SUCCESS) {
        LOGE("xrCreateSession failed");
        return false;
    }

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    if (xrCreateReferenceSpace(session_, &rsci, &space_) != XR_SUCCESS) {
        LOGE("xrCreateReferenceSpace failed");
        return false;
    }
    if (!init_passthrough()) {
        LOG("passthrough unavailable; using opaque OpenXR background");
    }
    return create_actions();
}

bool QuestClient::init_passthrough() {
    if (!passthrough_enabled_) return false;

    auto load = [&](const char* name, PFN_xrVoidFunction* out) {
        XrResult xr = xrGetInstanceProcAddr(instance_, name, out);
        if (XR_FAILED(xr) || !*out) {
            LOGE("%s unavailable: %s", name,
                 xr_result_string(instance_, xr).c_str());
            return false;
        }
        return true;
    };

    if (!load("xrCreatePassthroughFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrCreatePassthroughFB_)) ||
        !load("xrDestroyPassthroughFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyPassthroughFB_)) ||
        !load("xrPassthroughStartFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughStartFB_)) ||
        !load("xrPassthroughPauseFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughPauseFB_)) ||
        !load("xrCreatePassthroughLayerFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrCreatePassthroughLayerFB_)) ||
        !load("xrDestroyPassthroughLayerFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyPassthroughLayerFB_)) ||
        !load("xrPassthroughLayerResumeFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerResumeFB_)) ||
        !load("xrPassthroughLayerPauseFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerPauseFB_)) ||
        !load("xrPassthroughLayerSetStyleFB",
              reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerSetStyleFB_))) {
        passthrough_enabled_ = false;
        return false;
    }

    XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XrResult xr = xrCreatePassthroughFB_(session_, &pci, &passthrough_);
    if (XR_FAILED(xr)) {
        LOGE("xrCreatePassthroughFB failed: %s",
             xr_result_string(instance_, xr).c_str());
        passthrough_enabled_ = false;
        return false;
    }

    XrPassthroughLayerCreateInfoFB lci{
        XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    lci.passthrough = passthrough_;
    lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    xr = xrCreatePassthroughLayerFB_(session_, &lci, &passthrough_layer_);
    if (XR_FAILED(xr)) {
        LOGE("xrCreatePassthroughLayerFB failed: %s",
             xr_result_string(instance_, xr).c_str());
        destroy_passthrough();
        passthrough_enabled_ = false;
        return false;
    }

    XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
    style.textureOpacityFactor = 1.0f;
    style.edgeColor = {0, 0, 0, 0};
    xrPassthroughLayerSetStyleFB_(passthrough_layer_, &style);
    xrPassthroughStartFB_(passthrough_);
    xrPassthroughLayerResumeFB_(passthrough_layer_);

    passthrough_ready_ = true;
    LOG("passthrough ready");
    return true;
}

void QuestClient::destroy_passthrough() {
    passthrough_ready_ = false;
    if (passthrough_layer_ != XR_NULL_HANDLE) {
        if (xrPassthroughLayerPauseFB_) xrPassthroughLayerPauseFB_(passthrough_layer_);
        if (xrDestroyPassthroughLayerFB_) xrDestroyPassthroughLayerFB_(passthrough_layer_);
        passthrough_layer_ = XR_NULL_HANDLE;
    }
    if (passthrough_ != XR_NULL_HANDLE) {
        if (xrPassthroughPauseFB_) xrPassthroughPauseFB_(passthrough_);
        if (xrDestroyPassthroughFB_) xrDestroyPassthroughFB_(passthrough_);
        passthrough_ = XR_NULL_HANDLE;
    }
}

// -----------------------------------------------------------------------------
// Stream assets (created per connection, torn down on disconnect)
// -----------------------------------------------------------------------------

bool QuestClient::create_stream_assets() {
    const auto& h = net_.header();
    sw_w_ = static_cast<int>(h.width);
    sw_h_ = static_cast<int>(h.height);
    // (window_pose_ is set once in run() and kept across reconnects.)

    std::string err;
    if (!renderer_.init(err, app_->activity->vm)) {
        LOGE("renderer init failed: %s", err.c_str());
        return false;
    }

    // Pick a supported swapchain format; prefer non-sRGB RGBA8 so video frames
    // aren't gamma-encoded twice.
    constexpr int64_t kGLRGBA8 = 0x8058;
    constexpr int64_t kGLSRGBA8 = 0x8C43;
    uint32_t fn = 0;
    XrResult xr = xrEnumerateSwapchainFormats(session_, 0, &fn, nullptr);
    if (XR_FAILED(xr)) {
        LOGE("xrEnumerateSwapchainFormats(count) failed: %s",
             xr_result_string(instance_, xr).c_str());
        return false;
    }
    if (!fn) {
        LOGE("no swapchain formats reported");
        return false;
    }
    std::vector<int64_t> formats(fn);
    xr = xrEnumerateSwapchainFormats(session_, fn, &fn, formats.data());
    if (XR_FAILED(xr)) {
        LOGE("xrEnumerateSwapchainFormats(list) failed: %s",
             xr_result_string(instance_, xr).c_str());
        return false;
    }

    int64_t chosen = 0;
    for (uint32_t i = 0; i < fn; ++i) {
        if (formats[i] == kGLRGBA8) { chosen = kGLRGBA8; break; }
    }
    if (!chosen)
        for (uint32_t i = 0; i < fn; ++i)
            if (formats[i] == kGLSRGBA8) { chosen = kGLSRGBA8; break; }
    if (!chosen && fn) chosen = formats[0];
    if (!chosen) {
        LOGE("no swapchain formats reported");
        return false;
    }

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = chosen;
    sci.width = sw_w_;
    sci.height = sw_h_;
    sci.sampleCount = 1;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    xr = xrCreateSwapchain(session_, &sci, &swapchain_);
    if (XR_FAILED(xr)) {
        LOGE("xrCreateSwapchain failed: %s",
             xr_result_string(instance_, xr).c_str());
        return false;
    }

    uint32_t ic = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &ic, nullptr);
    images_.assign(ic, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(
        swapchain_, ic, &ic,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));

    if (!decoder_.open(sw_w_, sw_h_, renderer_.decode_window(), err)) {
        LOGE("decoder open failed: %s", err.c_str());
        return false;
    }

    LOG("stream assets ready: %dx%d, %u swapchain images", sw_w_, sw_h_, ic);
    assets_ready_ = true;
    return true;
}

void QuestClient::destroy_stream_assets() {
    // Unconditional: every component guards on its own handles, so this safely
    // cleans up a partial setup (e.g. when create_stream_assets failed midway).
    assets_ready_ = false;
    decoder_.close();
    renderer_.destroy();
    if (swapchain_) {
        xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
    }
    images_.clear();
}

// -----------------------------------------------------------------------------
// Events + interaction
// -----------------------------------------------------------------------------

void QuestClient::poll_xr_events() {
    XrEventDataBuffer ed{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ed) == XR_SUCCESS) {
        switch (ed.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ed);
                LOG("session state -> %u", static_cast<unsigned>(s->state));
                switch (s->state) {
                    case XR_SESSION_STATE_READY:
                        if (!session_running_) {
                            XrSessionBeginInfo sbi{
                                XR_TYPE_SESSION_BEGIN_INFO};
                            sbi.primaryViewConfigurationType =
                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                            if (xrBeginSession(session_, &sbi) == XR_SUCCESS)
                                session_running_ = true;
                        }
                        break;
                    case XR_SESSION_STATE_STOPPING:
                        if (session_running_) {
                            xrEndSession(session_);
                            session_running_ = false;
                        }
                        break;
                    case XR_SESSION_STATE_EXITING:
                    case XR_SESSION_STATE_LOSS_PENDING:
                        should_exit_ = true;
                        break;
                    default:
                        break;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                should_exit_ = true;
                break;
            default:
                break;
        }
        ed = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool QuestClient::trigger_held(XrPath hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = grab_action_;
    gi.subactionPath = hand;
    XrActionStateFloat v{XR_TYPE_ACTION_STATE_FLOAT};
    if (xrGetActionStateFloat(session_, &gi, &v) != XR_SUCCESS) return false;
    return v.isActive && v.currentState > kGrabThreshold;
}

bool QuestClient::locate_hand(XrPath hand, XrTime time, XrPosef& out) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    XrSpace sp = (hand == left_hand_) ? left_space_ : right_space_;
    if (xrLocateSpace(sp, space_, time, &loc) != XR_SUCCESS) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
    out = loc.pose;
    return true;
}

void QuestClient::sync_and_handle_grab(XrTime time) {
    XrActiveActionSet active{action_set_, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    xrSyncActions(session_, &si);

    const bool lp = trigger_held(left_hand_);
    const bool rp = trigger_held(right_hand_);
    XrPosef lpose, rpose;
    const bool lok = locate_hand(left_hand_, time, lpose);
    const bool rok = locate_hand(right_hand_, time, rpose);

    switch (grabber_) {
        case kNone:
            if (lp && lok) {
                grabber_ = kLeft;
                grab_offset_ = sub(window_pose_.position, lpose.position);
            } else if (rp && rok) {
                grabber_ = kRight;
                grab_offset_ = sub(window_pose_.position, rpose.position);
            }
            break;
        case kLeft:
            if (lp && lok)
                window_pose_.position = add(lpose.position, grab_offset_);
            else
                grabber_ = kNone;
            break;
        case kRight:
            if (rp && rok)
                window_pose_.position = add(rpose.position, grab_offset_);
            else
                grabber_ = kNone;
            break;
    }
}

// -----------------------------------------------------------------------------
// Per-frame render
// -----------------------------------------------------------------------------

void QuestClient::render_frame() {
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (xrWaitFrame(session_, &fwi, &fs) != XR_SUCCESS) return;
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(session_, &fbi);

    const XrTime t = fs.predictedDisplayTime;

    // Pump the decoder and consume the latest decoded frame into the GL texture.
    if (assets_ready_) {
        EncodedFrame f;
        while (net_.pop_frame(f, 0))
            decoder_.feed(f.data.data(), f.data.size(), f.pts_usec, f.keyframe);
        decoder_.drain_to_surface();
        renderer_.update_texture();
    }

    sync_and_handle_grab(t);

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = t;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    std::vector<const XrCompositionLayerBaseHeader*> layers;

    XrCompositionLayerPassthroughFB passthrough_layer{
        XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (passthrough_ready_) {
        passthrough_layer.space = space_;
        passthrough_layer.layerHandle = passthrough_layer_;
        layers.push_back(
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthrough_layer));
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (assets_ready_ && fs.shouldRender) {
        uint32_t idx = 0;
        xrAcquireSwapchainImage(swapchain_, nullptr, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 1'000'000'000;  // 1 s
        xrWaitSwapchainImage(swapchain_, &wi);

        renderer_.render_to_image(images_[idx].image, sw_w_, sw_h_);

        xrReleaseSwapchainImage(swapchain_, nullptr);

        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = space_;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.pose = window_pose_;
        quad.size = {kDefaultWindowWidth,
                     kDefaultWindowWidth * sw_h_ / float(sw_w_)};
        quad.subImage.swapchain = swapchain_;
        quad.subImage.imageRect = {{0, 0}, {sw_w_, sw_h_}};
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    }

    if (!layers.empty()) {
        fei.layerCount = static_cast<uint32_t>(layers.size());
        fei.layers = layers.data();
    }

    xrEndFrame(session_, &fei);
}

// -----------------------------------------------------------------------------
// Main run
// -----------------------------------------------------------------------------

void QuestClient::run(android_app* app) {
    app_ = app;

    std::string err;
    if (!egl_init(egl_, err)) {
        LOGE("egl init failed: %s", err.c_str());
        return;
    }
    if (!init_openxr(app) || !create_session_and_actions()) {
        LOGE("OpenXR init failed");
        return;
    }

    // Default floating-window pose (kept across reconnects).
    window_pose_.orientation = {0, 0, 0, 1};
    window_pose_.position = {0, 0, -kDefaultWindowDistance};

    // Background connector: discover/connect, and reconnect automatically on
    // loss. The render thread builds the swapchain+decoder once connected.
    connector_ = std::thread([this] {
        while (!stopping_) {
            if (!net_.connected()) {
                std::string e;
                if (net_.discover(3000)) {
                    if (net_.connect_and_run(e)) {
                        const auto& h = net_.header();
                        LOG("connected: stream %ux%u", h.width, h.height);
                    } else {
                        LOGE("connect failed: %s", e.c_str());
                    }
                }
                if (!net_.connected()) std::this_thread::sleep_for(std::chrono::seconds(2));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        net_.stop();
    });

    while (!app->destroyRequested && !should_exit_) {
        // Pump Android lifecycle/input events (non-blocking).
        int events;
        android_poll_source* src;
        while (ALooper_pollAll(0, nullptr, &events,
                               reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(app, src);
        }

        poll_xr_events();

        if (session_running_) {
            // Bring stream assets up/down with the connection.
            if (net_.connected() && !assets_ready_) {
                if (!asset_creation_failed_) {
                    if (!create_stream_assets()) {
                        destroy_stream_assets();
                        asset_creation_failed_ = true;
                        LOGE("stream asset creation failed; reconnect or relaunch to retry");
                    }
                }
            } else if (!net_.connected() && assets_ready_) {
                destroy_stream_assets();
                asset_creation_failed_ = false;
            } else if (!net_.connected()) {
                asset_creation_failed_ = false;
            }
            render_frame();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    stopping_ = true;
    destroy_stream_assets();
    if (connector_.joinable()) connector_.join();  // connector stops net_ on exit
    net_.stop();  // safe no-op if the connector already did

    destroy_passthrough();
    if (left_space_) xrDestroySpace(left_space_);
    if (right_space_) xrDestroySpace(right_space_);
    if (space_) xrDestroySpace(space_);
    if (session_) {
        if (session_running_) xrEndSession(session_);
        xrDestroySession(session_);
    }
    if (action_set_) xrDestroyActionSet(action_set_);
    if (instance_) xrDestroyInstance(instance_);
    egl_destroy(egl_);
    LOG("MetaShare client exited cleanly");
}

}  // namespace metashare

extern "C" void android_main(android_app* app) {
    metashare::QuestClient client;
    client.run(app);
}
