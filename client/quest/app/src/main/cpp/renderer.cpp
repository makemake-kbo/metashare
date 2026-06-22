#include "renderer.hpp"

#include <android/log.h>
#include <android/native_window.h>
#include <cstring>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MetaShare", __VA_ARGS__)
#define LOGE(...)                                                              \
    __android_log_print(ANDROID_LOG_ERROR, "MetaShare", __VA_ARGS__)

namespace metashare {

namespace {

// Full-screen triangle strip (clip space) with base texture coords. The
// SurfaceTexture transform matrix (uSTMatrix) maps these base coords into the
// correct sample coords, handling any Y-flip/crop the decoder requires.
//   x      y     u    v
constexpr float kVerts[] = {
    -1.f, -1.f, 0.f, 0.f, 1.f, -1.f, 1.f, 0.f,
    -1.f, 1.f,  0.f, 1.f, 1.f, 1.f,  1.f, 1.f,
};

const char* kVS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTex;
uniform mat4 uSTMatrix;
out vec2 vTex;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTex = (uSTMatrix * vec4(aTex, 0.0, 1.0)).xy;
})";

const char* kFS = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vTex;
uniform samplerExternalOES uTex;
out vec4 frag;
void main() { frag = texture(uTex, vTex); })";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link_program() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

}  // namespace

Renderer::~Renderer() { destroy(); }

bool Renderer::init(std::string& err, JavaVM* vm) {
    vm_ = vm;

    glGenTextures(1, &oes_tex_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, oes_tex_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    // NDK r26 has no ASurfaceTexture_create(): a SurfaceTexture must be created
    // as a Java object (against the current GL context) and then wrapped. The
    // constructor binds it straight to our OES texture name, so no
    // attach/detach round-trip is needed.
    JNIEnv* env = nullptr;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "MetaShare", nullptr};
        if (vm_->AttachCurrentThread(&env, &args) != JNI_OK) {
            err = "JNI AttachCurrentThread failed";
            return false;
        }
        jni_attached_ = true;
    }
    env_ = env;

    jclass cls = env->FindClass("android/graphics/SurfaceTexture");
    if (!cls) {
        err = "FindClass android/graphics/SurfaceTexture failed";
        return false;
    }
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(I)V");
    if (!ctor) {
        err = "SurfaceTexture.<init>(I) not found";
        return false;
    }
    jobject local = env->NewObject(cls, ctor, static_cast<jint>(oes_tex_));
    env->DeleteLocalRef(cls);
    if (!local) {
        err = "Failed to construct SurfaceTexture";
        return false;
    }
    jst_ = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);

    st_ = ASurfaceTexture_fromSurfaceTexture(env_, jst_);
    if (!st_) {
        err = "ASurfaceTexture_fromSurfaceTexture failed";
        return false;
    }
    decode_window_ = ASurfaceTexture_acquireANativeWindow(st_);
    if (!decode_window_) {
        err = "ASurfaceTexture_acquireANativeWindow failed";
        return false;
    }

    program_ = link_program();
    if (!program_) {
        err = "failed to build blit program";
        return false;
    }
    loc_st_ = glGetUniformLocation(program_, "uSTMatrix");
    loc_tex_ = glGetUniformLocation(program_, "uTex");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                          reinterpret_cast<void*>(sizeof(float) * 2));
    glBindVertexArray(0);

    glGenFramebuffers(1, &fbo_);
    LOG("renderer ready (OES tex=%u)", oes_tex_);
    return true;
}

bool Renderer::update_texture() {
    if (!st_) return false;
    // No-op (and harmless) when no new buffer is available; otherwise consumes
    // the latest decoded buffer into the bound OES texture.
    ASurfaceTexture_updateTexImage(st_);
    int64_t ts = ASurfaceTexture_getTimestamp(st_);
    if (ts != last_ts_ && ts > 0) {
        last_ts_ = ts;
        if (!has_frame_) LOG("renderer latched first decoded frame");
        has_frame_ = true;
    }
    return has_frame_;
}

void Renderer::render_to_image(GLuint target_tex, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target_tex, 0);

    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    if (has_frame_) {
        float m[16] = {0};
        ASurfaceTexture_getTransformMatrix(st_, m);
        glUseProgram(program_);
        glUniformMatrix4fv(loc_st_, 1, GL_FALSE, m);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, oes_tex_);
        glUniform1i(loc_tex_, 0);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    } else {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::destroy() {
    if (decode_window_) {
        ANativeWindow_release(decode_window_);
        decode_window_ = nullptr;
    }
    if (st_) {
        ASurfaceTexture_release(st_);
        st_ = nullptr;
    }
    if (jst_ && env_) {
        env_->DeleteGlobalRef(jst_);
        jst_ = nullptr;
    }
    if (jni_attached_ && vm_) {
        vm_->DetachCurrentThread();
        jni_attached_ = false;
        env_ = nullptr;
    }
    if (oes_tex_) {
        glDeleteTextures(1, &oes_tex_);
        oes_tex_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    last_ts_ = -1;
    has_frame_ = false;
}

}  // namespace metashare
