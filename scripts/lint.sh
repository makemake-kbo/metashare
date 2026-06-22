#!/usr/bin/env bash
# C++ lint for MetaShare: clang-format (style) + clang-tidy (static analysis).
#
# Run inside the dev shell so clang-tools and the build dependencies are on
# PATH:
#     nix develop --command scripts/lint.sh          # check (CI mode)
#     nix develop --command scripts/lint.sh --fix     # reformat in place
#
# clang-format covers every C++ source (including the Quest client's native
# code). clang-tidy only covers the meson-built sources, since it needs the
# compile_commands.json database; the Quest cpp is built by Gradle/NDK.
set -euo pipefail

cd "$(dirname "$0")/.."

fix=0
[[ "${1:-}" == "--fix" ]] && fix=1

# All hand-written C++ sources. Build outputs and the vendored Android SDK are
# excluded by only listing source directories.
mapfile -t cpp_files < <(find \
    src \
    client/desktop_test \
    client/gtk \
    client/quest/app/src/main/cpp \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | sort)

# --- clang-format ----------------------------------------------------------
if [[ "$fix" == 1 ]]; then
    echo ">> clang-format --fix (${#cpp_files[@]} files)"
    clang-format -i "${cpp_files[@]}"
else
    echo ">> clang-format --check (${#cpp_files[@]} files)"
    clang-format --dry-run --Werror "${cpp_files[@]}"
fi

# --- clang-tidy ------------------------------------------------------------
# A compile_commands.json with both optional sources enabled gives clang-tidy
# the widest coverage (portal + mutter capture backends).
build_dir=build-lint
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo ">> meson setup $build_dir (portal + mutter enabled)"
    meson setup "$build_dir" -Dportal=enabled -Dmutter=enabled >/dev/null
fi

mapfile -t tidy_files < <(find \
    src/streamer \
    client/desktop_test \
    client/gtk \
    -type f -name '*.cpp' | sort)

echo ">> clang-tidy (${#tidy_files[@]} translation units)"
clang-tidy --quiet -p "$build_dir" "${tidy_files[@]}"

echo "lint OK"
