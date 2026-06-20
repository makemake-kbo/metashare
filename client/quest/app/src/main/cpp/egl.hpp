// EGL context for the Quest client.
//
// A single tiny-pbuffer EGL setup is shared by:
//   * the OpenXR OpenGL ES graphics binding (display/config/context handles),
//   * the GLES blit that samples the decoded SurfaceTexture into the quad-layer
//     swapchain image.
//
// OpenXR hands us swapchain images to render into; the pbuffer only makes the
// GLES context current on runtimes that do not support surfaceless contexts.

#pragma once

#include <EGL/egl.h>

#include <string>

namespace metashare {

struct EglContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int major = 0, minor = 0;  // EGL version after init
};

// Creates and makes current a GLES 3.x context on the calling thread.
bool egl_init(EglContext& e, std::string& err);
void egl_destroy(EglContext& e);

inline bool egl_make_current(const EglContext& e) {
    return eglMakeCurrent(e.display, e.surface, e.surface, e.context) == EGL_TRUE;
}

}  // namespace metashare
