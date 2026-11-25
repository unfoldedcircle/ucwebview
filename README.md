# ucwebview

Embedding [WPE WebKit](https://wpewebkit.org/) 2.54 with its WPEPlatform API, as used in the firmware of the
Unfolded Circle Remote Two and Remote 3:

- **[lib/](lib/README.md): libucwebview**, a C/GLib shared library that runs a WPE WebKit web view inside another
  process and exposes it through a small, versioned, plain C ABI ([`uc_webview.h`](lib/uc_webview.h)). Frames go to
  the host as EGLImages on its own EGLDisplay (DMA-BUF, zero copy) or as shared memory pixels; touch, keys and text
  input come in through the C API. No Wayland compositor, no DRM master and no XKB data needed. The directory also
  holds `ucwebview-test`, a command line host that renders pages headless, dumps frames to PNG and runs
  self-checking test modes, and the unit tests.
- **[launcher/](launcher/README.md): wpe-launcher**, a fullscreen kiosk browser on WPEPlatform's DRM platform
  (`WPE_PLATFORM=drm`) without a compositor, with a small D-Bus control interface.
- **[flutter-pi/](flutter-pi/README.md): a flutter-pi plugin** that shows the library's web views as Flutter
  textures, driven over a `MethodChannel` and an `EventChannel` ([protocol](flutter-pi/PROTOCOL.md)); built into
  flutter-pi, the library `dlopen()`ed.

The library is `dlopen()`ed by the web view of [remote-ui](https://github.com/unfoldedcircle/remote-ui), the Qt user
interface of the remotes, which keeps a copy of `uc_webview.h`. What a web page sees of the remote (the device state
API) is documented with the [Core-API's web view entity](https://github.com/unfoldedcircle/core-api/blob/main/doc/entities/entity_webview.md),
and the [device API simulator](https://github.com/unfoldedcircle/core-api/tree/main/tools/device-api-simulator) lets
page authors try it on a desktop.

## Requirements

| Component | Needs |
|-----------|-------|
| both | a C11 compiler (GCC or Clang), GNU make, pkg-config, GLib/GIO 2.76 or newer |
| `lib/` library | WPE WebKit 2.54.x built with the WPEPlatform API: pkg-config modules `wpe-webkit-2.0`, `wpe-platform-2.0`, `wpe-web-process-extension-2.0`; `egl`; `libsoup-3.0` (a WebKit dependency anyway) |
| `lib/` ucwebview-test | the library, `glesv2`, `libpng`, `libsoup-3.0` |
| `launcher/` | `wpe-webkit-2.0`, `wpe-platform-2.0` with the DRM platform (and the headless platform for `--headless`), `gio-unix-2.0`; at run time the XKB data (xkeyboard-config) and optionally a D-Bus system bus |
| host checks | GLib development files only; `dbus-daemon` for the launcher's D-Bus test; Node.js 19 or newer (optional) for the page script test; Python 3, `glib-mkenums` and the libxkbcommon development files (a WPEPlatform header includes `xkbcommon.h`) for the syntax check; `curl`, `xz` and `sha256sum` to fetch the WPE release tarball; `shellcheck` for `make lint` |

Tested with WPE WebKit 2.54.1 and 2.54.2 (the remotes' firmware, aarch64, GLib 2.76) and 2.54.3 (Debian testing/sid
and Arch Linux packages, x86_64, GLib 2.90). The packages of distributions that ship WPE WebKit 2.54 with WPEPlatform
are enough for a full build and the runtime tests, see [docs/ci-level-2.md](docs/ci-level-2.md).

## Build

```shell
make                                     # lib/ (library, extension, ucwebview-test) and launcher/; needs WPE WebKit
make -C lib VERSION=0.10.4 all ucwebview.pc
make -C lib install DESTDIR=/tmp/stage   # PREFIX=/usr: /usr/lib/libucwebview.so.1, the extension, header, .pc, tool
make -C launcher VERSION=0.10.4 wpe-launcher
```

Both directories have plain Makefiles that a firmware or distribution build calls the same way. A cross build passes
the toolchain on the command line, which overrides the Makefiles' assignments (they collect their own flags in
`ALL_CFLAGS`), for example in a Buildroot generic package:

```make
$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) VERSION=0.10.4 WITH_TEST=1 PREFIX=/usr -C $(@D)/lib all ucwebview.pc
$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) VERSION=0.10.4 PREFIX=/usr -C $(@D)/lib DESTDIR=$(TARGET_DIR) install
```

Variables (command line or environment; the header of each Makefile repeats them):

| Variable | Default | Used by | Meaning |
|----------|---------|---------|---------|
| `CC`, `CFLAGS`, `LDFLAGS` | make's | both | compiler and flags; the Makefiles add their warnings and defines in `ALL_CFLAGS` |
| `PKG_CONFIG` | `pkg-config` | both | pkg-config to use; a cross build passes its sysroot wrapper |
| `VERSION` | `0.0.0-dev` | both | the version compiled in (`uc_webview_library_version()` and `ucwebview-test --version`, `wpe-launcher --version`) and written to `ucwebview.pc`; release builds pass the tag without its `v` |
| `PREFIX` | `/usr` | both | installation prefix |
| `libdir`, `includedir`, `bindir` | `$(PREFIX)/lib`, `$(PREFIX)/include`, `$(PREFIX)/bin` | lib (launcher: `bindir`) | installation directories |
| `DESTDIR` | empty | both | staging root of `make install` |
| `WITH_TEST` | `1` | lib | `1`: `all` and `install` include `ucwebview-test`; `0`: the library only |
| `EXT_DIR` | `$(libdir)/ucwebview/web-process-extensions` | lib | directory of the web process extension, compiled into the library |
| `SOUP_CFLAGS`, `SOUP_LIBS` | from `PKG_CONFIG` | lib | libsoup-3.0 flags of `ucwebview-test`; without its development files on the build host pass `SOUP_CFLAGS=-I<sysroot>/usr/include/libsoup-3.0` |
| `WPE_HOST_INCLUDE` | `/nonexistent` (top level: `.cache/wpe-host-include-$(WPE_VERSION)`) | both | output of `lib/tests/mk-host-headers.py`, for the syntax checks |
| `NODE` | `node` | lib | Node.js for `check-js` |
| `GTK_HOST_LIBDIR` | `/usr/lib/x86_64-linux-gnu` | lib | WebKitGTK 4.1 runtime libraries of the `ucwebview-test-gtk` developer harness |
| `WPE_VERSION` | `2.54.2` | top level | WPE WebKit release of `make wpe-headers` (`WPE_SHA256`, `WPE_CACHE_DIR`, `WPE_TARBALL`: see `scripts/wpe-host-headers.sh`) |

## Test

Without WPE WebKit (what CI level 1 runs on every push and pull request):

```shell
make check            # GLib unit tests of lib/ and launcher/, the header as C89 and C++11, the page script in Node
make check-asan       # lib/'s tests with AddressSanitizer and UBSan
make syntax-check     # fetches the WPE WebKit 2.54.2 release tarball once (SHA-256 checked), generates its public
                      # headers and compiles every WPE source of lib/ and launcher/ against them (-fsyntax-only), a
                      # negative probe that must fail, and the screen glue against a fake WPEScreen
make lint             # SPDX headers, white space, shellcheck
make -C flutter-pi check   # the flutter-pi plugin's host tests against a fake library (GLib and libsystemd), also
                           # check-asan and check-fixtures; flutter-pi-build builds it into flutter-pi, see
                           # flutter-pi/README.md
```

With WPE WebKit (CI level 2, in a disposable container: it installs into the container's `/usr`):

```shell
docker run --rm -v "$PWD":/src:ro debian:testing sh -c 'cp -a /src /build && /build/scripts/ci-distro.sh --deps'
```

`scripts/ci-distro.sh` builds both components against the distribution's WPE WebKit, installs them and runs
`ucwebview-test`'s self-checking modes (rendering, IME, dialogs, logins, new windows, downloads, device state, links,
site settings with TLS exceptions and persistent cookies, load failures, magnification, pinch, zoom) and the
launcher on WPE's headless platform. On a device, [lib/README.md](lib/README.md#checks-on-a-device) lists the checks
with their expected output.

## How hosts load the library

Hosts do not link the library; they `dlopen()` it, so that a missing or older library only disables the web view:

```c
#include <dlfcn.h>
#include "uc_webview.h"   /* a copy of the header of one library release, unchanged */

/* the type of a function comes from the header, its address from the library (GCC/Clang __typeof__) */
#define RESOLVE(lib, name) ((__typeof__(&name)) dlsym((lib), #name))

void *lib = dlopen("libucwebview.so.1", RTLD_NOW | RTLD_LOCAL);
__typeof__(&uc_webview_api_version) api_version = lib ? RESOLVE(lib, uc_webview_api_version) : NULL;
if (api_version == NULL || api_version() != UC_WEBVIEW_API_VERSION)
    return;                                     /* no web view: no library, or another API version */
__typeof__(&uc_webview_global_init) global_init = RESOLVE(lib, uc_webview_global_init);
__typeof__(&uc_webview_new) new_view = RESOLVE(lib, uc_webview_new);
__typeof__(&uc_webview_new_sized) new_sized = RESOLVE(lib, uc_webview_new_sized);  /* 0.4.0: may be NULL */

uc_webview_callbacks cb = { 0 };                /* zero-initialised, then the callbacks the host handles */
cb.frame_ready = on_frame_ready;
global_init(egl_display, data_dir, NULL);       /* on the UI thread, which iterates GLib's default main context */
uc_webview *view = new_sized ? new_sized(480, 800, &cb, sizeof cb, host) : new_view(480, 800, &cb, host);
```

Every function is called on the thread that called `uc_webview_global_init()`, and that thread iterates GLib's
default main context (Qt does with its GLib event dispatcher); callbacks arrive there. A render thread may use a
frame's EGLImage but hands `uc_webview_frame_rendered()`/`_release()` back to the UI thread. The full contract is at
the top of [`uc_webview.h`](lib/uc_webview.h) and in [lib/README.md](lib/README.md#threading-contract);
remote-ui's [docs/webview.md](https://github.com/unfoldedcircle/remote-ui/blob/main/docs/webview.md) describes a
complete Qt Quick host.

## ABI and versioning

- **`UC_WEBVIEW_API_VERSION`** (1) is the ABI. It changes only with an incompatible change (a removed or changed
  function, a changed struct layout or value); the soname `libucwebview.so.1` follows it. Hosts compare
  `uc_webview_api_version()` with the value they were compiled against and treat a mismatch as "web view
  unavailable".
- **Library releases** are `MAJOR.MINOR.PATCH` (`VERSION`, git tags `vX.Y.Z`). Within API version 1 a minor release
  adds exports or appends callbacks; a patch release changes behaviour without a new export or type. Hosts and the
  library come from different releases in both directions (an application built against a newer header runs on an
  older system, and the other way round), so:
  - new functions are additive and name the release that added them; hosts resolve them with `dlsym()`;
  - `uc_webview_callbacks` only grows at its end and is read through `uc_webview_new_sized()` with the host's
    `sizeof`; `uc_webview_frame` and `uc_webview_cookie` never change within API version 1;
  - hosts ignore constant values they do not know.

  [lib/README.md](lib/README.md#compatibility-within-api-version-1) has the rules and the table of what each release
  added; [CHANGELOG.md](CHANGELOG.md) the releases.
- The launcher is versioned with the repository.

## Repository layout

| Path | Content |
|------|---------|
| `lib/` | libucwebview, its web process extension, `uc_webview.h`, `ucwebview-test`, `tests/` (unit tests, the WPE header generator, the WebKitGTK harness) |
| `launcher/` | wpe-launcher and its tests |
| `flutter-pi/` | the flutter-pi plugin: a host-neutral C core (loader, codec, protocol), the flutter-pi adapter, the CMake option patch, host tests |
| `scripts/` | the WPE header download, the lints, the distribution build of CI level 2 |
| `webkit-patches/` | an optional patch for WPE WebKit (key values of remote control keys), see its [README](webkit-patches/README.md) |
| `docs/` | [CI level 2](docs/ci-level-2.md) |

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md). Please report security vulnerabilities as described in Unfolded Circle's
[security policy](https://github.com/unfoldedcircle/.github/blob/main/SECURITY.md), not as public issues.

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Unfolded Circle ApS.
