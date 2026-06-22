// Decode-surface + GLES blit for the Quest client.
//
// Owns the consumer side of the GPU decode path:
//   * an `ASurfaceTexture` (native NDK, API 28+) whose ANativeWindow is handed
//     to MediaCodec as the decode target — so decoded frames never touch the
//     CPU;
//   * a GL_TEXTURE_EXTERNAL_OES texture that the SurfaceTexture updates;
//   * a tiny ESSL3 program that samples that external texture (applying the
//     SurfaceTexture transform matrix) into an OpenXR quad-layer swapchain
//     image.
//
// All methods must be called on the render thread that has the EGL context
// current (ASurfaceTexture_updateTexImage and GL calls require it).

#pragma once

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES (must follow gl3.h)
#include <android/surface_texture.h>
#include <android/surface_texture_jni.h>
#include <jni.h>

#include <cstdint>
#include <string>

namespace metashare {

class Renderer {
  public:
    Renderer() = default;
    ~Renderer();

    // Creates the OES texture, the Android SurfaceTexture (constructed via JNI
    // against this thread's current GL context, then wrapped as an
    // ASurfaceTexture), the blit program and an FBO. On success,
    // decode_window() is the surface to pass to the Decoder. `vm` is the app's
    // JavaVM.
    bool init(std::string& err, JavaVM* vm);
    void destroy();

    // Surface MediaCodec decodes into.
    ANativeWindow* decode_window() const { return decode_window_; }

    // Consume the most recent decoded buffer into the OES texture. Safe to call
    // every frame (no-op when no new buffer is available). Returns true once at
    // least one frame has been produced.
    bool update_texture();

    // Blit the latest decoded frame into `target_tex` (a GL_TEXTURE_2D of size
    // w x h, typically an OpenXR swapchain image). Clears to black until the
    // first frame arrives so the quad never shows uninitialized memory.
    void render_to_image(GLuint target_tex, int w, int h);

  private:
    ASurfaceTexture* st_ = nullptr;
    ANativeWindow* decode_window_ = nullptr;
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    jobject jst_ = nullptr;  // global ref to the Java SurfaceTexture
    bool jni_attached_ = false;
    GLuint oes_tex_ = 0;
    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0, fbo_ = 0;
    GLint loc_st_ = -1, loc_tex_ = -1;
    int64_t last_ts_ = -1;
    bool has_frame_ = false;
};

}  // namespace metashare
