# MetaShare — Quest client

A standard Android 2D app for Quest/Horizon OS. It discovers the MetaShare
streamer, receives raw H.265/H.264 + Opus RTP over UDP, decodes video with the
Quest hardware decoder (`MediaCodec`), and renders into a `SurfaceView`.

The primary panel owns a compact control strip that stays greyed and partly
hidden at the top edge until the pointer approaches it. It contains only the
virtual keyboard, pointer toggle, and monitor minus/count/plus controls.
Secondary panels intentionally stay chrome-free.

This intentionally does **not** use OpenXR or libwebrtc. Quest launches it as a
normal resizable app window, which matches the system-window approach used by
remote-desktop-style apps.

## Prerequisites

- A Quest in developer mode with USB debugging enabled.
- Android SDK installed by `setup-android-sdk.sh` into `client/quest/.android-sdk`.

## Build & run

```sh
nix develop .#android
cd client/quest
./setup-android-sdk.sh
gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.metashare.client/.MainActivity
```

On launch the app broadcasts a discovery probe. Start the streamer on a host on
the same LAN and the Quest window will connect automatically. If no host is
found, or the stream drops, it keeps retrying.

For a known-good test stream:

```sh
./build/src/streamer/metashare-streamer --source test --width 1280 --height 720 --fps 60 --bitrate 8000
```

For desktop capture:

```sh
./build/src/streamer/metashare-streamer --source portal
```

## Architecture

```text
MainActivity / MonitorActivity
    SurfaceView (plain android.view.SurfaceView — no EGL)
        VideoDecoder: MediaCodec("video/hevc"|"video/avc") -> Surface (async)

StreamSession (background thread)
    UDP broadcast discovery on 7777  (protocol v4)
    TCP signaling on 7778            (HELLO stream params -> READY {port} -> START)
    RtpReceiver on an ephemeral UDP port
        demux by SSRC
        video: jitter buffer + H.265/H.264 depacketizer -> VideoSink (Annex B)
        audio: Opus payload -> AudioSink
        reliability: NACK (sequence gaps) + PLI (request keyframe)
    AudioDecoder: MediaCodec("audio/opus") -> AudioTrack

RemoteInputController
    pointer: controller/touch/mouse -> normalized screen coordinates
    keyboard: Android IME -> transport-neutral text/key callbacks
```

Pointer and keyboard events currently stop at transport-neutral callbacks; the
views and Quest system keyboard are ready, but a host input protocol and input
injection backend are still required before events can control the host.

No third-party media dependency is required — everything uses the Android
framework (`MediaCodec`, `AudioTrack`, `DatagramSocket`, `Surface`).
