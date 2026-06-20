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
          '';
        };
      });
}
