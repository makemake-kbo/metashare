#!/usr/bin/env bash
# Fetches the Khronos OpenXR SDK (headers + the prebuilt Android arm64 loader)
# from GitHub into third_party/openxr. Quest-compatible; no Meta account needed.
#
#     ./client/quest/fetch-openxr.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="$ROOT/third_party/openxr"
VER="1.1.60"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo ">> downloading Khronos openxr_loader_for_android-$VER.aar"
curl -fsSL -o "$tmp/loader.aar" \
  "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$VER/openxr_loader_for_android-$VER.aar"

rm -rf "$DEST"
mkdir -p "$DEST/include" "$DEST/lib/arm64-v8a"

# Headers live under the AAR's prefab 'headers' module.
unzip -q "$tmp/loader.aar" 'prefab/modules/headers/include/openxr/*' -d "$tmp"
mv "$tmp/prefab/modules/headers/include/openxr" "$DEST/include/openxr"

# arm64 loader .so (built by Khronos with NDK r23c; ABI-stable).
unzip -q "$tmp/loader.aar" \
  'prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so' -d "$tmp"
mv "$tmp/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so" \
  "$DEST/lib/arm64-v8a/"

echo ">> OpenXR $VER at $DEST"
find "$DEST" -type f | sort
