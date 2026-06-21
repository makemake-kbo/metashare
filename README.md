# MetaShare

Mirror a Linux **Wayland** session to a **Meta Quest** and view it as a
floating Horizon OS app window. An open re-creation of Meta Remote Desktop for
Linux.

## Quick start

Builds are happening using nix. Please get nix if u dont to ensure all deps are in place:
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

To also build the Mutter `RecordVirtual` source:

```sh
meson setup build -Dportal=enabled -Dmutter=enabled
ninja -C build
```

## Installing as an app (Nix)

The flake exposes a `metashare` package with the streamer, the desktop test
client, and the GTK4 control panel plus its GNOME `.desktop` entry.

```sh
# Run ad-hoc without installing:
nix run .#ui            # GNOME control panel
nix run .#streamer      # CLI streamer
nix run .#testclient    # SDL2 test client

# Or build a cached result symlink:
nix build .#metashare   # → ./result/bin/{metashare-streamer,metashare-streamer-ui,…}

# Or install into your user profile (binaries land in ~/.nix-profile/bin,
# the .desktop entry in ~/.nix-profile/share/applications, so GNOME picks
# them up automatically):
nix profile install .#metashare
```

### NixOS / Home Manager

To install system-wide on NixOS:

```nix
environment.systemPackages = [
  metashare-flake.packages.x86_64-linux.default
];
```

Or under Home Manager:

```nix
home.packages = [
  metashare-flake.packages.x86_64-linux.default
];
```

The streamer needs a running `xdg-desktop-portal` (and a portal impl such as
`xdg-desktop-portal-gnome` or `xdg-desktop-portal-wlr`) to capture a Wayland
monitor. GNOME provides this out of the box.

### Run the streamer

```sh
./build/src/streamer/metashare-streamer --source portal
```

## How discovery works

Zero-config by design. The streamer listens on **UDP 7777**. The client
broadcasts a small probe to `255.255.255.255:7777`; the streamer replies with
its hostname and **TCP** stream port (default **7778**). The client then opens
the TCP connection and starts decoding at the next keyframe. Ports are fixed
defaults.
