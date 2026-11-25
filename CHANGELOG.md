# Changelog

All notable changes to this project are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the library follows the versioning rules of the
[README](README.md#abi-and-versioning): a minor release adds exports or callbacks within `UC_WEBVIEW_API_VERSION`,
a patch release changes behaviour without a new export or type.

## [0.11.0] - 2026-11-25

### Added

- libucwebview: `uc_webview_library_version()` returns the library's version, the `VERSION` it was built with
  (`"0.11.0"`, `"0.0.0-dev"` for a build without one), from any thread and before `uc_webview_global_init()`. A new
  export within API version 1 (52 instead of 51): hosts resolve it with `dlsym()`, an older library has none. The
  flutter-pi plugin's `hello` reply carries it as `library` (it was always `null`). The library's start line names the
  version: `ucwebview 0.11.0 (API 1): WPE WebKit ...` instead of `ucwebview 1: WPE WebKit ...`.
- ucwebview-test: `--version` prints the version of the library it loaded next to its own
  (`ucwebview-test 0.11.0 (API 1), library 0.11.0 (API 1)`), and `--selftest` checks that the library reports the
  version the tool was built with.

## [0.10.4] - 2026-11-24

### Added

- flutter-pi plugin (`flutter-pi/`): shows the library's web views as flutter-pi textures (DMA-BUF frames zero copy)
  and drives them over two plain platform channels, `uc.webview/control` and `uc.webview/events`, with everything of
  `uc_webview.h` remote-ui's Qt host uses: views, navigation, zoom and pinch zoom, touch, pointer, keys and text
  input, the page's dialogs and logins as requests, site settings, the device state for web pages. The protocol is in
  [flutter-pi/PROTOCOL.md](flutter-pi/PROTOCOL.md), with golden byte files for contract tests. The plugin is built
  into flutter-pi (a CMake option patch) and `dlopen()`s the library; optional library functions are reported as
  features. No change to the library.
  Requests (dialogs, logins) pending when the app stops listening, or whose event could not be sent, are answered
  with the library's default, so the page never waits for an answer nobody can give; log lines never show parts of
  the user info of a malformed URL.

### Changed

- libucwebview: the library sets WebKit's cache model to `DOCUMENT_BROWSER` (it set none, so WebKit used
  `WEB_BROWSER`). WebKit's web process cache is off then: the web process of the last view now ends when the view is
  freed, instead of staying (about 100 MB) for 5 minutes for a later page of the same site on a system with 2 GB of
  RAM or more. Back and forward in a view still come from the back/forward cache (2 pages, as before); WebKit's memory
  cache is smaller (96 instead of 128 MB with 2 GB of RAM) and its disk cache at most 75 MB instead of 1 GB. `UC_WEBVIEW_CACHE_MODEL=viewer|browser|web-browser` selects another
  model; the start log names it. After a view went to another site, a host with a data directory still keeps one
  spare web process when the last view is freed (about 53 MB on a Remote with 4 GB of RAM): it does not depend on the
  cache model, and WebKit's API has no switch for it (lib/README.md names the WebKit change a later release could
  carry). No ABI change.
- ucwebview-test: `--process-test` counts the web processes left after the last view is freed and checks back and
  forward across two sites with the library's cache model; the distribution CI runs it with all three models.

## [0.10.3] - 2026-11-23

### Fixed

- libucwebview: an https load could fail with TIMEOUT ("Operation was cancelled") a moment after it started when
  WebKit did not accept the server's certificate yet. WebKit's preconnect to the server had rejected the certificate,
  and the load went on with the preconnect's cancelled connection without checking the certificate itself: a
  certificate exception for the server did not apply (the page did not show), and without one the host got TIMEOUT
  instead of CERTIFICATE. The library now loads such a load again (an https load cancelled less than 5 s after it
  started, at most twice within 10 s for one URI); the host sees STARTED and FINISHED without a failure, then the new
  load. A server that never answers still fails with TIMEOUT after 10 s. No ABI change.
- libucwebview: a load the library repeats (a certificate exception accepted the certificate, or the case above) no
  longer replaces a navigation the host started in the meantime, for example from its FINISHED callback.
- libucwebview: without a host EGLDisplay, DMA-BUF frames handed over as `pixels` have the stride of the pixels WebKit
  returns. WPE WebKit 2.54.3 returns rows without padding, and the library passed the padded stride of the buffer's
  plane: for every width whose rows are padded a sheared picture, and a host that read `height` rows of that stride
  read past the end of the pixels. The stride now comes from the size of the pixels, right with 2.54.2 (the mapping's
  rows) and 2.54.3; a frame whose size is not whole rows is dropped with a warning. No ABI change.

### Added

- ucwebview-test: `--load-fail-test` has a `tls-race` step (`--load-fail-tls-hosts N`) that times HTTPS loads of new
  hosts against WebKit's preconnect, so that a part of them takes over the connection of a preconnect that rejected
  the certificate; it needs `--load-fail-tls-cert` and adds about 25 s.

## [0.10.2] - 2026-11-23

First public release. The library and the launcher have shipped in the firmware of the Remote Two and Remote 3; the
version continues the firmware's library releases. API version 1, 51 exported functions,
`sizeof(uc_webview_callbacks)` 144 bytes on 64-bit hosts.

### libucwebview

- In-process WPEPlatform display for WPE WebKit 2.54 (WPEDisplay, screen with frame pacing at the host's refresh rate,
  view, toplevel, keymap without XKB data, input method context, gesture controller) behind a plain C ABI,
  `uc_webview.h`, for hosts that `dlopen()` the library; the header compiles as C89, C11 and C++11.
- Frames as EGLImages imported on the host's EGLDisplay (DMA-BUF with modifiers, zero copy) or as shared memory
  pixels; explicit frame lifecycle with back-pressure (`uc_webview_frame_rendered()`, `uc_webview_frame_release()`).
- Input: touch with WebKit's tap and scroll gestures and a 16 px drag threshold, pointer, scroll, keys as XKB keysyms
  with evdev keycodes (DOM `key`, `code` and `keyCode` for remote control keys), text input with pre-edit, commit and
  surrounding text, including a focused element whose web process was swapped away.
- Pinch zoom recognised in the library with a host-side preview, then `uc_webview_set_magnification()` around a focal
  point; page zoom; `uc_webview_when_rendered()` to wait for a rendering update without giving the page a user
  gesture.
- Requests: script dialogs, the "leave this page?" question, HTTP logins, `window.close()`, new windows and refused
  downloads reach the host through callbacks and are answered by id; without them the library applies documented
  defaults and a dialog loop guard.
- Site settings: request headers for one origin (bearer or API key authentication through the library's web process
  extension), user agent presets (the default identifies the Remote and its firmware version), cookies from the host,
  certificate exceptions by SHA-256 fingerprint, TLS error policy, dark color scheme, clearing website data; cookies of
  a persistent session in a private SQLite file.
- Device state for web pages (opt-in per view): `navigator.getBattery()`, `window.deviceHost` with the screen and
  power state, document `freeze`/`resume` around a standby, delivered without user activation for the page; hidden
  pages throttled.
- Load failures with a reason (host not found, no network, connection refused, timeout, certificate, refused scheme)
  through `load_failed_detail`.
- URI scheme allowlist (`file:` only on request), WebKit's PDF viewer off by default, an opaque white default
  background.
- Logs made to be shared: no typed text, dialog texts, credentials or header and cookie values, URIs as their origin
  only (`UC_WEBVIEW_LOG_FULL_URLS=1` for debugging).
- `ucwebview-test`: a command line host that renders a URL headless, dumps frames to PNG and runs self-checking modes
  for IME, dialogs, logins, windows, downloads, links, site settings, device state, load failures, magnification,
  pinch, zoom and frame pacing.

### wpe-launcher

- Fullscreen kiosk browser on WPEPlatform's DRM platform without a compositor; reload with back-off after a web
  process crash or a failed load; D-Bus control interface `org.unfoldedcircle.WebKiosk` (`Open`, `Reload`, `GetUri`,
  `Quit`); `--headless` smoke test on WPE's headless platform.

### Fixed since the firmware's 0.10.1

- libucwebview: `uc_webview_global_set_cookies()` no longer does pointer arithmetic on a NULL cookie value before
  refusing it (undefined behaviour that clang's UBSan reports; the result was correct). No ABI change.

[0.11.0]: https://github.com/unfoldedcircle/ucwebview/releases/tag/v0.11.0
[0.10.4]: https://github.com/unfoldedcircle/ucwebview/releases/tag/v0.10.4
[0.10.3]: https://github.com/unfoldedcircle/ucwebview/releases/tag/v0.10.3
[0.10.2]: https://github.com/unfoldedcircle/ucwebview/releases/tag/v0.10.2
