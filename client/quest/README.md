# MetaShare — Quest client

A standard Android 2D app for Quest/Horizon OS. It discovers the MetaShare
streamer, decodes the H.264 stream with the Quest hardware decoder
(`MediaCodec`), and renders into a `SurfaceView`.

This intentionally does **not** use OpenXR. Quest launches it as a normal
resizable app window, which matches the system-window approach used by remote
desktop style apps.

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
MainActivity
    SurfaceView
        MediaCodec("video/avc") renders decoded frames directly to the Surface

Background stream thread
    UDP broadcast discovery on 7777
    TCP stream connect on 7778
    StreamHeader + FrameHeader parsing
    encoded H.264 access units queued into MediaCodec input buffers
```

The old OpenXR native client sources are still in `app/src/main/cpp` for
reference, but Gradle no longer builds or packages them for the Quest APK.
