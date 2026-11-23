#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Builds lib/ and launcher/ against the WPE WebKit of a Linux distribution, installs them under /usr and runs the
# smoke tests: ucwebview-test renders a page in software (shared memory frames) and runs its self-checking modes,
# wpe-launcher loads a page on WPE's headless platform. Meant for a disposable container running as root (CI level 2,
# docs/ci-level-2.md); it installs into the container's /usr.
#
# usage: scripts/ci-distro.sh [--deps]
#   --deps   install the build and test dependencies first (apt-get: Debian sid; pacman: Arch Linux)
#
# Environment: VERSION (default: the version of CHANGELOG.md's latest release), UC_SMOKE_MODES (the ucwebview-test
# runs, one command line per line, default below; $cert and $data name a test certificate and a data directory),
# UC_SMOKE_TIMEOUT (seconds per run, default 300). --raf-test is left out: it measures timing, which a shared CI
# machine does not keep.
set -eu

cd "$(dirname "$0")/.."

if [ "${1:-}" = "--deps" ]; then
    if command -v apt-get >/dev/null 2>&1; then
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends build-essential pkgconf libglib2.0-dev libegl-dev libgles-dev \
            libpng-dev libsoup-3.0-dev libwpewebkit-2.0-dev nodejs dbus fonts-dejavu-core ca-certificates openssl
    elif command -v pacman >/dev/null 2>&1; then
        pacman -Syu --noconfirm --needed base-devel pkgconf glib2 libglvnd mesa libpng libsoup3 wpewebkit nodejs dbus \
            ttf-dejavu openssl
    else
        echo "ci-distro: no apt-get or pacman, install the dependencies yourself" >&2
        exit 1
    fi
fi

version=${VERSION:-$(sed -n 's/^## \[\([0-9][0-9.]*\)\].*/\1/p' CHANGELOG.md | head -n 1)}
echo "== WPE WebKit $(pkg-config --modversion wpe-webkit-2.0), WPEPlatform $(pkg-config --modversion wpe-platform-2.0)," \
    "GLib $(pkg-config --modversion glib-2.0), $(cc --version | head -n 1); building $version"

make -C lib clean >/dev/null
make -C launcher clean >/dev/null
make -C lib VERSION="$version" all ucwebview.pc 2>&1 | tee /tmp/ci-distro-build.log
make -C launcher VERSION="$version" wpe-launcher 2>&1 | tee -a /tmp/ci-distro-build.log
echo "== compiler warnings: $(grep -c 'warning:' /tmp/ci-distro-build.log || true)"

echo "== exports"
exports=$(nm -D --defined-only lib/libucwebview.so.1 | grep ' T ' | awk '{print $3}')
others=$(echo "$exports" | grep -cv '^uc_webview_' || true)
echo "libucwebview.so.1: $(echo "$exports" | grep -c '^uc_webview_') uc_webview_* functions, $others other"
[ "$others" -eq 0 ]
ext=$(nm -D --defined-only lib/libucwebview-extension.so | grep ' T ' | awk '{print $3}')
echo "libucwebview-extension.so: $ext"
[ "$ext" = "webkit_web_process_extension_initialize_with_user_data" ]

make -C lib PREFIX=/usr install
make -C launcher PREFIX=/usr install
ldconfig 2>/dev/null || true

echo "== unit tests"
make check

# Containers cannot create the namespaces of WebKit's bubblewrap sandbox (distribution builds enable it).
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
tmo=${UC_SMOKE_TIMEOUT:-300}
status=0
run() {
    echo "== $*"
    start=$(date +%s)
    if timeout "$tmo" "$@"; then
        echo "== ok ($(($(date +%s) - start)) s): $*"
    else
        echo "== FAILED ($?): $*"
        status=1
    fi
}

run ucwebview-test --selftest
page=$(mktemp /tmp/ucwebview-page.XXXXXX.html)
printf '<html><body style="background:#0080ff"><h1>ucwebview</h1></body></html>\n' > "$page"
run ucwebview-test --egl none --html "$page" --frames 1 --out /tmp/ucwebview-frame.png --timeout 60
# A self-signed certificate for 127.0.0.1, 127.0.0.2 and localhost, as lib/README.md describes for --site-test.
work=$(mktemp -d /tmp/ucwebview-smoke.XXXXXX)
cert=$work/site-test.pem
data=$work/site-data
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$work/key.pem" -out "$work/cert.pem" \
    -days 30 -subj /CN=ucwebview-test -addext subjectAltName=DNS:localhost,IP:127.0.0.1,IP:127.0.0.2 2>/dev/null
cat "$work/cert.pem" "$work/key.pem" > "$cert"
# shellcheck disable=SC2016 # $cert and $data are replaced per line below
modes=${UC_SMOKE_MODES:-'--ime-test
--ime-swap-test
--dialog-test
--auth-test
--window-test
--download-test
--device-test
--link-test
--site-test --site-tls-cert $cert --data-dir $data
--site-test --site-tls-cert $cert --data-dir $data --site-expect-persisted
--load-fail-test --load-fail-tls-cert $cert
--magnify 2@100,700
--pinch 100:250@240,400
--pinch 100:250 --prevent-touch
--zoom-test 125,150,100'}
while IFS= read -r line; do
    [ -n "$line" ] || continue
    line=$(echo "$line" | sed -e "s|\$cert|$cert|g" -e "s|\$data|$data|g")
    # shellcheck disable=SC2086 # one command line per line, split into its arguments
    run ucwebview-test --egl none $line </dev/null
done <<EOF
$modes
EOF
run env WPE_PLATFORM=headless wpe-launcher --headless --timeout 60 'data:text/html,<p>wpe-launcher</p>'
run wpe-launcher --version

[ $status -eq 0 ] && echo "== ci-distro: all checks passed" || echo "== ci-distro: FAILED"
exit $status
