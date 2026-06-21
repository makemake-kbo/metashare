# MetaShare

Mirror a Linux **Wayland** session to a **Meta Quest** and view it as a
floating Horizon OS app window — an open re-creation of Meta Remote Desktop for
Linux.

The host runs a modern C++ CLI **streamer** that captures the Wayland session,
encodes it to H.264, and serves it over the LAN. The Quest **client** is a
standard resizable Android app, so Horizon OS presents it as a floating system
window. A **desktop test client** mirrors the stream path so the pipeline can be
developed and verified without headset hardware.

```
 ┌──────────────────────── Linux host ────────────────────────┐        ┌──────── Quest ──────────┐
 │ xdg-desktop-portal ─▶ PipeWire ─▶ Encoder(H.264) ─▶ TCP ────┼──LAN──▶│ TCP ─▶ MediaCodec ─▶     │
 │            ▲ user picks a monitor          UDP discovery ◀──┼────────┤ Android SurfaceView     │
 └────────────────────────────────────────────────────────────┘        └─────────────────────────┘
```

## Status

| Component                              | State                              |
| -------------------------------------- | ---------------------------------- |
| Nix dev environment (`flake.nix`)      | ✅ working                         |
| H.264 encoder (libavcodec / x264)      | ✅ working                         |
| TCP frame server + UDP discovery       | ✅ working                         |
| Test-pattern capture source            | ✅ working (verified end-to-end)   |
| Wayland capture (portal + PipeWire)    | ✅ builds; needs a Wayland session |
| Desktop test client (SDL2)             | ✅ working                         |
| GTK4 streamer control panel            | ✅ working — see `client/gtk`       |
| Quest Android 2D client                | ✅ working — see `client/quest`     |

Hardware (VAAPI/NVENC) encoding is the next planned step; today the encoder is
portable software x264 tuned for zero-latency.

## Quick start

Everything builds inside the Nix dev shell — no system packages required.

```sh
nix develop                       # enter the dev shell (downloads deps once)
meson setup build                 # core pipeline (test-pattern source)
ninja -C build
```

To build the real Wayland capture backend (needs the host to be a Wayland
session with `xdg-desktop-portal` running):

```sh
meson setup build -Dportal=enabled
ninja -C build
```

### Run the streamer

```sh
# Synthetic test pattern — works anywhere, great for a first smoke test:
./build/src/streamer/metashare-streamer --source test --width 1280 --height 720 --fps 60

# Real Wayland capture (portal dialog asks you to pick a monitor):
./build/src/streamer/metashare-streamer --source portal
```

Key flags: `--source test|portal`, `--width/--height/--fps`, `--bitrate <kbps>`,
`--port <n>` (default 7778), `--no-discovery`. See `--help`.

### Run the desktop test client

```sh
# Auto-discover the streamer on the LAN and open a window:
./build/client/desktop_test/metashare-testclient

# Or connect directly:
./build/client/desktop_test/metashare-testclient --host 192.168.1.20
```

Press `Esc` to quit.

### Run the GTK4 control panel

A modern GNOME/GTK4 launcher (`client/gtk`) for the streamer. It lets you pick
resolution (with a locked aspect ratio), fps, bitrate, codec, hardware-accel and
port; "Start Stream" spawns the streamer as a subprocess and surfaces its log.

```sh
./build/client/gtk/metashare-streamer-ui
```

## How discovery works

Zero-config by design. The streamer listens on **UDP 7777**. The client
broadcasts a small probe to `255.255.255.255:7777`; the streamer replies with
its hostname and **TCP** stream port (default **7778**). The client then opens
the TCP connection and starts decoding at the next keyframe. Ports are fixed
defaults for now; both are overridable.

## Wire protocol

Defined once in [`src/common/protocol.hpp`](src/common/protocol.hpp) and shared
by the streamer and both clients:

- **Discovery (UDP):** `DiscoveryProbe` → `DiscoveryOffer`.
- **Stream (TCP):** one `StreamHeader` (codec, resolution, fps), then a
  sequence of `FrameHeader` + encoded H.264 access unit. SPS/PPS are kept
  in-band before each keyframe so a client can join mid-stream.

## Layout

```
flake.nix                     Nix dev shell + package
meson.build, meson_options.txt Build (use -Dportal=enabled for Wayland capture)
src/common/protocol.hpp       Shared wire protocol
src/streamer/                 The CLI streamer
  frame_source.hpp            Capture-backend interface
  test_pattern_source.*       Synthetic source (no desktop needed)
  portal_pipewire_source.*    Real Wayland capture (xdg-desktop-portal + PipeWire)
  encoder.*                   Low-latency H.264 (libavcodec)
  net_server.*                TCP fan-out to clients
  discovery.*                 UDP discovery responder
  main.cpp                    Pipeline + CLI
client/desktop_test/          SDL2 reference client (validates the stream)
client/gtk/                   GTK4 streamer control panel (launcher only)
client/quest/                 Quest Android 2D client — see its README
```

## License

TODO — pick a license before publishing.
