# MetaShare

<img width="1600" height="900" alt="" src="https://github.com/user-attachments/assets/e0545698-e204-4ccc-a434-8956f32d5fd4" />

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

The flake exposes a `metashare` package (streamer + SDL2 test client + GTK4
control panel and its GNOME `.desktop` entry). It tracks **nixpkgs
nixos-unstable**, so point its `nixpkgs` input at an unstable channel.

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    metashare.url = "github:makemake-kbo/metashare";
    metashare.inputs.nixpkgs.follows = "nixpkgs";
  };
}

# Pass `inputs` to your modules (specialArgs / extraSpecialArgs), then:
environment.systemPackages = [ inputs.metashare.packages.${pkgs.system}.default ];  # NixOS
home.packages             = [ inputs.metashare.packages.${pkgs.system}.default ];   # Home Manager

# Wayland capture needs a portal running (GNOME ships one by default):
xdg.portal.enable = true;
xdg.portal.extraPortals = [ pkgs.xdg-desktop-portal-gnome ];
```

```nix
# Run without installing:  nix run github:makemake-kbo/metashare[#streamer]
```

### Run the streamer

```sh
# Monitors will add additional virtual desktops. It does not go over 3 for now.
./build/src/streamer/metashare-streamer --source portal --monitors 3
```

## How discovery works

***ABSOLUTELY DO NOT EXPOSE ONLINE AND MAKE SURE THESE PORTS ARE ONLY EXPOSED LOCALLY!!!***

Zero-config by design. The streamer listens on **UDP 7777**. The client
broadcasts a small probe to `255.255.255.255:7777`; the streamer replies with
its hostname and **TCP** stream port (default **7778**). The client then opens
the TCP connection and starts decoding at the next keyframe. Ports are fixed
defaults.
