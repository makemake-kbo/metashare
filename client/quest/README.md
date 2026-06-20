# MetaShare — Quest 3 client

A native Android (OpenXR) app that discovers the MetaShare streamer, decodes the
H.264 stream with the hardware decoder (`MediaCodec`), and shows it on a
**floating quad layer** in front of you.

Unlike the streamer and desktop client, this app builds with the **Android
toolchain**, not Nix — it needs the Android SDK/NDK and the Meta OpenXR Mobile
loader, which only exist for Android/aarch64. So it lives here as a structured
scaffold with the platform-agnostic parts (networking, protocol) fully written
and the device-specific parts (OpenXR session, GL swapchain, MediaCodec→texture)
laid out with clear integration points.

## Prerequisites

- Android Studio + Android SDK (API 32+) and NDK (r26+).
- [Meta OpenXR Mobile SDK](https://developers.meta.com/horizon/downloads/) —
  provides `libopenxr_loader.so` and the Meta XR headers. Drop the loader into
  `app/src/main/jniLibs/arm64-v8a/` and point `CMakeLists.txt` at its headers.
- A Quest 3 in developer mode with USB debugging enabled.

## Build & run

```sh
cd client/quest
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.metashare.client/android.app.NativeActivity
```

On launch the app broadcasts a discovery probe; start the streamer on a host on
the same LAN and the window appears. (No host found? It retries; you can also
hardcode an IP in `net_stream.cpp` while bringing things up.)

## Architecture

```
NativeActivity (android_native_app_glue)
        │
        ├── NetStream            discover() → connect() → reads FrameHeader+NALs
        │                        into a lock-free-ish frame queue (net_stream.*)
        │
        ├── Decoder              AMediaCodec("video/avc") configured against an
        │                        ANativeWindow from a SurfaceTexture; decoded
        │                        frames land in an external GL texture (decoder.*)
        │
        └── XrApp                OpenXR session on an EGL context. Each frame:
                                 update the SurfaceTexture, then submit an
                                 XrCompositionLayerQuad sampling the decoded
                                 texture — the "floating window" (app.cpp)
```

### Why a quad layer

`XrCompositionLayerQuad` is the right primitive for a flat panel floating in
space: the compositor samples our swapchain image at full display resolution
(no extra eye-buffer resample), giving crisp text. We place it ~1.5 m in front
of the user; pose can later be made grab/move-able.

### Decode → texture path

`MediaCodec` decodes straight onto a `Surface` backed by a `SurfaceTexture`,
which exposes a `GL_TEXTURE_EXTERNAL_OES` texture. Each XR frame we call
`updateTexImage()` and blit that external texture into the quad-layer swapchain
image with a tiny GLES program. This keeps decoded frames on the GPU — no CPU
copy of pixel data.

## What's implemented vs. stubbed

- ✅ `net_stream.*` — discovery, TCP connect, protocol parsing, frame queue.
  Portable C++; reuses `src/common/protocol.hpp`.
- 🚧 `decoder.*` — `AMediaCodec` wrapper; SPS/PPS + frame feed is written, the
  `SurfaceTexture` plumbing is marked `TODO` (needs a JNI call or NDK
  `ASurfaceTexture` on API 28+).
- 🚧 `app.cpp` — OpenXR instance/session/quad-layer lifecycle outlined with the
  per-frame flow; the EGL/GLES swapchain blit is marked `TODO`.

These TODOs are the device-specific glue you finish in Android Studio against
the real Meta OpenXR headers.
