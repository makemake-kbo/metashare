{ lib
, stdenv
  # native
, meson
, ninja
, pkg-config
, cmake
, wrapGAppsHook4
, glib
  # runtime
, pipewire
, ffmpeg-full
, sdbus-cpp
, systemd
, libdrm
, SDL2
, gtkmm4
, gsettings-desktop-schemas
, adwaita-icon-theme
, hicolor-icon-theme
}:

stdenv.mkDerivation {
  pname = "metashare";
  version = "0.1.0";

  # Keep the source copy out of the store's way: drop .git and the various
  # local ./build* trees that would otherwise be pulled in wholesale.
  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let base = baseNameOf path;
      in !(type == "directory" && (base == "result"
        || lib.hasPrefix "build" base
        || base == ".flatpak-builder"));
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    cmake
    # Wraps the GTK4 binary with GSettings schemas, GI typelibs, GDK pixbuf
    # loaders, etc. so it renders correctly under GNOME.
    wrapGAppsHook4
    glib # for gappsWrapperArgs / makeSchemaParser
  ];

  buildInputs = [
    pipewire           # capture the screencast PipeWire stream
    ffmpeg-full        # libavcodec/util/format/swscale for encode
    sdbus-cpp          # talk to xdg-desktop-portal over D-Bus
    systemd            # provides libsystemd.pc that sdbus-c++ requires
    libdrm             # DMA-BUF frame import
    SDL2               # desktop test client window/render
    gtkmm4             # GTK4 control panel (client/gtk)
    # Schemas/icon theme the wrapped GTK4 frontend expects at runtime.
    gsettings-desktop-schemas
    adwaita-icon-theme
    hicolor-icon-theme
  ];

  # Real Wayland capture is the whole point of the app — require it.
  mesonFlags = [ "-Dportal=enabled" ];

  # The GTK frontend spawns `metashare-streamer` via $PATH lookup. When
  # launched from a GNOME .desktop entry, $out/bin is *not* on PATH, so inject
  # it into the wrapper for every wrapped binary.
  preFixup = ''
    gappsWrapperArgs+=(--prefix PATH : "$out/bin")
  '';

  meta = {
    description = "Stream a Wayland session to a Meta Quest 3 floating window";
    homepage = "https://github.com/makemake/metashare";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "metashare-streamer-ui";
  };
}
