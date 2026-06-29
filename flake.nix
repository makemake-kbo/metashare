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

        # Flake-driven Flatpak build. The flatpak-builder manifests under
        # ./flatpak are the source of truth (portable: they build metashare from
        # source against a flatpak runtime). This wrapper just gives them a
        # `nix run` entrypoint so the flake "drives" the flatpak output:
        #   nix run .#flatpak-cli   -> dev.metashare.Streamer.flatpak
        #   nix run .#flatpak-ui    -> dev.metashare.StreamerUI.flatpak
        mkFlatpakApp = { name, appId, manifest }:
          pkgs.writeShellApplication {
            inherit name;
            runtimeInputs = with pkgs; [ flatpak flatpak-builder git coreutils ];
            text = ''
              set -euo pipefail
              root="$(git rev-parse --show-toplevel)"
              cd "$root"
              state="$(mktemp -d)"
              trap 'rm -rf "$state"' EXIT

              echo ">> Ensuring the flathub remote (per-user)"
              flatpak --user remote-add --if-not-exists flathub \
                https://dl.flathub.org/repo/flathub.flatpakrepo

              echo ">> Building ${appId} with flatpak-builder"
              flatpak-builder --user --force-clean --disable-rofiles-fuse \
                --install-deps-from=flathub \
                --repo="$state/repo" "$state/build" "flatpak/${manifest}"

              out="${appId}.flatpak"
              echo ">> Exporting single-file bundle: $out"
              flatpak build-bundle "$state/repo" "$out" "${appId}"
              echo ">> Wrote $root/$out"
            '';
          };

        flatpakCli = mkFlatpakApp {
          name = "metashare-flatpak-cli";
          appId = "dev.metashare.Streamer";
          manifest = "dev.metashare.Streamer.yml";
        };
        flatpakUi = mkFlatpakApp {
          name = "metashare-flatpak-ui";
          appId = "dev.metashare.StreamerUI";
          manifest = "dev.metashare.StreamerUI.yml";
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
        apps.default = self.apps.${system}.ui;

        # `nix run .#flatpak-cli` / `.#flatpak-ui` — build distributable Flatpaks
        apps.flatpak-cli = {
          type = "app";
          program = "${flatpakCli}/bin/metashare-flatpak-cli";
        };
        apps.flatpak-ui = {
          type = "app";
          program = "${flatpakUi}/bin/metashare-flatpak-ui";
        };

        devShells.default = pkgs.mkShell {
          packages = nativeDeps ++ runtimeDeps ++ (with pkgs; [
            gdb
            clang-tools   # clang-format / clangd
            wayland-utils
            flatpak           # `nix run .#flatpak-{cli,ui}` and manual testing
            flatpak-builder
          ]);

          shellHook = ''
            echo "MetaShare dev shell — run: meson setup build && ninja -C build"
            echo "Streamer: ./build/src/streamer/metashare-streamer"
            echo "GTK4 control panel: ./build/client/gtk/metashare-streamer-ui"
            echo "Lint: scripts/lint.sh  (--fix to reformat)"
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
            curl          # setup-android-sdk.sh downloads cmdline-tools
            cacert        # TLS roots for curl/sdkmanager in headless/CI shells
          ];

          # Point curl/Java at the Nix cert bundle so the SDK downloads work in
          # CI and other environments without system certificates.
          SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

          shellHook = ''
            echo "MetaShare Android shell — SDK at client/quest/.android-sdk"
            echo "  cd client/quest && gradle assembleDebug"
            echo "  (first run: ./setup-android-sdk.sh to install the SDK)"
          '';
        };
      });
}
