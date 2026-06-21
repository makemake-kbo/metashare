{
  description = "MetaShare — stream a Wayland session to a Meta Quest 3 floating window";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Runtime libraries the streamer/UI link against.
        runtimeDeps = with pkgs; [
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

        nativeDeps = with pkgs; [
          meson
          ninja
          pkg-config
          cmake
          # Wraps the GTK4 binary with GSettings schemas, GI typelibs, GDK
          # pixbuf loaders, etc. so it renders correctly under GNOME.
          wrapGAppsHook4
          glib                # for gappsWrapperArgs / makeSchemaParser
        ];

        metashare = pkgs.stdenv.mkDerivation {
          pname = "metashare";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = nativeDeps;
          buildInputs = runtimeDeps;

          # Real Wayland capture is the whole point of the app — require it.
          mesonFlags = [
            "-Dportal=enabled"
          ];

          # The GTK frontend spawns `metashare-streamer` via $PATH lookup.
          # When launched from a GNOME .desktop entry, $out/bin is *not* on
          # PATH, so inject it into the wrapper for every wrapped binary.
          preFixup = ''
            gappsWrapperArgs+=(--prefix PATH : "$out/bin")
          '';

          # Take the bare desktop file through the standard install path.
          # Meson already installs it to share/applications/.
        };

      in
      {
        packages.default = metashare;
        packages.metashare = metashare;

        # `nix run .#ui`     — launch the GNOME control panel
        # `nix run .#stream` — launch the CLI streamer
        apps.ui = {
          type = "app";
          program = "${metashare}/bin/metashare-streamer-ui";
        };
        apps.streamer = {
          type = "app";
          program = "${metashare}/bin/metashare-streamer";
        };
        apps.testclient = {
          type = "app";
          program = "${metashare}/bin/metashare-testclient";
        };
        apps.default = self.apps.${system}.ui;

        devShells.default = pkgs.mkShell {
          packages = nativeDeps ++ runtimeDeps ++ (with pkgs; [
            gdb
            clang-tools   # clang-format / clangd
            wayland-utils
          ]);

          shellHook = ''
            echo "MetaShare dev shell — run: meson setup build && ninja -C build"
            echo "Streamer: ./build/src/streamer/metashare-streamer"
            echo "Test client: ./build/client/desktop_test/metashare-testclient"
            echo "GTK4 control panel: ./build/client/gtk/metashare-streamer-ui"
          '';
        };

        # Android (Quest client) toolchain. The Android SDK + NDK themselves are
        # installed into client/quest/.android-sdk via sdkmanager (see
        # client/quest/README.md); this shell provides JDK + Gradle to drive it.
        devShells.android = pkgs.mkShell {
          packages = with pkgs; [
            jdk17
            gradle
            unzip
            which
            file
          ];

          shellHook = ''
            echo "MetaShare Android shell — SDK at client/quest/.android-sdk"
            echo "  cd client/quest && gradle assembleDebug"
            echo "  (first run: ./setup-android-sdk.sh to install the SDK)"
          '';
        };
      });
}
