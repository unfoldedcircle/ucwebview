# Changelog

All notable changes to this project are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the library follows the versioning rules of the
[README](README.md#abi-and-versioning): a minor release adds exports or callbacks within `UC_WEBVIEW_API_VERSION`,
a patch release changes behaviour without a new export or type.

## Unreleased

### Fixed

- libucwebview: `uc_webview_global_set_cookies()` no longer does pointer arithmetic on a NULL cookie value before
  refusing it (undefined behaviour that clang's UBSan reports; the result was correct). No ABI change.

## [0.10.1]

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

[0.10.1]: https://github.com/unfoldedcircle/ucwebview/releases/tag/v0.10.1
