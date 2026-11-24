#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Builds flutter-pi with the uc_webview plugin on the host, the way a firmware build does it: a copy of a flutter-pi
# checkout, patches/*.patch applied, this directory copied to src/plugins/uc_webview with uc_webview.h of this
# checkout, BUILD_UC_WEBVIEW_PLUGIN=ON. The checkout itself is not changed. A Debug build, which flutter-pi compiles
# with -Werror; extra CMake arguments pass through (e.g. -DCMAKE_BUILD_TYPE=Release).
#
# Needs flutter-pi's build dependencies (libdrm, gbm, libsystemd, libinput, xkbcommon, libudev, EGL, GLESv2) and
# glib-2.0. flutter-pi links no Flutter engine at build time (it loads it at run time).
#
# usage: scripts/build-flutter-pi.sh <flutter-pi checkout> <build dir> [cmake arguments...]
set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <flutter-pi checkout> <build dir> [cmake arguments...]" >&2
    exit 2
fi
src=$1
out=$2
shift 2
here=$(cd "$(dirname "$0")/.." && pwd)

rm -rf "$out/flutter-pi" "$out/build"
mkdir -p "$out"
# the tracked files only (a git checkout), or everything
if git -C "$src" rev-parse --git-dir >/dev/null 2>&1; then
    mkdir -p "$out/flutter-pi"
    git -C "$src" archive --format=tar HEAD | tar -x -C "$out/flutter-pi"
    echo "flutter-pi $(git -C "$src" rev-parse --short HEAD)"
else
    cp -a "$src" "$out/flutter-pi"
fi

for patch in "$here"/patches/*.patch; do
    patch -d "$out/flutter-pi" -p1 --forward --quiet < "$patch"
done
# the plugin without its host tests and build products (the build directory may be inside it: .cache/)
mkdir -p "$out/flutter-pi/src/plugins/uc_webview"
tar -C "$here" --exclude=./.cache --exclude=./tests -cf - . |
    tar -C "$out/flutter-pi/src/plugins/uc_webview" -xf -
cp "$here/../lib/uc_webview.h" "$out/flutter-pi/src/plugins/uc_webview/core/"

cmake -S "$out/flutter-pi" -B "$out/build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_UC_WEBVIEW_PLUGIN=ON \
    -DBUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=OFF -DBUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN=OFF -DENABLE_SESSION_SWITCHING=OFF \
    -DLTO=OFF "$@"
cmake --build "$out/build" -j "$(nproc)"
# the plugin is in: its channel name (LTO folds the registration function into others)
grep -qa "uc.webview/control" "$out/build/flutter-pi"
echo "flutter-pi with the uc_webview plugin: $out/build/flutter-pi"
