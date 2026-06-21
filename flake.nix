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

        # Native libraries the streamer links against.
        runtimeDeps = with pkgs; [
          pipewire           # capture the screencast PipeWire stream
          ffmpeg-full        # libavcodec/util/format/swscale for encode
          sdbus-cpp          # talk to xdg-desktop-portal over D-Bus
          systemd            # provides libsystemd.pc that sdbus-c++ requires
          libdrm             # DMA-BUF frame import
          SDL2               # desktop test client window/render
          gtkmm4             # GTK4 control panel (client/gtk)
        ];

        nativeDeps = with pkgs; [
          meson
          ninja
          pkg-config
          cmake
        ];

        metashare = pkgs.stdenv.mkDerivation {
          pname = "metashare";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = nativeDeps;
          buildInputs = runtimeDeps;
          meson = pkgs.meson;
        };
      in
      {
        packages.default = metashare;

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
