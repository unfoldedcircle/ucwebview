#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Generates the public wpe-webkit-2.0 / wpe-platform-2.0 headers of an official WPE WebKit release tarball for the
# host checks (make syntax-check, syntax-check-negative, check-wpe-fake), without building WebKit: the tarball is
# downloaded once into a cache directory, its SHA-256 checked, only the API directories extracted, and
# lib/tests/mk-host-headers.py run on them. An output directory that already holds the headers of that version is
# kept.
#
# usage: scripts/wpe-host-headers.sh OUTPUT_DIR
#
# Environment:
#   WPE_VERSION    release to use (default 2.54.2)
#   WPE_SHA256     expected SHA-256 of wpewebkit-$WPE_VERSION.tar.xz (default: the published sum of 2.54.2;
#                  required for any other version)
#   WPE_CACHE_DIR  where the tarball is kept (default .cache in the repository)
#   WPE_TARBALL    a local copy of the tarball to use instead of a download (its SHA-256 is checked as well)
#   PYTHON         Python 3 interpreter (default python3)
set -eu

die() {
    echo "wpe-host-headers: $*" >&2
    exit 1
}

[ $# -eq 1 ] || die "usage: $0 OUTPUT_DIR"
out=$1

top=$(cd "$(dirname "$0")/.." && pwd)
version=${WPE_VERSION:-2.54.2}
case "$version" in
2.54.2) known_sha256=f3284e19e47fcb5ac8c63c2e1c113dbd65b2423eb8a545f5b345707829004f56 ;; # wpewebkit-2.54.2.tar.xz.sums
*) known_sha256= ;;
esac
sha256=${WPE_SHA256:-$known_sha256}
[ -n "$sha256" ] || die "no known SHA-256 for WPE WebKit $version: set WPE_SHA256"
cache=${WPE_CACHE_DIR:-$top/.cache}
python=${PYTHON:-python3}
name=wpewebkit-$version
url=https://wpewebkit.org/releases/$name.tar.xz

if [ -f "$out/VERSION" ] && [ "$(cat "$out/VERSION")" = "$version" ]; then
    echo "wpe-host-headers: $out already has the headers of WPE WebKit $version"
    exit 0
fi

check_sha256() {
    actual=$(sha256sum "$1" | cut -d' ' -f1)
    [ "$actual" = "$sha256" ] || die "$1: SHA-256 $actual, expected $sha256"
}

if [ -n "${WPE_TARBALL:-}" ]; then
    tarball=$WPE_TARBALL
else
    tarball=$cache/$name.tar.xz
    if [ ! -f "$tarball" ]; then
        mkdir -p "$cache"
        echo "wpe-host-headers: downloading $url"
        curl -fsSL --retry 3 -o "$tarball.part" "$url" || die "download of $url failed"
        mv "$tarball.part" "$tarball"
    fi
fi
check_sha256 "$tarball"

# Only what mk-host-headers.py reads, into a fresh directory of its own.
src=$(mktemp -d "${TMPDIR:-/tmp}/wpe-host-src.XXXXXX")
trap 'rm -rf "$src"' EXIT
tar -xJf "$tarball" -C "$src" \
    "$name/Source/WebKit/UIProcess/API/glib" \
    "$name/Source/WebKit/UIProcess/API/wpe" \
    "$name/Source/WebKit/WPEPlatform" \
    "$name/Source/WebKit/WebProcess/InjectedBundle/API/glib" \
    "$name/Source/WebKit/WebProcess/InjectedBundle/API/wpe" \
    "$name/Source/WebKit/PlatformWPE.cmake" \
    "$name/Source/cmake/OptionsWPE.cmake"

rm -rf "$out"
mkdir -p "$out"
"$python" -I "$top/lib/tests/mk-host-headers.py" --expect-version "$version" "$src/$name" "$out"
