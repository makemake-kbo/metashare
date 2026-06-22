#include "egl.hpp"

namespace metashare {

namespace {
EGLConfig choose_config(EGLDisplay dpy) {
    const EGLint attrs[] = {EGL_RED_SIZE,
                            8,
                            EGL_GREEN_SIZE,
                            8,
                            EGL_BLUE_SIZE,
                            8,
                            EGL_ALPHA_SIZE,
                            8,
                            EGL_DEPTH_SIZE,
                            0,
                            EGL_STENCIL_SIZE,
                            0,
                            EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES3_BIT,
                            EGL_SURFACE_TYPE,
                            EGL_PBUFFER_BIT,
                            EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    eglChooseConfig(dpy, attrs, &cfg, 1, &n);
    return (n >= 1) ? cfg : nullptr;
}
}  // namespace

bool egl_init(EglContext& e, std::string& err) {
    e.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (e.display == EGL_NO_DISPLAY) {
        err = "eglGetDisplay failed";
        return false;
    }
    if (eglInitialize(e.display, &e.major, &e.minor) == EGL_FALSE) {
        err = "eglInitialize failed";
        return false;
    }
    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        err = "eglBindAPI(EGL_OPENGL_ES_API) failed";
        return false;
    }
    e.config = choose_config(e.display);
    if (!e.config) {
        err = "eglChooseConfig found no matching config";
        return false;
    }
    // GLES 3.0 minimum (OpenXR GLES requires it); ask for 3.0.
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    e.context =
        eglCreateContext(e.display, e.config, EGL_NO_CONTEXT, ctx_attrs);
    if (e.context == EGL_NO_CONTEXT) {
        err = "eglCreateContext failed";
        return false;
    }
    const EGLint surf_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    e.surface = eglCreatePbufferSurface(e.display, e.config, surf_attrs);
    if (e.surface == EGL_NO_SURFACE) {
        err = "eglCreatePbufferSurface failed";
        return false;
    }
    if (!egl_make_current(e)) {
        err = "eglMakeCurrent failed";
        return false;
    }
    return true;
}

void egl_destroy(EglContext& e) {
    if (e.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(e.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (e.context != EGL_NO_CONTEXT)
            eglDestroyContext(e.display, e.context);
        if (e.surface != EGL_NO_SURFACE)
            eglDestroySurface(e.display, e.surface);
        eglTerminate(e.display);
    }
    e = {};
}

}  // namespace metashare
