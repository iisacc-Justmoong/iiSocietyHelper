#!/usr/bin/env bash
set -euo pipefail

# SDK validation must resolve the selected CMake targets, not inherited header
# or loader overrides from a previously installed SDK.
unset CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH LIBRARY_PATH
unset DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_FALLBACK_LIBRARY_PATH
unset DYLD_VERSIONED_LIBRARY_PATH DYLD_VERSIONED_FRAMEWORK_PATH

if [[ $# -ne 0 ]]; then
    printf '사용법: INSTALL_PREFIX=... QT_PREFIX_PATH=... CMAKE_PREFIX_PATH=... ./install.sh\n' >&2
    exit 2
fi

source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$source_dir/build"
install_prefix="${INSTALL_PREFIX:-$HOME/.local/SDK/iiSocietyHelper}"
qt_prefix="${QT_PREFIX_PATH:-}"
if [[ -z "$qt_prefix" && -d "/Volumes/Storage/Qt/6.8.3/macos" ]]; then
    qt_prefix="/Volumes/Storage/Qt/6.8.3/macos"
fi
search_prefixes="${CMAKE_PREFIX_PATH:-}"
for sdk_prefix in "$HOME/.local/SDK/"*; do
    if [[ -d "$sdk_prefix" ]]; then
        search_prefixes="${search_prefixes:+$search_prefixes;}$sdk_prefix"
    fi
done
if [[ -n "$qt_prefix" ]]; then
    search_prefixes="$qt_prefix${search_prefixes:+;$search_prefixes}"
fi

cmake -S "$source_dir" -B "$build_dir"     -DCMAKE_BUILD_TYPE=Release     -DBUILD_TESTING=ON     "-DCMAKE_INSTALL_PREFIX=$install_prefix"     "-DCMAKE_PREFIX_PATH=$search_prefixes"
cmake --build "$build_dir" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
ctest --test-dir "$build_dir" -C Release --output-on-failure
cmake --install "$build_dir" --config Release

consumer_build_dir="$build_dir/consumer/build"
# This directory belongs to the installer. Probe the compiler again so a cache
# created with CPATH cannot silently suppress required installed include paths.
cmake --fresh -S "$source_dir/tests/consumer" -B "$consumer_build_dir"     -DCMAKE_BUILD_TYPE=Release     "-DiiSocietyHelper_DIR=$install_prefix/lib/cmake/iiSocietyHelper"     "-DCMAKE_PREFIX_PATH=$install_prefix${search_prefixes:+;$search_prefixes}"
cmake --build "$consumer_build_dir" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
ctest --test-dir "$consumer_build_dir" -C Release --output-on-failure
