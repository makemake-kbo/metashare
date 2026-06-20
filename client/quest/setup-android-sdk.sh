#!/usr/bin/env bash
# Installs the Android SDK + NDK into client/quest/.android-sdk for the Quest
# client build. Idempotent: skips work that's already done.
#
# Run inside the Nix Android shell so sdkmanager has a JDK:
#     nix develop .#android
#     cd client/quest && ./setup-android-sdk.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$HERE/.android-sdk"
CMDLINE_VER="11076708"   # cmdline-tools 11.0
NDK_VER="26.1.10909125"  # NDK r26b — matches app/build.gradle

mkdir -p "$SDK_ROOT"

# 1. commandline-tools --------------------------------------------------------
if [ ! -x "$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" ]; then
  echo ">> downloading cmdline-tools $CMDLINE_VER"
  tmp="$(mktemp -d)"
  curl -fsSL -o "$tmp/cml.zip" \
    "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_VER}_latest.zip"
  rm -rf "$SDK_ROOT/cmdline-tools"
  mkdir -p "$SDK_ROOT/cmdline-tools"
  unzip -q "$tmp/cml.zip" -d "$tmp"
  mv "$tmp/cmdline-tools" "$SDK_ROOT/cmdline-tools/latest"
  rm -rf "$tmp"
fi

export ANDROID_HOME="$SDK_ROOT"
export ANDROID_SDK_ROOT="$SDK_ROOT"
export PATH="$SDK_ROOT/cmdline-tools/latest/bin:$PATH"

# 2. licenses -----------------------------------------------------------------
echo ">> accepting SDK licenses"
yes | sdkmanager --licenses >/dev/null 2>&1 || true

# 3. packages -----------------------------------------------------------------
PKGS=(
  "platform-tools"
  "platforms;android-34"
  "build-tools;34.0.0"
  "ndk;${NDK_VER}"
  "cmake;3.22.1"
)
echo ">> installing: ${PKGS[*]}"
sdkmanager "${PKGS[@]}"

# 4. local.properties for Gradle/AGP -----------------------------------------
cat > "$HERE/local.properties" <<EOF
sdk.dir=$SDK_ROOT
EOF

echo ">> done. SDK at $SDK_ROOT"
