# libucwebview

A C/GLib shared library that runs a WPE WebKit 2.54 web view **inside** another process and talks to it through a
small, versioned, plain C ABI (`uc_webview.h`). The library implements an in-process WPEPlatform display: rendered
frames are handed to the embedding application as EGLImages imported on its own EGLDisplay (DMA-BUF, zero copy) or as
shared memory pixels, input events and text input (IME) come in through the C API. It needs no Wayland compositor, no
DRM master and no XKB data. The library is the only consumer of the WPE/WebKit headers; hosts
`dlopen("libucwebview.so.1")` and include only `uc_webview.h`.

Hosts: the WebView QML item of [remote-ui](https://github.com/unfoldedcircle/remote-ui) (Qt 5.15, `src/webview/`, host
side in its [docs/webview.md](https://github.com/unfoldedcircle/remote-ui/blob/main/docs/webview.md)) and
`ucwebview-test`, the command line host and test oracle of this directory. The web pages' side of the device state API
is documented in the [Core-API's web view
entity](https://github.com/unfoldedcircle/core-api/blob/main/doc/entities/entity_webview.md).

Build: see the [repository README](../README.md) and the variables at the top of the [Makefile](Makefile). License: MIT.
Section titles name the library release that brought a feature; [CHANGELOG.md](../CHANGELOG.md) lists the releases.

## Installed files

`make install` with `PREFIX=/usr` (the default):

| Path | Purpose |
|------|---------|
| `/usr/lib/libucwebview.so.1` (+ `libucwebview.so`) | the library, exports only `uc_webview_*` (`-fvisibility=hidden`) |
| `/usr/lib/ucwebview/web-process-extensions/libucwebview-extension.so` | the library's web process extension (0.8.0): every WPEWebProcess loads it and adds the origin headers of `uc_webview_set_origin_headers()` to requests, see "Site settings (0.8.0)"; since 0.9.0 it also creates the library's script world and starts and updates the device state of pages (`device-host-shim.js` compiled in), see "Device state for web pages (0.9.0)"; exports only `webkit_web_process_extension_initialize_with_user_data`. The directory is compiled into the library (`EXT_DIR`) |
| `/usr/include/ucwebview/uc_webview.h` | the ABI contract (remote-ui keeps a copy, `src/webview/uc_webview.h`) |
| `/usr/lib/pkgconfig/ucwebview.pc` | for hosts that link directly (the test tool) |
| `/usr/bin/ucwebview-test` | command line host and test oracle, see below (`WITH_TEST=1`, the default) |

Link dependencies of the library (`readelf -d`): `libWPEWebKit-2.0.so.1` (WPEPlatform is compiled into it in
2.54.1, `wpe-platform-2.0.pc` has `Libs: -lWPEWebKit-2.0`), `libgio/gobject/glib-2.0`, `libEGL`, and through
`wpe-webkit-2.0.pc` `libsoup-3.0` (called since 0.7.0) and `libgmodule-2.0`. No libgbm, libdrm, libxkbcommon, Wayland
or libinput. The web process extension (0.8.0) links `libWPEWebKit-2.0.so.1` (`wpe-web-process-extension-2.0.pc`; the
JavaScriptCore GLib API it calls since 0.9.0 is part of it), `libsoup-3.0` and GLib.

## The ABI (`uc_webview.h`)

`UC_WEBVIEW_API_VERSION 1`. Opaque `uc_webview *`, a `uc_webview_callbacks` struct, a `uc_webview_frame` struct,
integers and `void *` for EGL handles; no GLib/WPE/EGL types. Compiles as C89, C11 and C++11 (`make check` proves it).

| Group | Functions |
|-------|-----------|
| process | `uc_webview_api_version()`, `uc_webview_global_init(egl_display, data_dir, cache_dir)`, `uc_webview_global_shutdown()`, `uc_webview_global_set_refresh_rate(hz)`, `uc_webview_global_get_refresh_rate()` (library 0.2.0), `uc_webview_global_get_drag_threshold()` (library 0.3.0) |
| lifecycle | `uc_webview_new(w, h, cb, user)`, `uc_webview_new_sized(w, h, cb, cb_size, user)` (library 0.4.0), `uc_webview_free()`, `uc_webview_resize()`, `uc_webview_set_mapped()`, `uc_webview_set_focused()`, `uc_webview_set_scale()` |
| navigation | `load_uri`, `load_html`, `reload`, `stop`, `go_back`, `go_forward`, `can_go_back`, `can_go_forward`, `get_uri`, `get_title`, `run_javascript`, `set_zoom`, `set_background_rgba` (default opaque white since 0.6.2, black before) |
| magnification | `uc_webview_set_magnification(scale, x, y)`, `uc_webview_get_magnification()` (library 0.6.0, see "Pinch zoom") |
| rendering | `uc_webview_when_rendered(cb, user)` (library 0.5.0): `cb(wv, status, user)` once the page rendered, `UC_WEBVIEW_RENDERED_DONE`/`TIMEOUT`/`CANCELLED` |
| frames | `uc_webview_frame_rendered(handle)`, `uc_webview_frame_release(handle)` |
| input | `uc_webview_touch(id, phase, x, y, ms)`, `uc_webview_pointer_button()`, `uc_webview_pointer_move()`, `uc_webview_scroll()`, `uc_webview_key(xkb_keysym, keycode, pressed, mods, ms)` |
| text input | `uc_webview_ime_commit(utf8)`, `uc_webview_ime_preedit(utf8, cursor)`, `uc_webview_ime_delete_surrounding(offset, n)` |
| requests | `uc_webview_request_reply(id, accept, text)`, `uc_webview_request_auth(id, user, password)` (library 0.7.0, see "Requests (0.7.0)") |
| site settings | library 0.8.0, see "Site settings (0.8.0)": `uc_webview_set_origin_headers(origin, names, values, count)`, `uc_webview_set_user_agent(preset, custom)`, `uc_webview_global_set_cookies(url, cookies, count, cb, user)` with `uc_webview_cookie`, `uc_webview_global_allow_certificate(host, sha256)`, `uc_webview_global_set_tls_errors_policy(ignore)`, `uc_webview_global_set_color_scheme(scheme)`, `uc_webview_global_clear_data(types, cb, user)` |
| device state | library 0.9.0, see "Device state for web pages (0.9.0)": `uc_webview_set_device_api(enabled)`, `uc_webview_set_device_state(json)`, `uc_webview_device_event(type, slept_ms)` |
| callbacks | `frame_ready`, `load_changed(event, uri)`, `title_changed`, `progress_changed`, `web_process_terminated(reason)`, `ime_focus(focused, hints, purpose, surrounding, cursor)`, `ime_cursor_area`, `cursor_changed(css_name)`, `ime_surrounding(text, cursor, anchor)`; appended in library 0.6.0, read only through `uc_webview_new_sized()`: `pinch(phase, scale, x, y)`, `magnification_changed(magnification)`; appended in library 0.7.0, the same way: `script_dialog(id, type, origin, message, default_text)`, `authenticate(id, origin, realm, scheme, flags)`, `request_cancelled(id)`, `close_requested()`, `new_window(uri, result)`, `response_refused(uri, mime_type, reason)`; appended in library 0.10.0, the same way: `load_failed_detail(uri, reason, error_domain, error_code, tls_errors, message)` (see "Load failures (0.10.0)") |

Enumerations are `#define`s with the WPEPlatform/WebKit values and are `_Static_assert`ed against the real headers
in `uc_wpe_view.c`: load events = `WebKitLoadEvent` plus `UC_WEBVIEW_LOAD_FAILED` (4), termination reasons =
`WebKitWebProcessTerminationReason`, buttons = `WPE_BUTTON_*`, modifier bits = `WPEModifiers`, IME purpose/hints =
`WPEInputPurpose`/`WPEInputHints`; in `uc_requests_wk.c` (0.7.0) dialog types = `WebKitScriptDialogType`,
authentication schemes = `WebKitAuthenticationScheme`. Touch phases are 0 down, 1 move, 2 up, 3 cancel.

Also part of API version 1 from the first release (all additive to the core functions): `uc_webview_global_shutdown()`,
`uc_webview_get_uri()`, `uc_webview_get_title()`, `uc_webview_set_zoom()`, `uc_webview_set_scale()`
(`wpe_toplevel_scale_changed` → WebKit's intrinsic device scale factor), the `ime_surrounding` callback (text around
the caret changes while typing, needed for Qt's `ImSurroundingText`/`ImCursorPosition`), `UC_WEBVIEW_LOAD_FAILED`,
and the `pixels`/`stride` fields at the end of `uc_webview_frame` for shared memory frames (web process without a
GPU render node, `LIBGL_ALWAYS_SOFTWARE`, or `egl_display == NULL`): then `egl_image` is `NULL` and the frame carries
ARGB8888 pixels; the Qt item may ignore or `glTexImage2D` them.

Later releases add functions within API version 1: the header's rule keeps the version for new functions, an old host
simply never calls them, and a new host resolves them as optional symbols (`dlsym()`) and copes with their absence. A
new API version would make every host refuse every older library for no gain.

| Library | Adds (within API version 1) | Exports |
|---------|-----------------------------|---------|
| 0.1.0 | the functions and the nine callbacks of API version 1 | 32 |
| 0.2.0 | `uc_webview_global_set_refresh_rate()`, `uc_webview_global_get_refresh_rate()` (see "Frame pacing"); a 0.1.0 library paces at 60 Hz | 34 |
| 0.3.0 | `uc_webview_global_get_drag_threshold()` (see "Touch drag threshold") | 35 |
| 0.4.0 | `uc_webview_new_sized()` (see the next section) | 36 |
| 0.5.0 | `uc_webview_when_rendered()` (see "Rendered signal"; its callback is a function argument, so `uc_webview_callbacks` does not grow) | 37 |
| 0.6.0 | `uc_webview_set_magnification()`, `uc_webview_get_magnification()`; the first two callbacks appended to `uc_webview_callbacks`, `pinch` and `magnification_changed` (see "Pinch zoom") | 39 |
| 0.7.0 | `uc_webview_request_reply()`, `uc_webview_request_auth()`; six request callbacks appended (see "Requests (0.7.0)"); hosts detect it with `dlsym("uc_webview_request_reply")` | 41 |
| 0.8.0 | the seven site setting functions and the host-allocated struct `uc_webview_cookie`; the callback struct does not grow (see "Site settings (0.8.0)"); hosts resolve each function on its own | 48 |
| 0.9.0 | the three device state functions (see "Device state for web pages (0.9.0)"), resolved one by one like those of 0.8.0 | 51 |
| 0.10.0 | the callback `load_failed_detail` appended (see "Load failures (0.10.0)") | 51 |

Releases in between change behaviour without a new export or type (for example 0.6.1 switches WebKit's PDF viewer
off, 0.6.2 makes a new view's background opaque white, WebKit's own default, `WebKitWebViewWPE.cpp:226`).

### Compatibility within API version 1

The host and the library come from different releases: an application that embeds the library can run on whatever
system the device has, and a system update does not rebuild the application (on the Remote, a remote-ui custom UI and
the firmware). Within API version 1 either side's header can be the older one, so the rules in `uc_webview.h`
("Compatibility") cover both directions:

| Item | Allocated by | Rule |
|------|--------------|------|
| functions | | new ones are additive; each names the release that added it; hosts resolve those with `dlsym()` and cope with their absence |
| `uc_webview_callbacks` | host | fields are only appended. `uc_webview_new()` reads exactly the nine API version 1 callbacks (`frame_ready` .. `ime_surrounding`, 72 bytes on aarch64), whatever header the host has; later fields stay NULL. `uc_webview_new_sized(w, h, cb, sizeof(uc_webview_callbacks), user)` (0.4.0) reads `min(cb_size, the library's sizeof)` bytes and treats the rest as NULL; a host newer than the library is logged once (`the host's callback struct has N bytes, this library knows M`), a `cb_size` below the 72 bytes is refused with a warning and NULL. 0.6.0 appended `pinch` and `magnification_changed` (88 bytes on aarch64), 0.7.0 the six request callbacks (136 bytes), 0.10.0 `load_failed_detail` (144 bytes); `uc_abi.c` asserts their offsets right after the API version 1 fields. A host of a 0.6 header passes 88 bytes and gets the library's request defaults |
| `uc_webview_cookie` | host | 0.8.0. The host passes an array of them to `uc_webview_global_set_cookies()`, and the library steps through it with its own `sizeof`, so the struct keeps its layout (48 bytes on aarch64) for all of API version 1; `uc_abi.c` freezes it like the frame. A new cookie attribute needs a new function |
| `uc_webview_frame` | library | does not change within API version 1. Hosts copy it with the `sizeof` of their own header (remote-ui does), so a field appended for a newer host would be read past the end of an older library's frame; per-frame data added later come from new functions that take the frame handle |
| constants the library reports | | new load events, termination reasons, IME purposes or hint bits may appear; hosts ignore values they do not know |
| constants the host passes | | unknown touch phases and buttons are ignored with a warning, unknown modifier bits dropped |
| strings, scalars, handles | | strings are NUL terminated (no sizes); frame handles are only compared with the view's frames, never dereferenced before a match |

Up to 0.3.0 `uc_webview_new()` copied the whole struct (`wv->cb = *cb`) although the header called appended callback
fields additive: once a release appended one, a host built against the older header would have had the bytes after
its struct read as a function pointer. No release appended one, so no released combination was affected. Since 0.4.0
`uc_abi.c` does both copies and freezes the API version 1 layouts (`UcAbiCallbacksV1`, `UcAbiFrameV1` in `uc_abi.h`):
static asserts fail the build when a public field moves or changes its type or the frame grows (a positional
initializer under `-Werror=missing-field-initializers` also catches a field in the frame's tail padding).
`tests/test-abi.c` runs the copies on a host struct of exactly 72 bytes that ends at a `PROT_NONE` page and in
exact-size heap blocks for `make check-asan`, with negative controls that show the guard and ASan catch the 0.3.0 copy
of a grown struct.

What a host does: zero-initialise the callbacks (`uc_webview_callbacks cb{}`), resolve `uc_webview_new_sized` as an
optional symbol and call it with `sizeof(uc_webview_callbacks)` of its own build, or `uc_webview_new()` with the same
struct when an older library lacks it; copy frames by value at most; take `uc_webview.h` unchanged from one library
release.

### Threading contract

Every function, including `uc_webview_frame_rendered()`/`_release()`, is called on the thread that called
`uc_webview_global_init()` (the "UI thread"); that thread iterates the GLib default main context
(`g_main_context_default()`, which Qt's `QPAEventDispatcherGlib` drives on the GUI thread). All callbacks arrive on
that thread from the main context, never from inside a `uc_webview_*` call (a `frame_rendered`/`frame_release`
issued from inside `frame_ready` is completed from an idle, because WebKit is still inside
`wpe_view_render_buffer()` at that point, `AcceleratedBackingStore.cpp:283-293`), with one exception: a change of the
view's focus reports the page's focused editable element at once, `ime_focus(1)` inside the call that focuses the
view and `ime_focus(0)` inside the one that unfocuses it. WebKit notifies its input method from the view's
`has-focus` change (`WPEWebViewPlatform.cpp:100-110`, `InputMethodFilter::notifyFocusedIn/Out`); that is
`uc_webview_set_focused()`, and also the pointer press or touch tap with which WebKit focuses an unfocused view
itself (`WPEWebViewPlatform.cpp:368-370, 468`). Such a report repeats the hints WebKit computed when the element got
its focus (remote-ui uses this to tell it from a fresh focus, see its `docs/webview.md`); since 0.7.5 none comes for
an element whose web process is gone, and the first element of the next document is reported from the main context
("Text input (IME)"). The second exception
(0.5.0): `uc_webview_free()` ends the view's pending `uc_webview_when_rendered()` requests with
`UC_WEBVIEW_RENDERED_CANCELLED` inside the call; those callbacks must not call functions on the view. The request
callbacks of 0.7.0 keep the rule without exception: `uc_webview_free()` ends pending dialogs and logins without a
callback, and a request that a navigation call of the host ends is reported with `request_cancelled()` from the main
context after the call. A host may answer a request from inside its callback. A render
thread may use the
frame's EGLImage/GL objects but must marshal the two frame calls back to the UI thread. `uc_webview_free()` is not
allowed from a callback (logged and leaked instead of crashing). The library checks the calling thread and refuses
calls from other threads with a `g_critical`.

### Frame lifecycle

1. The web process renders into a DMA-BUF; WebKit calls our `WPEViewClass::render_buffer` (UI thread).
2. The library imports the buffer with `wpe_buffer_import_to_egl_image()` (exists in 2.54.1, `WPEBuffer.h:76`;
   `WPEBufferDMABuf.cpp:117-178` does `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` on `wpe_display_get_egl_display()`
   = the host's EGLDisplay, with `EGL_DMA_BUF_PLANEn_MODIFIER_LO/HI_EXT` when the display has
   `EGL_EXT_image_dma_buf_import_modifiers`; no GL context needs to be current), keeps a reference to the
   `WPEBuffer` and calls `frame_ready(frame)`. The EGLImage is cached in the buffer and destroyed with it
   (`WPEBufferDMABuf.cpp:71-88`), so it is valid until `uc_webview_frame_release()`.
3. `uc_webview_frame_rendered(handle)` → `wpe_view_buffer_rendered()` → `AcceleratedBackingStore::frameDone()`
   → the web process renders the next frame (`AcceleratedBackingStore.cpp:71-74, 301-305`). WebKit does not send a
   new frame before this (`frame()` asserts no pending buffer), so a host that never calls it gets one frame.
4. `uc_webview_frame_release(handle)` → `wpe_view_buffer_released()` → `ReleaseBuffer` to the web process
   (`AcceleratedBackingStore.cpp:312-319`), then the library drops its reference. Keep at most two frames.

Shared memory frames (`WPEBufferSHM`, `AcceleratedBackingStore.cpp:153-171`) are handed over as `pixels`. Without a host
EGLDisplay (`uc_webview_global_init()` with `NULL`) a DMA-BUF frame is mapped with `wpe_buffer_import_to_pixels()` and
handed over as `pixels` too; that `GBytes` is owned by the buffer (transfer none, `WPEBuffer.cpp:282-291`), so the
library takes its own reference (since 0.6.5). Its rows changed in WPE WebKit 2.54.3: up to 2.54.2 the `GBytes` is the
`gbm_bo_map()` mapping, `height` rows of the map's stride (`WPEBufferDMABuf.cpp:264-283` in 2.54.2), since 2.54.3 the
rows are not padded, `width * 4` bytes, and a mapping with another stride is copied row by row
(`WPEBufferDMABuf.cpp:273-283`, `wpeBufferCopyPixelsWithoutRowPadding()` in `WPEBuffer.cpp:285-296`, and the function's
documentation, "The rows of the returned data are not padded"). Up to 0.10.2 the library passed the stride of the
buffer's first plane (`wpe_buffer_dma_buf_get_stride()`), which is wrong with 2.54.3 for every width whose plane rows
are padded (a sheared picture, and a host that reads `height` rows of that stride reads past the end of the pixels), and
with 2.54.2 whenever the driver maps through a copy with another stride. Since 0.10.3 `stride` is the size of the
`GBytes` divided by `height`, which is right for both without a version check; a size that is not `height` rows of at
least `width * 4` bytes drops the frame with a warning (`uc_formats_pixels_stride()`, unit-tested in
`tests/test-formats.c`). Hosts always use `stride`, never `width * 4`.

### Frame pacing and the display refresh rate

WebKit runs `requestAnimationFrame` and the page's main-thread rendering updates from a display link per screen,
created at the first page load in the process (`DisplayLink.cpp:256-261`). How it ticks (WPE WebKit 2.54.1):

- Without a `WPEScreenSyncObserver` on the screen WebKit uses `DisplayVBlankMonitorTimer`, which ignores the screen
  completely: it sleeps `1000 / 60` ms per tick (`DisplayVBlankMonitor.cpp:54-61`,
  `DisplayVBlankMonitorTimer.cpp:40-48`, `WebCore::FullSpeedFramesPerSecond = 60`, `AnimationFrameRate.h:56`), about 61
  Hz. Reporting a different `WPEScreen:refresh-rate` alone changes nothing.
- With an observer, `DisplayVBlankMonitorWPE` ticks on the observer's syncs (called from a thread of the observer,
  `WPEScreenSyncObserver.cpp:98`; the display link handler must not run on the main thread, `DisplayLink.cpp:187`)
  and takes `wpe_screen_get_refresh_rate() / 1000` (integer Hz, so 49.999 Hz counts as 49) as the nominal frame
  rate once, when the display link is created (`DisplayVBlankMonitorWPE.cpp:36-50`, `DisplayLinkGLib.cpp:47-48`). The
  web process derives its preferred update rate from that nominal value (`Page.cpp:2773`,
  `AnimationFrameRate.cpp:47-86`); later screen changes do not reach it (`DisplayLink::displayPropertiesChanged()` is a
  FIXME, `DisplayLink.cpp:161-164`).
- The base `WPEScreen` class does provide a DRM vblank observer when WebKit is built with libdrm, and 0.1.0's screen
  inherited it, but it looks for a connected connector whose size in mm equals the screen's physical size
  (`WPEScreen.cpp:173-249, 375-377`). The virtual screen is sized after the view at 160 dpi (76x127 mm for 480x800,
  where a handheld's 480x800 panel reports about 44x75 mm), so it never matched, and every attempt opened all
  DRM primary nodes. 0.1.0 therefore ran WebKit's display link on the ~61 Hz timer with a nominal rate of 60.
- What that meant on a 50 Hz panel is less than the numbers suggest. Frames are back-pressured: WebKit composites
  the next frame only after `uc_webview_frame_rendered()` (`AcceleratedBackingStore.cpp:301-310` ->
  `ThreadedCompositor::frameComplete()`, `ThreadedCompositor.cpp:676-700`), and a tick that arrives while the
  previous update still waits for the compositor is kept and served right after that composite
  (`LayerTreeHost.cpp:141-153, 438-458`). Accelerated CSS animations (transform, opacity) do not use the display link
  at all: the compositor composites again after every frame while they run (`ThreadedCompositor.cpp:354, 412-413`).
  So while a page draws, a host that reports frames as it presents them (remote-ui reports after Qt's page flip,
  and Qt waits for the previous flip, `qeglfscontext.cpp:114`) paces it at the panel rate; the faster ticks only
  wake it up. The timer's rate showed in `requestAnimationFrame` loops that do not change the page (no frame, no
  back-pressure: ~61 callbacks per second), in WebKit's nominal frame rate of 60 (preferred update interval
  16.7 ms) and in hosts that report frames at once (`ucwebview-test` rendered ~61 frames per second).

Since 0.2.0 `UcWpeScreen` overrides `get_sync_observer` with `UcWpeSyncObserver` (`uc_wpe_screen.c`): a thread of its
own (`uc_vsync.c`) ticks with absolute `CLOCK_MONOTONIC` deadlines (ns arithmetic, no drift, a stall drops the
missed ticks instead of bursting them) and runs only while WebKit's display link is started (the observer's
`start`/`stop` follow WebKit's first and last callback, `WPEScreenSyncObserver.cpp:121-124, 149-152`; the display link
stops 20 ticks after the last page observer went, `DisplayLink.cpp:42, 218-224`). The screen reports the configured
refresh rate; the ticks run **1 % faster** (`uc_refresh_tick_mhz()`): a display link slower than the panel would now
and then leave a refresh without a new frame (a visible stutter), while the surplus tick of a slightly faster one
is absorbed by the back-pressure above. The 1 % covers a host that rounds (Qt reports the integer DRM `vrefresh`,
at most 0.5 Hz off). Who owns the pointer `get_sync_observer` returns depends on the WebKit release, although
`WPEScreen.h` documents transfer none: up to 2.54.1 `DisplayVBlankMonitorWPE` adopts it
(`DisplayVBlankMonitorWPE.cpp:44`), so the vfunc returns a reference of its own; 2.54.2 takes a reference itself
("Fix screen sync observer leak in wpe_screen_get_sync_observer()"), and an extra one would never be dropped. Since
0.6.4 the vfunc asks `wpe_platform_get_major/minor/micro_version()` at run time and adds the reference only below
2.54.2, so a library copied onto a firmware with the other release neither leaks nor over-releases.

Why self-clocked and not driven by the host's frame reports: `uc_webview_frame_rendered()` only arrives while frames
flow, but the display link must tick before the first frame of an animation exists, and Qt 5.15 emits
`frameSwapped` when the page flip is queued, not when it happens (`qeglfscontext.cpp:105-117`: wait for the previous
flip, `eglSwapBuffers`, queue the flip), so its timing carries no vblank phase. The back-pressure above already locks
drawing pages to the presentation.

Rate precedence (`uc_refresh.c`, unit-tested):

1. `UC_WEBVIEW_REFRESH_RATE=<Hz>` in the environment, read once by `uc_webview_global_init()` and logged in its
   start lines. A plain decimal number of Hz (`50`, `49.999`, `59.6`) in 20..240; anything else is logged and ignored.
   For a panel whose rate the host reports wrongly and for experiments.
2. The host's value, `uc_webview_global_set_refresh_rate(hz)` (any time; before `uc_webview_global_init()` it is
   kept). remote-ui passes `QScreen::refreshRate()` of the view's window at view creation and on screen and rate
   changes. Qt's eglfs_kms reports the DRM mode's integer `vrefresh` (`qeglfskmsscreen.cpp:212-219`, the kernel
   rounds `clock / (htotal * vtotal)`).
3. 60 Hz otherwise (`ucwebview-test` without `--refresh-rate`, other hosts).

Call the setter before the first page load: WebKit takes the nominal rate when it creates the display link; later
calls change the tick rate at once but not WebKit's nominal value.

| Device | Panel mode | Panel rate | Qt reports | Display link ticks |
|--------|----------------------------|-----------|------------|--------------------|
| Remote 3 | 22358 kHz, htotal 548, vtotal 816 | 49.999 Hz (WPE's DRM platform logs `refresh 49999 mHz`) | 50 | 50.5 Hz |
| Remote Two | 32400 kHz, htotal 1003, vtotal 542 | 59.600 Hz | 60 | 60.6 Hz |

Qt's rounded value is above the panel's rate on both models, so neither needs `UC_WEBVIEW_REFRESH_RATE`: an exact
59.6 for the Remote Two would only move WebKit's nominal rate from 60 to 59 and the idle ticks from 60.6 to 60.2 Hz.
The variable stays the override for a panel whose mode the host misreports and for experiments.

`UC_WEBVIEW_TRACE=1` (the variable remote-ui uses for its input trace) makes the library log the vsync thread's start
and stop and, every 10 s while it runs, `vsync 50.500 Hz: 505 ticks in 10.0 s = 50.50 Hz, latest wake-up 0.12 ms after
the deadline`. The kiosk launcher ([../launcher](../launcher/README.md)) is not affected: WPEPlatform's DRM screen paces
it on the real vblank (`WPEScreenDRM.cpp:66-82, 126-138`).

### Rendered signal (library 0.5.0)

`int uc_webview_when_rendered(uc_webview *, uc_webview_rendered_cb cb, void *user)` calls `cb(wv, status, user)`
exactly once when the page has finished a whole rendering update that started after the call and a frame has reached
the host since the call. remote-ui shows a busy indicator with it while a zoom step re-renders a large page: WebKit
does the relayout in the web process, the UI thread stays free.

| Status | When |
|--------|------|
| `UC_WEBVIEW_RENDERED_DONE` (0) | the rendering update ran and a frame came: at once when a frame had already reached the host since the call, else at the next frame, at most 100 ms after the update (an update that changes nothing visible produces no frame) |
| `UC_WEBVIEW_RENDERED_TIMEOUT` (1) | 60 s after the call without the update (15 s up to 0.5.x): the view is unmapped (a request completes when it is mapped again in time), the page keeps the web process busy |
| `UC_WEBVIEW_RENDERED_CANCELLED` (2) | a navigation commits (before `load_changed(COMMITTED)`), the web process terminates (before `web_process_terminated`), the script did not run (before the view's first load, after its web process terminated until the next load, no web process extension), or the view is freed (inside `uc_webview_free()`) |

Return value 0: the request is pending; -1: refused (invalid view, `cb` NULL, another thread, from inside
`uc_webview_free()`, 32 requests of the view pending), `cb` is never called then.

How it works (WebKit 2.54.1):

1. `webkit_web_view_set_zoom_level()` sends `DidSetPageZoomFactor` at once (`WebPageProxy.cpp:6719-6734`), and the
   web process rebuilds the style and lays the page out synchronously in that message
   (`WebPage.cpp:2821-2836`, `LocalFrame.cpp:1085-1130`). The paint follows in the next rendering update. Every
   queued zoom message does the whole style rebuild and layout again: WebKit does not coalesce them; remote-ui
   does (its `docs/webview.md`).
2. The request sends a probe script to the library's web process extension (`ucwebview.host-script.1`,
   `webkit_web_view_send_message_to_page()`), which runs it with JavaScriptCore in the library's own script world
   `ucwebview-host` of the main frame (since 0.9.0; see "Device state for web pages" for the world). The message
   travels over the same connection to the same `WebPage` as the zoom message (`WebKitWebView.cpp:5598-5627`), and the
   extension runs the probe inside it (`WebPageGLib.cpp:99-123`), so it runs after the zoom's layout.
3. The probe waits for two `requestAnimationFrame` callbacks. The first
   runs at the start of the next rendering update, before its layout and paint (`Page.cpp:2329-2337`). An update
   that changed the scene makes the next one wait for the compositor's `didComposite`
   (`LayerTreeHost.cpp:141-153, 438-457`), which the compositor posts before it swaps and sends the frame
   (`ThreadedCompositor.cpp:585-607`). So the second callback means painted and composited. Callbacks of an isolated
   world run like the page's (`ScriptedAnimationController.cpp:177-190`).
4. The second callback posts the request id to the script message handler `ucwebviewRendered`, which the library
   registers for its world only. WebKit looks handlers up per world (`UserMessageHandlersNamespace.cpp:81-101`), and
   the page's world has no `window.webkit` for it (`LocalDOMWindow.cpp:862-888`): page scripts can neither see the
   probe nor fake its report. The handler's world and the extension's are one (`WebUserContentController.cpp:139-158`).
5. The frame of that update reaches the host only after its GPU fence signalled (`AcceleratedBackingStore.cpp:194-210`),
   so it can come after the report; hence the frame rule in the table. On a page that animates, a frame already on
   its way can count, and DONE may come one frame early.

The timeout is 60 s since 0.6.0 (15 s before). Measured on a Remote 3 (`ucwebview-test --zoom-test`, median per zoom
step): the internal long page 3.2 s in the normal power mode, 11 to 15 s in the low power mode (408 MHz, which the
remote enters about 10 s after the last input, so a long render finishes there); a long Wikipedia article 4.3 s, and
every step over 15 s at 408 MHz. A host treats `TIMEOUT` as "show the
next frame as it comes" (remote-ui does).

Design choices:
- No user gesture (0.9.0). Up to 0.8.x the probe ran with `webkit_web_view_evaluate_javascript()`, and WebKit forces a
  user gesture on every script of its UI process API (`WebKitWebView.cpp:4494-4502`, `ScriptController.cpp:654-663`):
  the transient activation ended with the probe (it first hopped through a `MessageChannel` task, because
  `requestAnimationFrame` keeps the gesture of the script that registers it, `ScriptedAnimationController.cpp:120,
  178-195`), but the sticky and the history-action activation stayed (`LocalDOMWindow.h:145-150`), and so did
  `Page::userDidInteractWithPage`, which arms the page's "leave this page?" question. A host that waits for every
  loaded page (remote-ui) gave every page user activation that way. JavaScriptCore knows no user gesture, and the hop
  is gone. `ucwebview-test --device-test` checks it (`rendered` steps: `act:0`, no question; the old probe: `act:1`
  and the question).
- The probe returns at once, and the extension replies at once. A reply that waited for the callbacks would keep the
  message's `GTask`, and with it a reference on the `WebKitWebView` (`WebKitWebView.cpp:5611`), until they come: in
  a document that was navigated away from they never come, so a freed view could outlive `uc_webview_free()` with its
  web process.
- No message to a view before its first load or after its web process terminated until the next load
  (`uc_page_wk.c`): WebKit would start a web process for about:blank (`WebPageProxy::ensureRunningProcess()`). Such
  a request ends `CANCELLED` from the main context. A request after a termination ended so before as well
  (WebKit's script API refuses it, `WebPageProxy.cpp:7541-7545`); one before the first load started a web process
  for about:blank up to 0.8.x.
- Navigation cancels at `COMMITTED`, not `STARTED`: until the commit the old document is still on screen and its
  update still completes; same-document navigations send no load events and cancel nothing.
- The library logs no error text of a failed probe: `rendered probe 3 did not run (g-io-error-quark 45)` when no
  page was reachable, `(WebKitUserMessageError 0)` without the extension, `(exception 0)` when the probe threw.

`UC_WEBVIEW_TRACE=1` logs one line per result (see "What the logs carry"): `rendered after 2840 ms (done)`, with
`rendering update after N ms` when the update and the end differ (the frame came later, no frame came, a timeout or
cancellation after the update). The request queue is `uc_rendered.c` (GLib only, `tests/test-rendered.c`), the
WebKit side is in `uc_webview.c`. `ucwebview-test --zoom-test` measures zoom steps without a UI (below).

### Pinch zoom (library 0.6.0)

WebKit's WPE port has no zoom gesture: its gesture controller knows tap and drag only (`WPEGestureController.h:44-49`),
GTK's magnification gesture and its GPU "transient zoom" are compiled for GTK only. Redrawing the page on every finger
move would cost a style rebuild and repaint per step (`Page.cpp:1684-1741`), seconds on a large page. So the library
recognises the pinch, the host scales its last frame on the GPU while the fingers move, and at the end the library
sets WebKit's page scale once.

**Recognition.** A host that sets the `pinch` callback (`uc_webview_new_sized()`) gets a `UcGestureController`
(`uc_wpe_gesture.c`) as the view's `WPEGestureController`. It wraps WebKit's default controller, which
`wpe_view_get_gesture_controller()` creates and `wpe_view_set_gesture_controller()` replaces (`WPEView.cpp:1165-1188`),
and runs the pinch tracker `uc_pinch.c` next to it:

- WebKit hands the controller only the touch events the page did not handle, after the web process answered them, and
  cancels it when a listener called `preventDefault()` (`PageClientImpl.cpp:248-266`, `WebPageProxy.cpp:5455-5531`).
  A page with a pinch of its own (a map) keeps it; the page gets its DOM touch events with both fingers as before.
- One finger: the event goes to WebKit's controller (tap → click, drag → scroll, `WPEWebViewPlatform.cpp:433-497`).
- A second finger: WebKit's controller is cancelled (no click at the first finger's release, its scroll stops) and
  gets nothing until every finger is up. Two fingers neither tap nor scroll.
- `BEGIN` when the finger distance changed by more than the drag threshold (16 px) since the second press, scale 1 at
  the midpoint; the scale counts from that distance, so it starts without a jump (like Android's
  `ScaleGestureDetector`). `UPDATE` per move of one of the two fingers: scale = distance / distance at `BEGIN`, the
  midpoint. `END` when one of them lifts, with the values of the last `UPDATE`. `CANCEL`: a touch cancel, the page took
  the touches over, a navigation committed, the web process died.
- A two-finger pan or tap reports nothing; a third finger is ignored; a press of a finger whose release was lost
  starts afresh.

**Magnification.** `uc_webview_set_magnification(scale, x, y)` sets WebKit's page scale
(`webkit_web_view_set_magnification()`, new in 2.54) and keeps the content at the focal point in place. WebKit's call
alone keeps no point fixed, although its documentation says "around the center of the view": it sends the view centre
c, and the web process scrolls to `r * (s - c)` (`WebPage.cpp:2960-2970`; `rootViewToContents(-c) = s - c`); keeping c
would be `r * (s + c) - c`. On WebKitGTK 2.54.0, which shares that code, magnification 2 at scroll (500, 1000) in a
480x800 view landed at (260, 600) instead of (620, 1200). `WebPageProxy::scalePage()` with an origin is not public, so
the library sets the scroll position itself with two scripts in its own script world (`uc_magnify.c`): one before the
scale message records `visualViewport.scale`, `.pageLeft`, `.pageTop`, one after it scrolls with
`window.scrollTo({behavior: 'instant'})` so that the content point under the focal point stays. The three go over one
connection to the same `WebPage` and run in order (`WebPageProxy.cpp:6792-6808, 7533-7560`), and so does a
`uc_webview_when_rendered()` request right after. Precision: `scrollTo()` takes whole CSS pixels
(`LocalDOMWindow.cpp:1833-1863`), so the point stays within half a CSS pixel; near the page's edges WebKit clamps the
scroll position and the content moves. Checked on WebKitGTK 2.54.0 with the library's own scripts: the focal point
stayed at every step from 1 to 2, 2 to 3.5, back to 1 and with a page zoom of 1.5, and at the page's corner.

- Range 0.25 to 10 (`UC_WEBVIEW_MAGNIFICATION_MIN/MAX`); hosts keep their own limits inside (remote-ui 1 to 4).
- The layout width stays the view's (`ScrollView.cpp:367-370`), so line breaks stay; `innerWidth` and
  `visualViewport` report the smaller visible part. WebKit rebuilds the style and repaints at the new scale; touches
  and clicks keep working in view pixels.
- Page zoom (`uc_webview_set_zoom()`, a new layout) and magnification are separate WebKit factors and multiply.
- Navigation, as WebKit does it (checked on WebKitGTK 2.54.0): a new page starts at 1.0 (`WebPage.cpp:8233-8250`;
  the UI process resets its copy silently at the commit, `WebPageProxy.cpp:8824-8829`); back, forward, reload and
  same-document history steps restore the magnification the page had when it was left, with its scroll position
  (`HistoryController.cpp:133, 192-193`), and tell the UI process (`WebLocalFrameLoaderClient.cpp:1594-1600`). The
  library reports each change it did not make itself as `magnification_changed(value)`, from the main context, and
  `uc_webview_get_magnification()` returns WebKit's value.
- The two scripts run through the library's web process extension like the rendered probe (since 0.9.0): no user
  gesture for the page (up to 0.8.x they were one, like every script of WebKit's API, see "Rendered signal"). Before
  the view's first load and while its web process is gone only WebKit's scale changes.

`ucwebview-test --pinch` drives the whole path like a host; `--magnify` the magnification alone (see below).

**Touch panel.** A pinch needs a touch panel whose kernel driver reports two fingers; with a single touch slot the
host never gets a second finger and there are no pinch reports (a host can zoom with keys instead, remote-ui uses the
volume keys). `ucwebview-test --pinch` synthesizes both fingers inside the library, so it does not test the panel.

### Buffer formats and the DRM device

`get_preferred_buffer_formats` advertises ARGB8888 and XRGB8888 with the modifiers `eglQueryDmaBufModifiersEXT`
reports for the host display (one RENDERING group on the render device; `DRM_FORMAT_MOD_INVALID` is kept only when
no explicit modifier exists, LINEAR when nothing is reported, both formats with LINEAR when EGL reports neither).
The web process intersects the list with its own and allocates with `gbm_bo_create_with_modifiers2()`
(`AcceleratedSurface.cpp:270-298`). `WPE_BUFFER_FORMAT=XR24:0` still overrides everything (`WPEToplevel.cpp:622-645`).

`get_drm_device`: `eglQueryDisplayAttribEXT(EGL_DEVICE_EXT)` + `eglQueryDeviceStringEXT(EGL_DRM_DEVICE_FILE_EXT /
EGL_DRM_RENDER_NODE_FILE_EXT)` like the upstream Qt 6 binding (`WPEDisplayQtQuick.cpp:67-95`; on a Rockchip RK3566 Qt's
GBM display on the KMS node `card1` resolves to the Panfrost device `card0`/`renderD128`), fallback
`/dev/dri/renderD128` + `/dev/dri/card0` if they exist, else no DRM device: the web process then uses a surfaceless
display and shared memory (`AcceleratedSurface.cpp:594-607`). `WPE_DRM_DEVICE` overrides (`WPEDisplay.cpp:699-706`).

### Keyboard without XKB data

WebKit asks the display for a `WPEKeymap` to read the modifier state (`WebPageProxyWPE.cpp:206-208`). Without a
`get_keymap` vfunc WPEDisplay creates `wpe_keymap_xkb_new()` (`WPEDisplay.cpp:449-461`), which compiles
`evdev/pc105/us` from `/usr/share/X11/xkb` (`WPEKeymapXKB.cpp:175-185`) and crashes in `xkb_state_new()` when the
data is missing. `libucwebview` therefore provides its own `WPEKeymap` subclass whose
`get_modifiers()` answers from the key events the host sent (`uc_events.c` tracks Shift/Control/Alt/Meta/Caps Lock);
`translate_keyboard_state`/`get_entries_for_keyval` return FALSE (only WebDriver uses them). **No XKB data
(xkeyboard-config) is needed**; the host passes XKB keysyms in `uc_webview_key()` and WebKit derives
`key`, text and Windows key codes from the keysym and `code` from the hardware keycode (`WebKeyboardEventWPE.cpp`,
see [Key names](#key-names-dom-key-code-and-keycode)).

### Key names (DOM `key`, `code` and `keyCode`)

`uc_webview_key()` passes keysym and keycode to `wpe_event_keyboard_new()` unchanged: no keymap lookup, and `UcWpeIme`
filters no keys, so they reach the page also while a text field has the focus. WebKit builds the DOM event in
`WebEventFactoryWPE.cpp:224-237`: `key` and the legacy `keyCode` from the keysym (`keyValueStringForWPEKeyval`,
`windowsKeyCodeForWPEKeyval` in `WebKeyboardEventWPE.cpp`), `code` from the keycode, which is the evdev code + 8
(`keyCodeStringForWPEKeycode`). What a host passes for the standard key names of a remote (checked against the
2.54.2 tables, with and without the patch, by compiling them into a host test; `mods` 0):

| DOM `key`          | keysym                                   | keycode (evdev + 8)         | `code`              | `keyCode`   | patch |
|--------------------|------------------------------------------|-----------------------------|---------------------|-------------|-------|
| ArrowUp/Down/Left/Right | `Up` 0xff52, `Down` 0xff54, `Left` 0xff51, `Right` 0xff53 | 111, 116, 113, 114 | same as `key` | 38, 40, 37, 39 | |
| Enter              | `Return` 0xff0d                          | 36 (KEY_ENTER)              | Enter               | 13          |       |
| Escape             | `Escape` 0xff1b                          | 9 (KEY_ESC)                 | Escape              | 27          |       |
| Backspace          | `BackSpace` 0xff08                       | 22 (KEY_BACKSPACE)          | Backspace           | 8           |       |
| Tab                | `Tab` 0xff09                             | 23 (KEY_TAB)                | Tab                 | 9           |       |
| PageUp, PageDown   | `Page_Up` 0xff55, `Page_Down` 0xff56     | 112, 117                    | same as `key`       | 33, 34      |       |
| Home, End          | `Home` 0xff50, `End` 0xff57              | 110, 115                    | same as `key`       | 36, 35      |       |
| ContextMenu        | `Menu` 0xff67                            | 135 (KEY_COMPOSE)           | ContextMenu         | 93          |       |
| BrowserBack        | `XF86Back` 0x1008ff26                    | 166 (KEY_BACK)              | BrowserBack         | 166         |       |
| BrowserForward     | `XF86Forward` 0x1008ff27                 | 167 (KEY_FORWARD)           | BrowserForward      | 167         |       |
| BrowserRefresh     | `XF86Refresh` 0x1008ff29                 | 181 (KEY_REFRESH)           | BrowserRefresh      | 168         |       |
| BrowserStop        | `XF86Stop` 0x1008ff28                    | 136 (KEY_STOP)              | BrowserStop         | 169         |       |
| Exit               | `XF86Close` 0x1008ff56                   | **182** (KEY_EXIT)          | Unidentified        | 0           | yes   |
| AudioVolumeUp      | `XF86AudioRaiseVolume` 0x1008ff13        | 123 (KEY_VOLUMEUP)          | AudioVolumeUp       | 175         |       |
| AudioVolumeDown    | `XF86AudioLowerVolume` 0x1008ff11        | 122 (KEY_VOLUMEDOWN)        | AudioVolumeDown     | 174         |       |
| AudioVolumeMute    | `XF86AudioMute` 0x1008ff12               | 121 (KEY_MUTE)              | AudioVolumeMute     | 173         |       |
| ChannelUp          | `XF86ChannelUp` 0x10081192               | 410 (KEY_CHANNELUP)         | Unidentified        | 427 (0x1AB) | yes   |
| ChannelDown        | `XF86ChannelDown` 0x10081193             | 411 (KEY_CHANNELDOWN)       | Unidentified        | 428 (0x1AC) | yes   |
| MediaPlayPause     | `XF86AudioPlay` 0x1008ff14               | **172** (KEY_PLAYPAUSE)     | MediaPlayPause      | 179 (0xB3)  | yes   |
| MediaPlay          | `XF86AudioPlay` 0x1008ff14               | 215 (KEY_PLAY)              | Unidentified        | 250         |       |
| MediaPause         | `XF86AudioPause` 0x1008ff31              | 209 (KEY_PAUSECD)           | Unidentified        | 19          |       |
| MediaStop          | `XF86AudioStop` 0x1008ff15               | 174 (KEY_STOPCD)            | MediaStop           | 178         |       |
| MediaRecord        | `XF86AudioRecord` 0x1008ff1c             | 175 (KEY_RECORD)            | Unidentified        | 416 (0x1A0) |       |
| MediaRewind        | `XF86AudioRewind` 0x1008ff3e             | 176 (KEY_REWIND)            | Unidentified        | 227         |       |
| MediaFastForward   | `XF86AudioForward` 0x1008ff97            | 216 (KEY_FASTFORWARD)       | Unidentified        | 228         |       |
| MediaTrackPrevious | `XF86AudioPrev` 0x1008ff16               | 173 (KEY_PREVIOUSSONG)      | MediaTrackPrevious  | 177         |       |
| MediaTrackNext     | `XF86AudioNext` 0x1008ff17               | 171 (KEY_NEXTSONG)          | MediaTrackNext      | 176         |       |
| ColorF0Red ... ColorF3Blue | `XF86Red` 0x1008ffa3, `XF86Green` 0x1008ffa4, `XF86Yellow` 0x1008ffa5, `XF86Blue` 0x1008ffa6 | 406-409 (KEY_RED..KEY_BLUE) | Unidentified | 403-406 (0x193-0x196) | |
| Shift+Tab          | `Tab` 0xff09, `mods` `UC_WEBVIEW_MOD_SHIFT` | 23 (KEY_TAB)             | Tab                 | 9           |       |

- `code` "Unidentified": the UI Events code specification has no value for these keys (Chromium on Linux gives ""
  for Exit, the channel and colour keys and its own MediaPlay, MediaPause, MediaRecord, MediaRewind and
  MediaFastForward).
- **patch**: WebKit 2.54.2 has no key value for these; the optional WebKit patch
  [`0001-WPE-Key-values-for-ChannelUp-ChannelDown-MediaPlayPause-and-Exit.patch`](../webkit-patches/README.md) adds
  them. Without it Exit arrives as "Close", the
  channel keys as "Unidentified" with `keyCode` 0, MediaPlayPause as "MediaPlay" with `keyCode` 250.
  xkeyboard-config gives KEY_PLAYPAUSE and KEY_EXIT the keysyms of KEY_PLAY and KEY_CLOSE and there are no keysyms
  of their own, so the patch tells them apart by the keycode: MediaPlayPause and Exit need exactly the keycodes in
  bold, with any other keycode (or 0) the same keysyms give MediaPlay and Close. The patch also maps `ISO_Left_Tab`
  to "Tab" and `XF86Reload` to "BrowserRefresh", the keysyms an XKB keymap (the kiosk launcher) produces for
  Shift+Tab and KEY_REFRESH.
- Shift+Tab needs no Shift key event: the library puts `mods` on the event, `shiftKey` is true and WebKit moves the
  focus backwards (`EventHandler.cpp:4569-4570, 5344-5365`). Do not use `ISO_Left_Tab` without the patch: the focus
  still moves, but `key` is "Unidentified".
- No WebKit default action for the browser, media, channel, colour and Exit keys: the page handles them. Enter, Tab,
  Backspace and Escape carry text, so WebKit also sends a `keypress` after an unhandled keydown
  (`EventHandler.cpp:4363-4379`). A held key is repeated by more key-downs without key-up; WebKit sets `event.repeat`
  when the keycode equals the previous key-down's (`KeyAutoRepeatHandler`).
- The input trace names only the keys of `uc_trace_key_name()`; the media, browser, channel and colour keys appear
  as `key printable` (see [What the logs carry](#what-the-logs-carry)).

### Text input (IME)

`create_input_method_context` returns a `UcWpeIme : WPEInputMethodContext`. WebKit
(`WebKitInputMethodContextImplWPE.cpp`) sets purpose/hints on it, calls `focus_in` when an editable element is focused
while the view has focus (`InputMethodFilter.cpp:91-102`), `set_cursor_area`, `set_surrounding`, `focus_out`, `reset`.
The library forwards these as `ime_focus(1, hints, purpose, surrounding, cursor)` (repeated when hints/purpose change
while focused), `ime_cursor_area`, `ime_surrounding`, `ime_focus(0, ...)`. `uc_webview_ime_commit()` emits
`committed(text)`; `uc_webview_ime_preedit(text, cursor)` emits `preedit-started` + `preedit-changed` (then
`preedit-changed` on updates, and `preedit-changed("")` + `preedit-finished` when ended with `""`);
`uc_webview_ime_delete_surrounding()` emits `delete-surrounding(offset, count)`. A commit or delete during a pre-edit
ends the pre-edit first. The state machine is `uc_ime_state.c`, unit-tested on the host.

**A focused element whose web process is gone (0.7.5).** WebKit 2.54 keeps the input method state the web process sent
last in the UI process (`InputMethodFilter`) and changes it only on the web process' messages, which come only when the
web process' own state changes (`WebPageGLib.cpp:215-227`). When a navigation to another site moves the page to another
web process (process swap) or the web process terminates, nothing resets that state (`PageClientImpl::processDidExit()`
is empty for WPE) and the new web process starts without one. Up to 0.7.4 the host therefore got no `ime_focus(0)` for
the element of the old page, every focus of the view replayed `ime_focus(1)` for it (`WPEWebViewPlatform.cpp:104-114`),
and a field the new page focused brought no report when its state equalled the gone one. A host then opened its keyboard
at every tap on any page after a page that had focused a field itself, and when its view got the focus back after a
dialog. Since 0.7.5:

- At the swap (`notify::page-id` of the `WebKitWebView`, emitted only by `WebPageProxy::swapToProvisionalPage()`,
  right before the commit's `load_changed(COMMITTED)`) and before `web_process_terminated()` the library reports
  `ime_focus(0)` for a focused element; commits, pre-edits and deletions are ignored from then on.
- Until the new document has a focused element, the view's focus changes (`uc_webview_set_focused()`, WebKit's own
  focus at a tap) bring no `ime_focus(1)` for the gone element.
- The new document's first focused element is reported as `ime_focus(1)` with its hints and purpose, also when its
  state equals the gone one: the library sets a hint bit that no state has (bit 30) on WebKit's context and its own, so
  WebKit's next state always changes the hints. The report comes from an idle (`G_PRIORITY_HIGH`) after WebKit's
  update, because GLib delivers the hints change before the purpose change, and only while the view has the focus
  (else WebKit's replay reports it when the view gets the focus). The marker bit never reaches the host.
- Same-site navigations, focus moves within a page, the replay of an element that still has the focus, `inputmode`
  and `INHIBIT_OSK`, pre-edit, commit and delete-surrounding behave as before. The text around the caret of the new
  element arrives with the next `ime_surrounding` that WebKit sends; WebKit sends none when it equals the gone
  element's (`InputMethodFilter.cpp:244-245`), then the host keeps none until the next edit.

`ucwebview-test --ime-swap-test` checks this (see below).

### URI scheme allowlist

`uc_webview_load_uri()` and the base URI of `uc_webview_load_html()` go through an allowlist (`uc_uri_policy.c`,
unit-tested on the host): `http`, `https`, `about` and `data` are always allowed, `file` only when the process sets
`UC_WEBVIEW_ALLOW_FILE_URLS=1` (also `true`/`yes`/`on`) before `uc_webview_global_init()`, which reads it once and
logs the result in its start line (`..., file: URLs allowed (UC_WEBVIEW_ALLOW_FILE_URLS)` or `file: URLs blocked`).
**Production default: file: URLs are blocked.** Every other scheme (`javascript:`, `blob:`, `ftp:`, custom ones) and
strings without a scheme (a bare path, `example.com`; `localhost:8080` parses as the scheme `localhost`) are refused
too. A refused load logs `ucwebview: load_uri <uri> rejected: <reason>` with `g_warning` (the URI as described in
"What the logs carry") and
reports `load_changed(UC_WEBVIEW_LOAD_FAILED, uri)` from an idle (callbacks never run inside a `uc_webview_*` call),
without STARTED/FINISHED, since 0.10.0 right after `load_failed_detail(uri, UC_WEBVIEW_FAIL_SCHEME_REFUSED, "", 0, 0,
"")`; the current page and a running load stay as they are. A `load_html()` base URI of `NULL`
or `""` is `about:blank` and allowed.

The API is the only way to a `file:` document: WebCore refuses page-initiated loads of local URLs from http(s), data
and about documents (`FrameLoader::loadFrameRequest`, `FrameLoader.cpp:1513-1516`, `SecurityOrigin::canDisplay`
with `restrictAccessToLocal`), while API loads (`FrameLoader::load(FrameLoadRequest&&)`) are not checked there, and
WPE grants every API `file:` load universal read access because sandbox extensions are compiled out
(`WebKitFeatures.cmake:278`, `SandboxExtension.h:165`, `WebPageProxy.cpp:2138-2146`). The network process reads
`file:` URLs with GIO in the host's mount namespace (`NetworkDataTaskSoup.cpp:139-141, 1435-1478`; the helpers are
plain children of the host process, so a systemd sandbox's bind mounts apply to them as well).

MIME types of `file:` documents come from the file name (WebKit's bundled xdgmime, `MIMETypeRegistryXdg.cpp:37-50`) or
GIO's content type (`NetworkDataTaskSoup.cpp:1466-1475`). Both need the shared-mime-info database, which a minimal
system image may not install and a sandboxed host may not see: without it an `.html` file is then typed `text/plain` by
GIO's text sniffing (checked on a host with `XDG_DATA_DIRS` pointing to an empty directory) and shows as source text.
Showing `file:` pages needs shared-mime-info (GPL-2.0-or-later data) in the host's mount namespace (a systemd sandbox:
`BindReadOnlyPaths=/usr/share/mime`).

### Load failures (0.10.0)

`load_failed_detail(uri, reason, error_domain, error_code, tls_errors, message)` tells the host why a main-frame load
failed, so that it can say so ("the host name does not resolve", "no network", ...) instead of a generic text. It comes
right before `load_changed(UC_WEBVIEW_LOAD_FAILED)` of the same failure, with the same `uri`: exactly one detail and one
FAILED per failure, and no detail without a FAILED. The loads that bring no FAILED bring no detail either: a load
replaced by a newer one (`WEBKIT_NETWORK_ERROR_CANCELLED`), a response the library refuses (`response_refused`,
"Requests (0.7.0)"), a certificate an exception accepts (the library loads again), an https load cancelled with the
connection of WebKit's preconnect after that had failed on the certificate (0.10.3, the library loads again, see below),
HTTP error pages (they load as pages). A host of an older header (`uc_webview_new_sized()` with a struct up to
`response_refused`, or `uc_webview_new()`) gets FAILED alone, as before; an older library never calls the callback, so
"no detail before FAILED" means "reason unknown".

The reason comes from the domain and code of WebKit's `GError` (`uc_load_error.c`, unit-tested in
`tests/test-load-error.c`), never from the message text; the strings are as WebKit passed them (`error_domain` is
`g_quark_to_string()` of the domain, `""` when there is none; `message` may be `""`):

| Reason | `error_domain` and `error_code` | Example |
|--------|---------------------------------|---------|
| `UC_WEBVIEW_FAIL_HOST_NOT_FOUND` (1) | `g-resolver-error-quark` `G_RESOLVER_ERROR_NOT_FOUND` (0) or `TEMPORARY_FAILURE` (1); `g-io-error-quark` `G_IO_ERROR_HOST_NOT_FOUND` (28) | `Error resolving “www.example.com”: Temporary failure in name resolution` (also what the resolver says without any network) |
| `UC_WEBVIEW_FAIL_NO_NETWORK` (2) | `g-io-error-quark` `G_IO_ERROR_NETWORK_UNREACHABLE` (38), `G_IO_ERROR_HOST_UNREACHABLE` (37) | `Could not connect to 255.255.255.255: Network is unreachable` |
| `UC_WEBVIEW_FAIL_CONNECTION_REFUSED` (3) | `g-io-error-quark` `G_IO_ERROR_CONNECTION_REFUSED` (39) | `Could not connect to 127.0.0.1: Connection refused` |
| `UC_WEBVIEW_FAIL_TIMEOUT` (4) | `g-io-error-quark` `G_IO_ERROR_TIMED_OUT` (24) or `G_IO_ERROR_CANCELLED` (19); `WebKitNetworkError` -1001 | `Operation was cancelled` (a host that never answers, after 10 s, see below) |
| `UC_WEBVIEW_FAIL_CERTIFICATE` (5) | `g-tls-error-quark` (WebKit: `G_TLS_ERROR_BAD_CERTIFICATE`, 2), `tls_errors` the `GTlsCertificateFlags` | `Unacceptable TLS certificate`, `tls_errors` 0x1 (`G_TLS_CERTIFICATE_UNKNOWN_CA`) for a self-signed certificate |
| `UC_WEBVIEW_FAIL_SCHEME_REFUSED` (6) | `""`, 0, message `""` | the scheme allowlist refused the URI ("URI scheme allowlist") |
| `UC_WEBVIEW_FAIL_OTHER` (0) | anything else (HTTP/2 and proxy errors, `SoupSessionError`, `WebKitPolicyError` such as a restricted port) | |

Facts of WebKit 2.54 this rests on:
- The UI process builds the `GError` from the network process' `ResourceError` with the error's domain string and
  code (`WebKitNavigationClient.cpp:62-73, 85-92`; `toWebKitError()` passes codes it does not know unchanged,
  `WebKitPrivate.cpp:135-184`), and libsoup's and GIO's errors keep theirs (`ResourceErrorSoup.cpp:83-99`).
- A certificate WebKit does not accept (TLS errors policy FAIL, the library's default) is `G_TLS_ERROR_BAD_CERTIFICATE`
  with the flags (`ResourceErrorSoup.cpp:101-107`). WebKit emits `load-failed-with-tls-errors` first and
  `load-failed`, synchronously and with the same error, only when no handler returned TRUE
  (`WebKitWebView.cpp:2783-2801`). The library's handler (certificate exceptions, 0.8.0) keeps the flags when it does
  not accept the certificate, and `load-failed` reports them; an accepted certificate brings no `load-failed` at all.
  Exactly one detail either way.
- A host that never answers: before a main-frame load WebKit preconnects to it (`WebPageProxy.cpp:2343, 8190-8207`,
  `ENABLE_SERVER_PRECONNECT` on GLib ports), the preconnect has a 10 s timeout and cancels its load when it fires
  (`NetworkProcess.cpp:1747-1770`, `PreconnectTask.cpp:119-124`), and libsoup has handed the connection it was still
  opening to the page's request (`soup_session_steal_preconnection()`, `soup-session.c:1661-1682` in libsoup 3.4.4).
  So the load fails 10 s after STARTED with `g-io-error-quark` 19, "Operation was cancelled" (WebKitGTK 2.54.0, with
  and without process swaps). A load WebKit itself cancels is `WebKitNetworkError` 302 and never a FAILED.
- The same takeover also brings a cancellation without any timeout (0.10.3). The preconnect checks the server's
  certificate itself when the web process' request comes after the TLS handshake got that far, and a certificate the
  session does not accept (no exception applied yet, or none at all) makes it cancel itself
  (`NetworkDataTaskSoup.cpp:159-162, 502-515`). libsoup still hands its connection over until the connect operation
  has completed (`soup_session_steal_preconnection()` checks only the preconnect's state, `soup-session.c:1741-1764` in
  libsoup 3.8.1), and the connect, which runs with the preconnect's cancellable, ends the request with
  `g-io-error-quark` 19 a moment after STARTED. That load never saw the certificate: no
  `load-failed-with-tls-errors`, no exception could apply, and up to 0.10.2 the host got TIMEOUT instead of the page
  (with an exception) or CERTIFICATE (without). It happens when the request comes within a millisecond or so after the
  preconnect's rejection; WPE WebKit 2.54.3 in containers showed it in 3 of 100 runs of `--site-test` and in a few of
  every ten CI runs. Since 0.10.3 the library loads such a load again from an idle when it is an https main-frame
  load, still provisional, and failed less than 5 s after STARTED (the preconnect's own timeout is 10 s): no detail, no
  FAILED, no error page, the host sees STARTED, FINISHED, then the new load, which checks the certificate itself (an
  exception applies, or CERTIFICATE). At most twice within 10 s for one URI, then the failure goes to the host as
  before (TIMEOUT); a navigation of the host or the page between the failure and the repeat wins (as for the
  certificate exceptions). The decision and the guard are `uc_load_error_cancelled_early()` and `UcLoadRetries`
  (`uc_load_error.c`, `tests/test-load-error.c`); `--load-fail-test`'s `tls-race` step provokes the case
  ("ucwebview-test"). Log line: `ucwebview: https://192.168.1.20: load cancelled 2 ms after it started, on the
  connection of WebKit's preconnect, which had failed on the server's certificate; loading again`.
  libsoup has no I/O timeout in WebKit's session (`SoupNetworkSession.cpp:116-118`); the kernel's connect timeout
  (`G_IO_ERROR_TIMED_OUT`, about 130 s) and WebKit's own request timeout (`WebKitNetworkError` -1001,
  `ResourceErrorSoup.cpp:109-118`, only for requests with a timeout interval) are mapped too.

The library's log line of a failure names the domain and code since 0.10.0:
`ucwebview: load failed: http://www.example.com: Error resolving “www.example.com”: Temporary failure in name
resolution (g-resolver-error-quark 1)` (the URI and URIs in the message as described in "What the logs carry", the
origin only since 0.10.1). `ucwebview-test` prints the
detail in every mode (`load failed: <uri>: HOST_NOT_FOUND (g-resolver-error-quark 1, tls 0x0): <message>`) before its
`load FAILED` line, and `--load-fail-test` checks it end to end ("ucwebview-test").

### PDF viewer (off since 0.6.1)

WebKit 2.54 shows a PDF in its built-in PDF.js viewer (`ENABLE_PDFJS` is on for WPE, `OptionsWPE.cmake:53`; the
preference `PDFJSViewerEnabled` is true for GTK and WPE, `UnifiedWebPreferences.yaml:6491-6506`). On a Remote 3
(RK3566, Panfrost) the web process crashed about 2 s after the viewer drew, in 3 of 3 runs with the W3C `dummy.pdf`,
and a host that reloads after a crash (remote-ui does) would loop. So the viewer is off. Library 0.6.1 does this
inside the library, so hosts need no change (`uc_pdf_policy.c`):

- `uc_webview_global_init()` sets the WebKit feature `PDFJSViewer` to FALSE in the `WebKitSettings` that all views
  share. GLib names a feature by its preference without `Enabled` (`WebKitFeature.cpp:131-136`), so a lookup of
  `PDFJSViewerEnabled` finds nothing. Without the viewer the web process does not create the viewer's document
  (`DOMImplementation.cpp:175-178`) and reports a PDF as a type it cannot show (`WebPage.cpp:8114-8117`), which
  WebKit's default policy ignores (`WebKitWebView.cpp:659-662`). An `<embed>` or `<object>` of a PDF type no longer
  becomes a frame (`WebLocalFrameLoaderClient.cpp:1896-1901`).
- A `decide-policy` handler on every view ignores navigation responses of the types `application/pdf`, `text/pdf`
  and `application/x-pdf` (any case, parameters ignored), in the main frame and in frames. Without it a `text/pdf`
  response shows as raw text, because WebKit shows every `text/` type it does not exclude
  (`MIMETypeRegistry.cpp:643-644`).
- The viewer's own scheme `webkit-pdfjs-viewer:` needs no rule. WebKit loads it only into a frame inside the
  viewer's document (`ResourceLoader.cpp:258-263, 923-932`), and that document no longer exists.

What the host and the user see:

| Case | Host callbacks (0.6.1) | Since 0.7.0 | User |
|------|------------------------|-------------|------|
| tap on a PDF link (also a PDF sent as attachment) | `load_changed` STARTED (URI of the current page), FAILED (URI of the PDF), FINISHED (URI of the current page); no COMMITTED | STARTED, `response_refused(URI of the PDF, "application/pdf", UNSUPPORTED)` (an attachment: `DOWNLOAD`), FINISHED; no FAILED | the page stays; a host shows its load failed state with 0.6.1, nothing with 0.7.0 unless it handles `response_refused` |
| `uc_webview_load_uri()` of a PDF | STARTED (URI of the PDF), FAILED (URI of the PDF), FINISHED (URI of the page that stays, `""` in a new view) | STARTED, `response_refused`, FINISHED; no FAILED | as above |
| `<iframe>` with a PDF | none (frames report no load events) | none | the frame stays empty (`about:blank`) |
| `<embed>` of a PDF, with or without `type` | none | none | nothing: WebKit creates no frame for it |
| `<object>` of a PDF | none | none | the object's fallback content |

The journal gets `ucwebview: PDF <URI> (application/pdf) not shown, the PDF viewer is off` (`PDF in a frame ...` for
frames). With 0.6.1 the main frame also logged `ucwebview: load failed: <URI>: Frame load interrupted` (WebKit's
`WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE`; WebKit shows no error page for it,
`WebKitWebView.cpp:616-625`); since 0.7.0 that failure is not reported (see "Requests (0.7.0)"). URIs follow "What the
logs carry". The start line ends with `PDF viewer off`. Since 0.7.0 the rule lives in the response policy of
`uc_requests_wk.c`, next to downloads and unknown types.

`UC_WEBVIEW_PDF_VIEWER=1` (also `true`/`yes`/`on`, read once by `uc_webview_global_init()`) keeps the viewer on, for
the crash investigation: the start line ends with `PDF viewer on (UC_WEBVIEW_PDF_VIEWER)`, and the library leaves
every decision to WebKit. A WebKit built with `ENABLE_PDFJS=OFF` has no such feature: the start line then ends with
`no PDF viewer in this WebKit` and the response rule still applies.

Checked on WebKitGTK 2.54.0 (same `WebKitWebView.cpp`, `WebKitFeature.cpp`, `WebPage.cpp`,
`WebLocalFrameLoaderClient.cpp` and WebCore) on a broadway display. A probe compiled `uc_pdf_policy.c` in, made the
two calls of `uc_webview.c`, and loaded pages from a local server: the cases above plus `text/pdf`,
`application/x-pdf`, `Application/PDF; charset=binary`, a `target=_blank` link, an `<object>` without type or file
extension, and the W3C file over HTTPS. With the viewer off every case behaved as in the table: no document was a
PDF.js document, and the web process never terminated. With `UC_WEBVIEW_PDF_VIEWER=1`, PDF.js opened for the link,
the API load, both iframes, both embeds and both objects; `application/x-pdf` and the attachment failed as before.
The feature switch alone showed `text/pdf` as raw text; the handler alone also kept PDF.js out, but WebKit still
reported PDFs as showable. A link or an iframe to `webkit-pdfjs-viewer://pdfjs/web/viewer.html` failed or stayed
`about:blank` with the viewer on and off.

Showing PDFs again needs a backtrace of the PDF.js crash and a fix in WebKit; a WebKit built with `ENABLE_PDFJS=OFF`
drops the code.

### Fonts (integration note)

This concerns the system image, not the library. A small image ships a few font families only; the Remote firmware ships
three: Poppins, Space Mono and Noto Color Emoji. WebKit's web process resolves CSS font families through fontconfig and
Skia (`FontCacheSkia.cpp:411-427` `matchFamilyStyle()`), and Skia's fontconfig manager accepts fontconfig's match only
when one of the matched font's family names is in the (substituted) pattern, up to its last strongly bound name
(`SkFontMgr_fontconfig.cpp:876-913`, `remove_weak()` at 251-281). fontconfig's own `60-latin.conf` prefers DejaVu,
Liberation and the like for `sans-serif`, `serif` and `monospace`; none is installed, `fc-match sans-serif` answers
Poppins as a best effort, and Skia rejects it because "Poppins" is not in the pattern. WebKit then tries the next family
of the CSS list, and at the end its last resort, `serif`, which fails the same way, so the primary font becomes an empty
typeface (`FontCacheSkia.cpp:180-200` `lastResortFallbackFont()`, `SkTypeface::MakeEmpty()`). Its glyphs are still
drawn, because WebKit finds a font per character through `matchFamilyStyleCharacter()` (`FontCacheSkia.cpp:142`, no
family name check), but the primary font's ascent and descent are 0 (`FontSkia.cpp:99-115`): the line box keeps the
fallback font's height, while every inline box has none.

What that did on the Remote before its image had the aliases: text links and label texts could not be tapped, the
click went to the block behind them (`<p>`, `<body>`), and a checkbox sat 8 px below where the text was (the baseline
of a font without ascent); block links and buttons worked. `getBoundingClientRect()` of an inline `<a>` reported a
height of 0, `canvas.measureText('x').fontBoundingBoxAscent` 0 for `22px sans-serif` and 23 for `22px Poppins`.

The fix is a fontconfig file in `/etc/fonts/conf.d/` (the Remote firmware installs `56-uc-webview-fonts.conf`; the
number puts it before `60-latin.conf`): `<alias>` rules that prefer Poppins for
`sans-serif`, `serif`, `system-ui`, `ui-sans-serif`, `ui-serif`, `ui-rounded`, `cursive` and `fantasy`, and Space
Mono for `monospace` and `ui-monospace`. `<prefer>` puts the installed family into the pattern before the generic
name with a weak binding, like `60-latin.conf` does, which is what Skia's rule needs. fontconfig's `45-latin.conf`
maps the common names (`Arial`, `Helvetica`, `Times`, `Courier`, ...) to the generic families first, so those
resolve to the same fonts; a family the device does not have and fontconfig does not know (`Roboto`) falls through
to the next family of the CSS list or to `serif`. The library itself does not change: the matching runs in the web
process, and WebKit's settings for the generic families (`webkit_settings_set_sans_serif_font_family()`) would cover
the CSS generics only, not the last resort. With the file every family of the test page gets the 31 px inline boxes
of Poppins at 22 px, and a tap on an inline link navigates. A minimal example (replace the families with installed
ones):

```xml
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <alias><family>sans-serif</family><prefer><family>Poppins</family></prefer></alias>
  <alias><family>serif</family><prefer><family>Poppins</family></prefer></alias>
  <alias><family>system-ui</family><prefer><family>Poppins</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>Space Mono</family></prefer></alias>
</fontconfig>
```

`ucwebview-test --link-test` checks the metrics first (element `font`, no tap) and names the cause when an inline
element has no height. On such a system `fc-match sans-serif` prints `Poppins-Regular.ttf` with and without the file
(fontconfig's best effort), so the check is the substituted pattern: `fc-pattern -c sans-serif family` lists
`"Poppins"(w)` before `"sans-serif"(s)` only with the file. (Noto Color Emoji is in that list even without it, but
`fc-match` never returns it for a non-colour request: the pattern's `color` comes before the weak family names in
fontconfig's priorities.)

A note for page authors: `<button onclick="open('...')">` and
`<button onclick="close()">` do not open or close a window. The scope of an inline event handler includes the element
and its document (`JSHTMLElementCustom.cpp:125-145`, HTML "event handler content attributes"), so they call
`document.open()` and `document.close()`: `document.open()` with one argument is the overload that clears the
document (`Document+HTML.idl:61-62`), which looked like a "black screen" with the title emptied. `window.open()` and
`window.close()` behave as expected (the `--window-test` and `--link-test` buttons call them from functions). This is
the same in every browser.

### Requests (0.7.0)

A page asks for things a browser shows outside the page: script dialogs, logins, new windows, downloads. Up to 0.6.1 the
library left them to WebKit's WPE defaults, which show nothing: dialogs closed at once (`confirm()` false), a login hung
until the next load, new windows did nothing, a download failed as "Frame load interrupted".
Since 0.7.0 the host gets them through six callbacks appended to `uc_webview_callbacks` (only through
`uc_webview_new_sized()`) and answers by id; without the callbacks the library decides alone.

| Callback | When | The host answers | Without the callback |
|----------|------|------------------|----------------------|
| `script_dialog(id, type, origin, message, default_text)` | `alert()`, `confirm()`, `prompt()`, the "leave this page?" question of a beforeunload handler (`UC_WEBVIEW_DIALOG_*`) | `uc_webview_request_reply(id, accept, text)` | closed at once with WebKit's default (`confirm()` false, `prompt()` null); beforeunload leaves |
| `authenticate(id, origin, realm, scheme, flags)` | a server or proxy asks for a login (`UC_WEBVIEW_AUTH_SCHEME_*`, `UC_WEBVIEW_AUTH_RETRY`, `_PROXY`) | `uc_webview_request_auth(id, user, password)`; `user` NULL or `uc_webview_request_reply(id, 0, NULL)`: without credentials | continued without credentials, the server's 401 page loads |
| `request_cancelled(id)` | a pending dialog or login went away first | closes its dialog, no answer | – |
| `close_requested()` | `window.close()` where WebKit allows it (a page opened by script or with one history entry, `DOMWindow.cpp:127-153`) | leaves the page or does nothing | nothing |
| `new_window(uri, result)` | `window.open()`, `target=_blank` (`UC_WEBVIEW_NEW_WINDOW_LOADED`/`_REFUSED`) | – (a report) | the same, unreported |
| `response_refused(uri, mime_type, reason)` | the main frame got a download or a type WebKit cannot show (`UC_WEBVIEW_RESPONSE_DOWNLOAD`/`_UNSUPPORTED`) | – (a report) | the same, unreported |

**Ids and the end of a request.** Dialogs and logins get a `uint64_t` id, process-wide from 1, never 0, never reused
(`uc_requests.c`). Each ends exactly once: the host's answer, or `request_cancelled()` when it went away first. An
answer to an unknown or ended id returns -1 and does nothing, so a late answer racing `request_cancelled()` is
harmless. The library ends pending requests itself:

| Moment | Dialogs | Logins | Report |
|--------|---------|--------|--------|
| a navigation call of the host (`load_uri`, `load_html`, `reload`, `stop`, `go_back`, `go_forward`; back/forward only when there is history) | "no" (beforeunload: stay) | cancelled | `request_cancelled()` from an idle right after the call (never inside a `uc_webview_*` call) |
| a main-frame commit | "no": a dialog of the old document, left in a web process WebKit swapped away from | – | before `load_changed(COMMITTED)` |
| `web-process-terminated` | "no", the dialog is closed (WebKit does not, `WebKitWebView.cpp:5405-5421`; its completion would never run) | cancelled | before `web_process_terminated()` |
| WebKit gives a login up: the `cancelled` signal, emitted before the next load's STARTED/FINISHED/failure (`WebKitWebView.cpp:2686-2705, 2734-2780`) | – | cancelled by WebKit | at once |
| the next load's STARTED, FINISHED or failure, for logins WebKit no longer tracks (it keeps the newest only, `:3190-3201`, and counts a non-401 load as authenticated without completing the challenge) | – | cancelled | at once, before `load_changed()` |
| `uc_webview_free()` | "no", closed | cancelled | none |

After a host navigation the page's script runs on after the dialog's "no" until the web process takes the queued load
(`WebChromeClient.cpp:593-667`: dialogs are synchronous calls). A following `confirm()` would block the load again, so
dialogs the page opens in that window (not beforeunload) close at once with WebKit's default, until the next load event
of the view, at most 2 s (`navigation_quiet_us`; a beforeunload "stay" ends the window).

**Dialogs.** `origin` is `scheme://host[:port]` of the last committed main-frame URI (`webkit_web_view_get_uri()` is the
pending URI during a load, `PageLoadState.cpp:190-211`; WebKitGTK titles its dialog with the committed URL too); NULL
for pages without a host (`about:blank`, `data:`, `uc_webview_load_html()` without a base URI). WebKit does not say
which frame asked, so a dialog of a frame from another origin shows the main page's origin. `message` comes as the page
wrote it, any length (the host elides); beforeunload passes NULL and the host shows a fixed text. The page waits until
the answer: no script, timer or rendering update of it runs (`uc_webview_when_rendered()` waits too), and WebKit stops
its responsiveness timer meanwhile (`WebPageProxy.cpp:11036-11037`). A dialog of an unmapped view is passed on at once.
beforeunload asks only after a user interaction with the page and once per navigation (`FrameLoader.cpp:4064-4079,
4109-4141`), for reload, back, forward and host loads as well. It does not ask for a cross-site navigation: WPE swaps
the web process for it (`WebKitWebContext.cpp:444`, `:445` in 2.54.2), and WebKit runs `beforeunload` only when the
navigation continues in the current process (`FrameLoader.cpp:4217`, `ContinueLoad` only). A link to another site leaves
without the question, a same-site link asks; the library does not work around WebKit here. A host may answer from inside
`script_dialog()`: the library closes the dialog from an idle then, because closing it inside its own emission clears
WebKit's current dialog before WebKit's unref after the emission, which then unrefs NULL (`WebKitWebView.cpp:2926-2972`;
a crash on WebKitGTK 2.54.0 without that). Dialog loop guard: four dialogs are shown when each comes less than 1 s after
the previous answer, within one document; from the fifth on, the document gets WebKit's default answer. A dialog that
comes less than 1 s after the answer to the previous one is quick; after three quick dialogs in a row in one document
(four dialogs with the first), further dialogs of that document get WebKit's default answer at once, logged once
(`ucwebview: dialog loop guard: ...`), until the user taps or presses a key in the page or the next main-frame commit.
`for (;;) alert(1)` shows four alerts. beforeunload neither counts nor is suppressed: WebKit's default for it ("stay")
would keep the user on the page. Since 0.7.2 the user's input ends the guard (`uc_wk_requests_user_input()`, logged once
as `ucwebview: dialog loop guard ended by user input`): a touch down (`uc_webview_touch()`), a button press
(`uc_webview_pointer_button()`), a key press (`uc_webview_key()`) and text input that a focused element takes
(`uc_webview_ime_commit()`, `_ime_preedit()` with text, `_ime_delete_surrounding()`); not releases, moves, cancels or
scrolling, and not the host's navigation calls. The guard starts afresh then, as at a commit (counter, trip and the time
of the last answer), so the next dialog is not quick and a page that loops by itself is stopped again after four. WebKit
draws the line the same way for a user gesture: mouse press and release, keys and touches set one, wheel events do not
(`EventHandler.cpp:2045, 2573, 4274, 5473`). In remote-ui a dialog covers the page and takes the keys, so answering it
is no input to the page. Before 0.7.2 the guard lasted until the next page, and a button that asks `confirm()` after a
tripped guard got a silent "no".

**Logins.** `origin` is the challenge's server (or proxy, flag `UC_WEBVIEW_AUTH_PROXY`), the default port left out;
`realm` the server's text for the protected area. `UC_WEBVIEW_AUTH_RETRY`: the previous credentials were rejected
(`webkit_authentication_request_is_retry()`; WebKit gives no count). Credentials go to WebKit with
`WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION`: kept in the network session's memory and libsoup's auth cache for the life
of the host process, never written (`webkit_authentication_request_can_save_credentials()` is FALSE on this build: no
libsecret, `WebKitAuthenticationRequest.cpp:211-251`). "Without credentials" is
`webkit_authentication_request_authenticate(NULL)`, WebKitGTK's "Cancel": the network process cancels the challenge and
the server's 401 response loads as the page (`NetworkDataTaskSoup.cpp:685-720`), with no FAILED. Several logins can be
pending at once; the library keeps them all. Client certificate and PIN requests never reach the host: continued without
a certificate, like WebKitGTK's default (no certificate store on the remote). A login answered after a long wait can
fail (documented, not fixed): httpbin.org's Basic login answered after 1 min 43 s failed with `HTTP/2 Error: NO_ERROR`,
and WebKit's error page showed. The server had closed its idle HTTP/2 connection (GOAWAY with NO_ERROR) while the login
screen was open, and the request with the credentials was not sent again on a new connection (libsoup and WebKit's
network process; the library only hands the credentials over). Reloading or following the link again works; the next
logins of that session worked.

**New windows.** `http`/`https` GET requests load into this view with the navigation's request (Cog's way,
`cog-launcher.c:223-228`), `create` returns NULL (a non-NULL view must be a new related view); `window.open()` returns
null to the page, so a page that talks to its popup through `window.opener` does not work. Other schemes (`data:`,
`javascript:`, `blob:`, `file:`, `about:`) and a form posted to a new window (whether WebKit hands the body over is not
verified; a POST without it or a GET instead is a different request) are refused: the page
stays. WebKit blocks `window.open()` without a user gesture before any of this. `window.close()` of such a page is
refused by WebKit (more than one history entry), so an OAuth popup that ends with `window.close()` leaves the user on
its last page.

**Responses.** The response policy (`decide-policy`, every frame) ignores, in WebKit's order
(`WebKitWebView.cpp:651-664`): a `Content-Disposition: attachment` (any type; WebCore's rule,
`ResourceResponseBase.cpp:834-840`), a PDF while the PDF viewer is off (0.6.1's rule), and a type WebKit cannot show
unless the status is 204. The main frame's refusal goes to `response_refused()` (a PDF as `UNSUPPORTED`); a frame's is
logged only. `<a download>` becomes a download before any response policy (`WebPageProxy.cpp:5836-5844`) and comes only
through the session's `download-started`: cancelled, reported as `DOWNLOAD` with `mime_type` NULL. Nothing is saved. The
ignored main-frame load fails with `FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE`, which the library no longer reports as
FAILED (WebKit's own handler shows nothing for it either, `WebKitWebView.cpp:616-631`):

| Case | Host callbacks |
|------|----------------|
| tap on an attachment link, a PDF or an unknown type | `load_changed` STARTED (URI of the current page), `response_refused(URI, type, reason)`, FINISHED (URI of the current page) |
| `uc_webview_load_uri()` of one | STARTED (its URI), `response_refused`, FINISHED (URI of the page that stays, `""` in a new view) |
| `<a download>` | `response_refused(URI, NULL, DOWNLOAD)` only |
| a frame with one of them | none, the frame stays empty |

**Design decisions** (one line each in `uc_request_decisions`, `uc_requests.c`, checked by `tests/test-requests.c`):

1. Without a `script_dialog` callback beforeunload leaves (WebKit: stays silently); alert, confirm and prompt keep
   WebKit's default. Without `authenticate` a login continues without credentials (WebKit: the load hangs until the next
   one); client certificate and PIN requests the same, with or without the callback. New windows load into the same view
   and downloads are refused, with or without the callbacks. These change what a host of a 0.6 header sees with a 0.7.0
   library: its beforeunload pages now leave, logins show the 401 page, links with `target=_blank` work, and a download
   or PDF no longer shows its load failed state.
2. The dialog loop guard: four dialogs are shown when each comes less than 1 s after the previous answer, within one
   document (three quick ones after the first, `guard_quick_dialogs` 3); from the fifth on, WebKit's default answer
   until the next main-frame commit (since 0.7.2 also until the user's input, decision 6). beforeunload is excluded.
3. beforeunload passes no page text; the host shows a fixed one.
4. The PDFs refused since 0.6.1 are `response_refused(UNSUPPORTED)` instead of FAILED; no refused response is FAILED.
5. Two library rules: after a navigation call of the host, further dialogs of the old document (not beforeunload)
   get WebKit's default answer until the next load event of the view, at most 2 s (`navigation_quiet_us`); a form
   posted to a new window is refused.
6. The dialog loop guard ends as soon as the user taps or presses a key in the page: a dialog that follows the user's
   input is no runaway loop, a page that loops by itself is still stopped (`guard_ends_on_input`, library 0.7.2; see
   "Dialogs" above for the input that counts).
7. A login answered after a long wait can fail with WebKit's error page when the server closed its idle connection
   meanwhile: documented, not fixed (see "Logins" above).

Not run: client certificate requests (no test server with client authentication), proxy logins, NTLM and Negotiate;
the commit end path for dialogs (needs a process swap with a dialog pending). The decisions for them follow WebKitGTK.

Checked on WebKitGTK 2.54.0 (Debian 13) on a broadway display, which shares the request code of WebKit 2.54
(`WebKitWebView.cpp`, `WebKitScriptDialog.cpp`, `WebKitAuthenticationRequest.cpp`, `WebKitDownload.cpp`, WebCore):
`tests/webkitgtk-host.c` implements `uc_webview.h` there with `uc_requests_wk.c` and the other request sources compiled
in unchanged, and the real `ucwebview-test` request modes run against it (`make ucwebview-test-gtk`, "Host checks").
Every mode and answer variant passed; see "Checks on a device" for the list.

### Site settings (0.8.0)

What remote-ui's web view entity configures per site ([Core-API
`entity_webview`](https://github.com/unfoldedcircle/core-api/blob/main/doc/entities/entity_webview.md), the UI's
`WebViewSite`) or for the whole process. Seven functions within API version 1; the UI resolves each with `dlsym()` and
leaves the setting at WebKit's default when the library lacks it. Every function returns 0 when the setting is taken and
-1 when it is refused (a warning names the reason, never a secret); a refused call changes nothing. Facts of this stack
they rest on, checked in the code: one `WebKitNetworkSession`, one shared `WebKitSettings` object and one `UcWpeDisplay`
per process (`global` in `uc_webview.c`), all views on the default `WebKitWebContext` (`WebKitWebView.cpp:957-958`), no
multimedia in the Remote's WebKit build, no handler for permission requests (WebKit's default denies them, pointer lock
aside, `WebKitWebView.cpp:667-678`).

| Function | WebKit API | Scope | Limits |
|----------|------------|-------|--------|
| `uc_webview_set_origin_headers(wv, origin, names, values, count)` | the library's web process extension, `WebKitWebPage::send-request` | the view's requests to `origin` | see "Origin headers" |
| `uc_webview_set_user_agent(wv, preset, custom)` | `webkit_settings_set_user_agent()` on settings of the view's own | the view, from the next request | reload a shown page |
| `uc_webview_global_set_cookies(url, cookies, count, cb, user)` | `webkit_cookie_manager_add_cookie()` of the session | every view | public suffixes refused; SameSite Lax |
| `uc_webview_global_allow_certificate(host, sha256)` | `load-failed-with-tls-errors`, `webkit_network_session_allow_tls_certificate_for_host()` | main-frame loads of `host`, then the whole process | cannot be removed; see "Certificate exceptions" |
| `uc_webview_global_set_tls_errors_policy(ignore)` | `webkit_network_session_set_tls_errors_policy()` | every host | new connections only |
| `uc_webview_global_set_color_scheme(scheme)` | `WPE_SETTING_DARK_MODE` of the library's display | every page | pages without `color-scheme` keep their look |
| `uc_webview_global_clear_data(types, cb, user)` | `webkit_website_data_manager_clear()` of the session | every view and site | login credentials stay |

The asynchronous calls (`set_cookies`, `clear_data`) call `cb(status, user)` exactly once for an accepted request,
`UC_WEBVIEW_DONE_OK` (0) or `UC_WEBVIEW_DONE_FAILED` (1), from an idle of the main context, never from inside a
`uc_webview_*` call. `allow_certificate`, `set_tls_errors_policy` and `set_color_scheme` may come before
`uc_webview_global_init()`, which applies them; `set_cookies` and `clear_data` need the session and refuse before it.
The start line after the library's own reports them: `ucwebview: site settings: web process extension
/usr/lib/ucwebview/web-process-extensions, TLS errors checked, 0 host(s) with a certificate exception, light color
scheme`.

**Origin headers.** For the entity's `bearer` and `header` authentication: `{"Authorization"}, {"Bearer <token>"}` or
an API key header for the requests to one origin. The options evaluated:

| Option | Covers | Problem |
|--------|--------|---------|
| `webkit_web_view_load_request()` with the headers | the main resource of the host's load | no subresource, `fetch()` or XHR; back, forward and reload send none; WebKit's network process copies every header of a request into its redirect except `Authorization` and `Origin` (`NetworkDataTaskSoup.cpp:788-837`), so a custom header follows a redirect to another site |
| a user script that wraps `fetch()`/XHR | the page's script requests | no main resource, images, scripts or style sheets; the page's scripts can read the token |
| other UI process API of WPE 2.54 | nothing | there is no hook into a page's requests in the UI process: `decide-policy` cannot change a request's headers, URI scheme handlers cannot take `http`/`https`, content filters only block |
| a web process extension (chosen) | every request of the page: main resource, subresources, `fetch()`, XHR, and each redirect hop again | the headers are in the memory of the web processes, see below |

`libucwebview-extension.so` is loaded by every WPEWebProcess from `/usr/lib/ucwebview/web-process-extensions`, which
`uc_webview_global_init()` sets on the default `WebKitWebContext` when the file exists
(`webkit_web_context_set_web_process_extensions_directory()`; the injected bundle loads every `.so` there,
`WebProcessExtensionManager.cpp:38-49, 101-123`). Without the file `set_origin_headers()` refuses with a warning and
nothing else changes. The library merges the headers of all views into one table (`uc_headers.c`; for an origin two
views set, the later call counts) and sends it as the initialization data of every new web process
(`initialize-web-process-extensions`, `WebKitWebContext.cpp:2017-2023`) and, after every change, to all running ones
(`webkit_web_context_send_message_to_all_extensions()`, `WebKitWebContext.cpp:1936-1945`: prewarmed and cached
processes too). A generation number keeps a broadcast that was queued for a process before it started from
overwriting its newer initialization data. The messages go over the same connection as the page's loads, so headers
set before `uc_webview_load_uri()` are there for its first request. In the web process, `WebKitWebPage::send-request`
runs for every request before it goes out, again for every redirect hop with the redirecting response
(`WebKitWebPage.cpp:313-325, 551-585`), and its changes to the request's `SoupMessageHeaders` replace the request's
header fields (`WebKitURIRequest.cpp:166-179, 214-219`, `ResourceRequestSoup.cpp:152-161`). The rules
(`uc_headers_decide()` and `uc_headers_apply()`, unit-tested):

- a request to the origin gets the headers when the page's top-level document is of the origin, or when it is that
  document's main resource: the request URI is the page URI, which is the URI being loaded once the provisional load
  started (`WebKitWebPage.cpp:209-237`);
- not when the request carries an `Origin` header of another origin or `null`: WebKit adds one to a form another site
  posts to the origin and to a CORS request of a frame of another site, the requests a forged one (CSRF) would be. A
  link or redirect of another site to the origin is a GET without `Origin` and gets them, like a `SameSite=Lax` cookie;
- not a request of a page of another site to the origin (an image, a frame, a script);
- a redirect away from the origin that the extension sees as one (a redirect hop in the same web process, e.g. of a
  subresource) removes the header names again;
- the headers replace a header of the same name the page sets itself;
- since 0.8.1, the value rule: every request that does not get an origin's headers under these rules loses each header
  whose name and exact value equal one of that origin's headers.

Why the value rule (0.8.1). WebKit's network process copies a request's headers into its redirect and removes only
`Authorization` and `Origin` at a cross-origin one (`NetworkDataTaskSoup.cpp:834-838`). WPE always swaps the web
process at a cross-site main-frame navigation (`WebKitWebContext.cpp:442-446`; WebKitGTK 4.1 only with
`process-swap-on-cross-site-navigation-enabled`), also at a server redirect: the decision comes before the old process'
`send-request` for the hop would run, and the new process loads the redirected request, copies included, as a first
request (`WebPageProxy.cpp:6403-6421`), without a redirected response and with the target as page URI. 0.8.0 stripped
by name at redirects it saw and let `X-Uc-Token` through to the other site there (`ucwebview-test --site-test`,
`headers-redirect-away`; the WebKitGTK harness without process swaps had passed). A copy keeps the value
it was given, so the value rule catches it on whatever path it came, in any web process and at every hop of a chain.
Alternatives weighed: refusing a cross-site main-frame redirect in the UI process' `decide-policy` and loading the
target again without the headers would change the navigation (a 307 POST becomes a GET, history and referrer change)
and covers main frames only; telling the new process that its first request came from a redirect is not possible
through WebKit's GLib API (the provisional page gets a new page id, `ProvisionalPageProxy.cpp:89`, and the extension
sees neither the original request nor the redirect).

Redirects into the origin (library 0.8.3). Up to 0.8.2 a request also counted as the main
resource when its redirecting response had the page URI. `send-request` carries no frame, so that held for an image, a
frame or a no-cors fetch of another site's page that loads the page's own URL: its server redirected that request into
the origin and the hop got the headers, an authenticated GET forged by any page the view showed (CSRF with the token;
`ucwebview-test --site-test`'s `headers-selfimg-redirect-into` and `headers-selfframe-redirect-into` failed with 0.8.2,
with and without process swaps). 0.8.3 drops that clause: a redirect hop into the origin is judged by the page URI it
comes with. A main-frame redirect of another site into the origin still gets the headers where WebKit starts the
redirected load in another web process, which sees the origin's URI as page URI: on WPE every cross-site main-frame
navigation, also at a server redirect, moves to another process (`WebKitWebContext.cpp:442-446`) once the current
process has committed a document (`WebProcessPool.cpp:2330-2332`; `headers-redirect-into`). Within one process the hop
comes with the other origin's page URI (`send-request` runs before WebKit updates it, `ResourceLoader.cpp:447` then
`:470`) and the origin's page starts without the headers: a same-site redirect (another port, a sibling
subdomain), the view's first navigation (`headers-first-navigation-redirect-into`) and the first load in a web process
that WebKit has just started for the navigation (a chain other site -> origin begun from a third site). The extension
API cannot tell a main-frame hop from a subresource one (no frame in `send-request`, no main-frame flag in the UI
process' navigation action, and the policy check of a main-frame redirect comes after `send-request`), so the safe
side is taken. A page that then loads its own subresources from the origin gets them (the top-level rule), and a
reload of the page gets them.

Provisional loads: WebKit sets the page URI when the main frame starts loading a new page
(`FrameLoader.cpp:4324` `prepareForLoadStart()` before `:4341` `startLoadingMainResource()`), while the old document
keeps running until the commit. Requests the old document makes in that window are judged by the new page URI, so a
document that navigates the main frame to the origin can send GETs to the origin that carry the headers. On WPE that
old document must be of the same site as the origin (a cross-site navigation moves to another web process, the old
document keeps its own page URI there). The extension API offers no committed document URL to close it.

Limits: a frame of another site that the origin's own page embeds can send no-cors GET requests to the origin that carry
the headers (no `Origin` header, the top-level document is the origin's); its responses stay opaque to the frame. A page
of another site that sends exactly a configured value under its name itself loses it (the value rule cannot tell it from
a copy). WebSocket handshakes, service worker requests and requests of WebKit's network process itself (favicons, DNS
prefetch) are not covered. Pings (`<a ping>`, CSP reports) never get the headers: WebKit sends them from the network
process without `send-request` (`PingLoader.cpp:252-255`). Residual paths for a copy: a keep-alive request
(`navigator.sendBeacon()`, `fetch(..., {keepalive: true})`) of the origin's page that WebKit's network process completes
after the page went away follows redirects without any web process (`NetworkResourceLoader.cpp:1476-1479`), so a
cross-site redirect of it carries the copies (all but `Authorization`); and a request in flight while the host changes
or removes the headers is checked against the new table, so a copy of the old value is not recognised. Every web process
holds the headers of every view's origins in memory while they are set (the library cannot tell which process a site
will use): a page never sees them, only a compromised web process could read them. `uc_webview_free()` and `count` 0
remove them from the table, and the library and the extension overwrite the values they own before freeing them
(`explicit_bzero()`, 0.8.3, best effort: the GVariant messages and WebKit's copies are not wiped). The extension checks
the table it receives again (normalized origins, valid names and values) and skips an entry that is not (0.8.3; only the
library can send the message). Values are never logged, names are (`ucwebview: 2 origin header(s) for
https://ha.example: Authorization, X-Api-Key`). Header names (0.8.3): RFC 9110 tokens of at most 64 bytes (the Core-API
spec's limit), not `Host`, `Cookie`, `Cookie2`, `Content-Length`, `Content-Type`, `Connection`, `Keep-Alive`,
`Transfer-Encoding`, `Upgrade`, `TE`, `Trailer`, `Expect`, `Origin`, `Referer`, `User-Agent` and no `Proxy-*` or `Sec-*`
(the spec's list and a few more); up to 0.8.2 256 bytes and a shorter list.

Sandbox: WebKit's helpers are plain children of the host in the same mount namespace (`ProcessLauncherGLib.cpp:268`,
no bubblewrap), so a sandboxed host only needs the extension directory visible to itself (remote-ui's units bind
`/usr/lib` read-only).

**Cookies and website data on disk.** `uc_webview_global_init()` with a data directory creates a persistent
`WebKitNetworkSession` (local storage, IndexedDB, service workers, HSTS and the HTTP cache in `data_dir` and
`data_dir/cache`). Its cookie jar was in memory only up to 0.7.x: WebKit stores cookies only after the application
names a file (`WebKitCookieManager.cpp:179-207`; the path starts empty, `WebsiteDataStore.h:649-650`). Since 0.8.0
`create_session()` sets `data_dir/cookies.sqlite` (SQLite, libsoup's `SoupCookieJarDB`, read when the network
process creates the session, `NetworkSessionSoup.cpp:60-61, 76-93`; libsoup3 depends on sqlite anyway). Cookies with
an expiry time are written as they change; session cookies stay in memory. The start line names the file:
`ucwebview: website data in /app/data/webkit, cache in /app/data/webkit/cache, cookies in
/app/data/webkit/cookies.sqlite`. Since
0.8.3 the library creates the file empty with mode 0600 before WebKit's network process opens it (SQLite takes an
empty file as a new database and gives its journals the database's mode, `os_unix.c` `findCreateFileMode()`) and sets
0600 on a file and journals an earlier release left 0644 (`uc_site_private_file()`). libsoup opens the database with
`PRAGMA secure_delete = 1` (`soup-cookie-jar-db.c:249`), so cookies it deletes (an expiry, `clear_data`) are
overwritten in the file; SQLite's temporary rollback journal is deleted, not wiped, and the flash storage may keep old
blocks; the 0700 directory and the 0600 file keep them from other users.

The data directory is the host's (remote-ui passes its `UC_WEBVIEW_DATA_DIR` to `uc_webview_global_init()`). The
library creates it with mode 0700 for the host's user, so other users, including a group that may read the parent,
cannot read the cookies. A host that passes no data directory keeps an ephemeral session (nothing written).

**Cookies from the host.** `uc_webview_global_set_cookies(url, cookies, count, cb, user)` checks every cookie first
(`uc_site_cookie_check()`, `uc_site.c`) and sets none when one breaks a rule (the header has them; since 0.8.3 the name
is an RFC 6265 token of 1..256 bytes and the value at most 4096 bytes of RFC 6265 cookie-octets, optionally quoted, the
Core-API spec's limits: before, bytes above 0x7e passed, WebKit turned invalid UTF-8 into a null string, dropped the
cookie in the network process, `CookieSoup.cpp:65-69, 80-81`, and `cb` still reported OK), plus libsoup's public suffix
rule (the jar drops cookies for `com`, `co.uk`, ..., `soup-cookie-jar.c:613-618`; the library refuses them instead).
`domain` NULL is a host-only cookie of the URL's host; a domain means the domain and its subdomains (libsoup's leading
dot), an IP address only itself. `path` NULL is `/` (WebKit drops a cookie without a path, `CookieSoup.cpp:78-81`).
`expires` -1 is a session cookie, a time in the past deletes. The cookies go through the session's `WebKitCookieManager`
into the network process' jar without a first-party check (`WebKitCookieManager.cpp:291-300`,
`WebCookieManager.cpp:128-137`, `NetworkStorageSessionSoup.cpp:500-503`); `cb` reports when all of them are there: the
UI loads the start page in `cb`. SameSite is libsoup's default, Lax. The log names the URL and the count, a refused
cookie's name and the rule, never a value.

**Certificate exceptions.** `uc_webview_global_allow_certificate(host, sha256)` keeps exceptions in the library
(`UcTlsExceptions`, up to 16 fingerprints per host and 64 hosts; IPv6 with or without brackets, IDN in punycode,
lower case: the form WebKit compares, `SoupNetworkSession.cpp:321-344`). WebKit's own API wants the certificate
itself (`webkit_network_session_allow_tls_certificate_for_host()`), which the host does not have, so the library
waits for it: when a main-frame load fails on its certificate while the policy is FAIL, WebKit emits
`load-failed-with-tls-errors` with the certificate (`WebKitWebView.cpp:2783-2799`, `WebKitNavigationClient.cpp:62-73`).
The library computes the SHA-256 of its DER encoding, and when an exception matches (that fingerprint, or `sha256`
NULL: any certificate of the host) it accepts the certificate for the host in the session
(`SoupNetworkSession.cpp:346-349`), returns TRUE (WebKit emits FINISHED without `load-failed`) and loads the URI again
from an idle. The host sees STARTED, FINISHED, then STARTED, COMMITTED, FINISHED of the new load. Since 0.10.3 a
navigation call of the host (`load_uri`, `load_html`, `reload`, `stop`, `go_back`, `go_forward`), for example from its
FINISHED callback, or another load that starts first, wins: the library does not load the URI again then
(`ucwebview: <origin>: not loaded again, another navigation came first`; up to 0.10.2 the repeated load replaced that
navigation). The first load can also fail before WebKit shows it the certificate, on the connection of a preconnect
that rejected it; the library loads it again as well ("Load failures (0.10.0)"). When the same URI
fails again within 10 s with the same certificate after two such attempts, the library gives up (WebKit's failure as
usual); since 0.8.3 the guard is keyed on the certificate's fingerprint too: another certificate of a
load-balanced host with an exception of its own is accepted again, and the same certificate gets a second attempt,
because `allow()` goes to the network process on another IPC connection than the repeated load and can arrive later
once. Without a match the log has the
fingerprint the user needs for an exception: `ucwebview: https://192.168.1.20/: the TLS certificate of 192.168.1.20 is
not valid (errors 0x1) and has no exception; its SHA-256 fingerprint is 768b8e95...`. Limits: WebKit keeps an accepted
certificate for the host for the rest of the process (no API removes it), for subresources too; a subresource of the
host requested before the main frame accepted the certificate fails; the repeated load is a GET of the URI (a form
posted to such a host is not sent again). `uc_webview_global_set_tls_errors_policy(1)` accepts every certificate of
every host (`WEBKIT_TLS_ERRORS_POLICY_IGNORE`, `WebKitNetworkSession.cpp:430-448`); it takes effect for new
connections, an open keep-alive connection keeps the decision it was made with.

**User agent.** The user agent lives in `WebKitSettings`, which all views share. A view that gets a preset other than
DEFAULT gets settings of its own, built like the shared ones (`settings_new()`: developer extras, console log, PDF
viewer switch, the default user agent), and `webkit_web_view_set_settings()` applies them and the custom user agent to
the page at once (`WebKitWebView.cpp:815-834, 4104-4122`). Presets:

| Preset | User-Agent |
|--------|------------|
| `UC_WEBVIEW_USER_AGENT_DEFAULT` | the Remote's, also for every view that never calls `uc_webview_set_user_agent()` (0.8.2): WebKit's own followed by `UCRemote/<firmware version>`, `Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/60.5 Safari/605.1.15 UCRemote/2.13.2` on the remote (WebKit's part: `UserAgentGLib.cpp:79-122`; no chassis information in the sandbox, so not the mobile form). Up to 0.8.1 WebKit's own without the token |
| `UC_WEBVIEW_USER_AGENT_MOBILE` | `Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Mobile/15E148 Safari/604.1` |
| `UC_WEBVIEW_USER_AGENT_DESKTOP` | `Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Safari/605.1.15` |
| `UC_WEBVIEW_USER_AGENT_CUSTOM` | `custom`: 1..512 bytes of visible ASCII and spaces |

The presets claim the engine the remote has (AppleWebKit/605.1.15 is the token WebKit itself sends), so a site that
serves Safari markup serves what WebKit renders; the platform part is what sites tell phones and computers apart by.

The Remote's token (0.8.2; the Core-API entity documentation says "`default`: the user agent of the Remote, which
identifies the Remote and its firmware version"): `UCRemote/<version>`, an RFC 9110 product token
(`UC_SITE_UA_PRODUCT` in `uc_site.h`), appended with a space to WebKit's standard user agent, so that WebKit's
platform part and the Safari tokens that sites test for stay. `<version>` is `UC_OS_VERSION` of the firmware, e.g.
`2.13.2` or `2.13.2-dev.3+5bf0cf38` (the firmware's version as its build writes it
into `/etc/uc-release`; 1..64 bytes of RFC 9110 tchar, else it is ignored). Where the library finds it, once per
process in `uc_webview_global_init()`:

1. `/etc/uc-release`, line `UC_OS_VERSION=...` (`uc_site_release_version()`, unit-tested: comments, quotes, CR LF,
   the unfilled template `$BUILD_VERSION`), when the process can read it: `ucwebview-test` and other hosts outside the
   sandbox;
2. else `UC_OS_VERSION` in the environment. A sandboxed host that cannot see the file gets it this way: a systemd unit
   with `EnvironmentFile=-/etc/uc-release` (read by systemd outside the sandbox) puts `UC_OS_VERSION` into the
   environment of the host and of the library, without a bind of the file (remote-ui's units do this);
3. else the token is `UCRemote` without a version.

The start line says which: `ucwebview: default user agent "Mozilla/5.0 (...) Safari/605.1.15 UCRemote/2.13.2"
(UC_OS_VERSION)` (or `(/etc/uc-release)`, or `(no version: no readable release file, no UC_OS_VERSION)`).
`webkit_settings_set_user_agent_with_application_details()` is not used: without a
version it appends WebKit's own 605.1.15 (`UserAgentGLib.cpp:146-149`). MOBILE, DESKTOP and CUSTOM carry no token.

**Color scheme.** `uc_webview_global_set_color_scheme(DARK)` sets `WPE_SETTING_DARK_MODE` on the library's display.
WebKit reads it from the primary display for the whole process (`SystemSettingsManagerProxyWPE.cpp:103-106, 206-216`)
and tells every page when it changes (`WPEWebViewPlatform.cpp:142-145`, `WebPageProxy.cpp:17431-17461`):
`prefers-color-scheme` and `matchMedia()` change events follow at once, and pages that declare `color-scheme: light
dark` get dark default colors. A page without a `color-scheme` keeps its look; its background where it is transparent
is `uc_webview_set_background_rgba()`.

**Clearing website data.** `uc_webview_global_clear_data(UC_WEBVIEW_DATA_CACHE)` clears WebKit's memory and disk
cache, `UC_WEBVIEW_DATA_ALL` everything WebKit lists (`WEBKIT_WEBSITE_DATA_ALL`: caches, cookies, local and session
storage, IndexedDB, service workers, HSTS, ITP, ...), in memory and in the data directory, with timespan 0 (with a
timespan WebKit keeps the cookies, `WebKitWebsiteDataManager.cpp:1323-1338`). Clearing the memory cache also drops
the back/forward cache and WebKit's cached web processes (`WebsiteDataStore.cpp:937-945`). Not cleared, also with
ALL: the credentials of logins answered with `uc_webview_request_auth()` (in memory until the process ends; WebKit
2.54's GLib API has no website data type for them), the certificate exceptions, the origin headers and the cookies
the host sets later. Other `types` values are refused, so a later release can add types. Shown pages stay; the UI
reloads them.

`ucwebview-test --site-test` checks all of this like a host (below).


### Device state for web pages (0.9.0)

What the pages of a web view can learn about the Remote, for dashboards that pause while the display is off, show the
battery or reconnect after a standby. The page side (API, events, the state per power mode, the standby rules) is
documented for web developers in the [Core-API's web view entity
documentation](https://github.com/unfoldedcircle/core-api/blob/main/doc/entities/entity_webview.md), section "Device
state for web pages", with a desktop simulator
([`tools/device-api-simulator`](https://github.com/unfoldedcircle/core-api/tree/main/tools/device-api-simulator))
whose shim behaves like the library's; remote-ui's
[docs/webview.md](https://github.com/unfoldedcircle/remote-ui/blob/main/docs/webview.md) has the host side.

**Hidden page throttling, every view.** `uc_webview_set_mapped(0)` hides the page (the display is off, the item
invisible): `visibilitychange`, no rendering, no `requestAnimationFrame`. Up to 0.8.x its timers kept their rate:
WebKit switches `HiddenPageDOMTimerThrottlingEnabled` and `HiddenPageCSSAnimationSuspensionEnabled` on by default for
GTK and Cocoa only (`UnifiedWebPreferences.yaml`). Since 0.9.0 the library switches both on in the settings of every
view (feature identifiers `HiddenPageDOMTimerThrottling` and `HiddenPageCSSAnimationSuspension`: WebKit drops
`Enabled` from the preference key, `WebKitFeature.cpp:126-135`). A hidden page's timers are then aligned to 1 s and its
CSS animations stop. WebKit aligns a timer only once it reached the maximum nesting level (`Document::
domTimerAlignmentInterval()`): a new `setInterval(..., 100)` runs its first five times at its own rate. The start
line says `hidden pages throttled`. WebKit's low power mode (30 fps, 30 ms timer alignment, triggered by
`net.hadess.PowerProfiles` on D-Bus) is not used: the page is hidden whenever the Remote would save power.

**The device API, per view and opt-in.** A host switches it on with `uc_webview_set_device_api(view, 1)` (remote-ui:
the web view entity's `device_api` option) and reports the state with `uc_webview_set_device_state(view, json)`:

```json
{"screen": "dim", "powerSource": "battery", "powerProfile": "balanced", "level": 42}
```

`screen` `on` / `dim` / `off`, `powerSource` `ac` / `battery`, `powerProfile` `power-saver` / `balanced` /
`performance`, `level` the battery level in percent (an integer 0..100); every member optional, a missing one keeps
its value, an invalid one too (logged), unknown members are ignored (a newer host). The reader (`uc_device.c`) is
strict RFC 8259 JSON, at most 4096 bytes and 16 levels, no member name twice; anything else is refused as a whole. A
view starts with `on`, `ac`, `balanced`, 100. `uc_webview_device_event(view, "suspend" | "resume", slept_ms)` reports a
standby. Pages of a view without the API get nothing: no `deviceHost`, no `getBattery()`.

**How it reaches the page.** Without a script of WebKit's UI process API: `webkit_web_view_evaluate_javascript()`,
`_call_async_javascript_function()` and `_run_javascript()` all run with `ForceUserGesture::Yes`
(`WebKitWebView.cpp:4500, 4648, 4921`), and `ScriptController::executeScriptInWorld()` then sets a user gesture on the
page's document whatever the script world (`ScriptController.cpp:654-662`). WebKit consumes the transient activation
afterwards, but the page keeps its sticky activation (`navigator.userActivation.hasBeenActive`),
`Page::userDidInteractWithPage` (which arms the "leave this page?" question of a `beforeunload` handler,
`FrameLoader.cpp:4064-4078`) and `Frame::hasHadUserInteraction`. The first draft of 0.9.0 pushed every change that way;
`ucwebview-test --device-test` shows it: `act:1` from the first state push on, and the question at every reload. Since
the rework the library's web process extension does it with the JavaScriptCore GLib API, which knows no user gesture
(protocol and reasons in `uc_ext_protocol.h`):

1. The extension creates a `WebKitScriptWorld` named `ucwebview-host`, the library's script world, when its web process
   starts. The web process maps the library's user content of that name to it ("The GLib API doesn't allow to create
   script worlds from the UI process. We need to use the existing world created by the web extension if any. The world
   name is used as the identifier.", `WebUserContentController.cpp:139-158`), so the user content and the extension
   share the world's window objects; page scripts never see them. For every document of a main frame the extension
   puts the function `ucwebviewDeviceStart` on that world's window (`window-object-cleared`, which WebKit emits for
   every document because the world exists, `WebKitWebPage.cpp:276-290`).
2. While the API is on, the view's user content manager has a user script in that world: `ucwebviewDeviceStart({...})`
   with the view's state (`uc_device_start_script()`, which writes only names of its tables and an integer, never text
   of the host's input). `WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START`, `WEBKIT_USER_CONTENT_INJECT_TOP_FRAME`, no allow
   or block list: every top-level document of the view runs it before its first script, frames do not; a user script
   runs without a user gesture (`LocalFrame::injectUserScriptImmediately()`). WebKit adds a user script for documents
   that start later (`InjectUserScriptImmediately::No`, `WebKitUserContentManager.cpp:235-240`), so switching the API on
   or off takes effect at the next load.
3. `ucwebviewDeviceStart` evaluates `device-host-shim.js` (compiled into the extension) in the page's own world, one
   function expression, and calls it with the state, before any script of the page ran. It returns the document's
   update function, which the extension keeps as `ucwebviewDeviceUpdate` on the library's window object (not writable,
   not configurable), one per document; it lives in the back/forward cache with its document. A second call in the
   same document (`document.open()`) does nothing. The page has no reference to it: no internal entry point on
   `window` any more (the first draft had `window.__ucDeviceHostUpdate`, which a page could call to fake events for
   itself).
4. A state change replaces the user script (the new one is added before the old one is removed) and goes to the shown
   page as the message `ucwebview.device.1` (`webkit_web_view_send_message_to_page()`, an `a{sv}` with `screen`,
   `powerSource`, `powerProfile`, `level`), on the connection of the page's web process in order with WebKit's own
   messages. The extension makes a JSON object of the known members with valid values (`uc_device_change_json()`),
   parses it in the page's world (plain data properties: no setter of the page runs) and calls the document's update
   function; a document that started while the API was off has none and ignores the message.
5. An event goes the same way, with the view's state: `{event: "suspend", ...state}` applies the state, then fires
   `freeze` on `document` and `suspend` on `deviceHost`; `{event: "resume", sleptMs, ...state}` fires `resume` on both
   first, then applies the state. remote-ui reports the standby's state before `suspend` and the new state after
   `resume`, so a page sees the state change, freeze, suspend, then resume, resume and the new state.
6. After every finished load of the main frame the state goes there once more: a page from the back/forward cache kept
   the state of its first load. A page that already has the state sees no event (events fire only on real changes).
7. The update function posts the change to a `MessageChannel` of the shim and returns; the page's handlers run in a
   task of their own, in order. It posts with the port and `postMessage` it took before the page's first script,
   through a bound call: nothing a page replaces later (`Array.prototype.push`, `MessagePort.prototype.postMessage`,
   `Function.prototype.call`) runs inside the extension's call (`tests/test-device-shim.mjs` checks it).
8. Process swaps and crashes: WPE moves a page to another web process at every cross-site navigation; the new page has
   another page id, its messages to the view are dropped until it commits (`ProvisionalPageProxy.cpp:713-877`), and
   the view's messages reach it only after the commit, when its document has started. The user script is part of the
   page's creation parameters in whatever process, so the new document starts with the state like any other; the
   messages follow the committed page. No message goes to a view before its first load or after its web process
   terminated, until it loads again (`uc_page_wk.c`): WebKit would start a web process for it
   (`WebPageProxy::ensureRunningProcess()`), which a crashed view must not get from a state change.
**The shim** (`device-host-shim.js`, the same behaviour as the shim of the Core-API's device API simulator, checked
with the same change sequence: identical events, event classes, details and attributes):

| Page API | Behaviour |
|----------|-----------|
| `navigator.getBattery()` | the same promise of one `BatteryManager` (on `Navigator.prototype`, like a browser's): `charging` = power source `ac` (also at full level), `level` = percent / 100, `chargingTime` 0 on power at 100 %, else `Infinity`, `dischargingTime` `Infinity`; `chargingchange` (with `on-ac` / `on-battery`), `levelchange`; `chargingtimechange` and `dischargingtimechange` exist (with their `on...` handlers) but never fire |
| `document` | `freeze` and `resume` events, `document.onfreeze` / `onresume` (accessors on `Document.prototype`) |
| `window.deviceHost` | `version` 1, read-only `screen`, `powerSource`, `powerProfile`; events `suspend` (an `Event`), `resume` (`detail.sleptMs`), `on-ac`, `on-battery`, `screenchange` (`detail.state`), `powerprofilechange` (`detail.profile`), only through `addEventListener`; no thermal state |
| `<html>` | `data-screen`, `data-power-profile`, set before the page's scripts and on every change |

Order within one change: `chargingchange`, `on-ac` / `on-battery`, `levelchange`, `screenchange`, `powerprofilechange`;
every attribute has its new value before the first event. Values that are not valid are ignored (the shim does not rely
on the library's checks). The shim differs from the simulator's in what a page cannot tell apart in normal use: the
deferred delivery above, the validation, event handler attributes that follow HTML (a handler set to `null` is removed,
set again it is added at the end), the same promise for every `getBattery()` call (the specification's battery promise;
the simulator returns a new promise of the same object), and the entry point: the simulator drives its shim through
`window.__ucDeviceHostUpdate`, the Remote through the update function only the extension holds.

**Standby is best effort.** The library never delays a standby. `CLOCK_MONOTONIC` stops while the Remote sleeps, so
timers and `performance.now()` do not see the standby; `sleptMs` is the host's measurement (remote-ui:
`CLOCK_BOOTTIME - CLOCK_MONOTONIC` before and after).

### Internals

| Type | Implements |
|------|-----------|
| `UcWpeDisplay : WPEDisplay` | `connect`, `create_view`, `create_toplevel`, `get_egl_display`, `get_keymap`, `get_preferred_buffer_formats`, `get_n_screens` (1), `get_screen`, `get_drm_device`, `create_input_method_context`; created with `g_object_new()` and passed to the `WebKitWebView` through the construct-only `display` property (`WebKitWebView.cpp:1368-1370`), set as primary display |
| `UcWpeScreen : WPEScreen` | one virtual screen (id 1) with the view's size, 160 dpi physical size and the display refresh rate; `get_sync_observer` returns its `UcWpeSyncObserver` (`uc_wpe_screen.c`) |
| `UcWpeSyncObserver : WPEScreenSyncObserver` | `start`/`stop` of a `UcVsync` thread that calls the base class's `sync` at the refresh rate + 1 %, see frame pacing |
| `UcWpeKeymap : WPEKeymap` | see above |
| `UcWpeView : WPEView` | `render_buffer`, `set_cursor_from_name` → `cursor_changed`, `can_be_mapped`; mapped and focused at creation |
| `UcWpeToplevel : WPEToplevel` | `resize` → `wpe_toplevel_resized` + `wpe_view_resized`, `set_fullscreen` (state only), `get_screen`; state ACTIVE, max 1 view |
| `UcWpeIme : WPEInputMethodContext` | see above |
| `UcGestureController : WPEGestureController` | 0.6.0, only for a host with the `pinch` callback: wraps WebKit's default controller and runs the pinch tracker (`uc_pinch.c`), see "Pinch zoom" |

Device state for web pages (0.9.0) adds no subclass: `uc_device_wk.c` (WebKit GLib API shared with WebKitGTK) keeps
the start script in the view's user content manager and sends the changes through `uc_page_wk.c` (messages to the
page's web process extension, only while the page is reachable), `uc_device.c` (GLib only, compiled into the library
and the extension) holds the state, its JSON reader, the start script, the changes and their JSON for the shim;
`uc_ext_device.c` is the extension's side (the library's script world, the shim, the update function), and
`device-host-shim.js` is compiled into the extension as bytes (the Makefile generates `uc_device_shim.h` with `od`).
`uc_ext_protocol.h` is what the library and the extension agree on.

Site settings (0.8.0) add no subclass either: `uc_site_wk.c` (WebKit GLib API shared with WebKitGTK, like
`uc_requests_wk.c`) sets cookies, clears data, handles `load-failed-with-tls-errors` and sends the origin header table;
`uc_site.c` and `uc_headers.c` (GLib only) hold the checks, the tables and the per-request decision, and
`uc_extension.c` is the web process extension.

`uc_webview_when_rendered()` adds no subclass: the probe goes to the web process extension through `uc_page_wk.c` (since
0.9.0) and reports through the view's own `WebKitUserContentManager` (WebKit creates one per view,
`WebKitWebView.cpp:990-991`); `uc_rendered.c` keeps the requests and the probe's text, which the WebKitGTK harness
shares. The magnification scripts (`uc_magnify.c`) go the same way. Neither do the requests of 0.7.0: `uc_requests_wk.c`
connects `script-dialog`, `authenticate`, `create`, `close` and `decide-policy` of the `WebKitWebView` and gets the
load, failure and termination events and the session's `download-started` from `uc_webview.c`; it uses only the WebKit
GLib API that WPE and WebKitGTK share, which is what lets the WebKitGTK host harness run it. `uc_requests.c` (GLib only)
holds the table, the rules and the decisions.

Environment read by the library: `UC_WEBVIEW_DEVELOPER_EXTRAS=1` (Web Inspector, with
`WEBKIT_INSPECTOR_SERVER=ip:port`), `UC_WEBVIEW_CONSOLE_LOG=1` (page console to stdout, see "What the logs carry"),
`UC_WEBVIEW_ALLOW_FILE_URLS=1` (file: URLs in `load_uri` and as `load_html` base, see above; blocked by default),
`UC_WEBVIEW_DRAG_THRESHOLD=<px>` (1..200, default 16 px, see "Touch drag threshold"), `UC_WEBVIEW_REFRESH_RATE=<Hz>`
(display refresh rate, wins over the host's value, see frame pacing), `UC_WEBVIEW_TRACE=1` (vsync, key and text input
lines, see "What the logs carry"), `UC_WEBVIEW_LOG_FULL_URLS=1` (URIs in log lines with path, query, fragment and
payload), `UC_WEBVIEW_PDF_VIEWER=1` (WebKit's PDF viewer stays on, see "PDF viewer"), `G_MESSAGES_DEBUG=all`;
WPEPlatform's `WPE_DRM_DEVICE`, `WPE_BUFFER_FORMAT`, `WPE_USE_EXPLICIT_SYNC`, `LIBGL_ALWAYS_SOFTWARE`; JavaScriptCore's
`JavaScriptCoreUseJIT=0`.

### Touch drag threshold

WebKit's gesture recogniser turns a touch sequence into a scroll instead of a click once a move is farther than
`WPE_SETTING_DRAG_THRESHOLD` from the press (`WPEGestureDetector.cpp:63-75`; WPE's default is 8 px,
`WPESettings.cpp:106`) and reads the value from the view's display on every move. Since 0.3.0 the library's display
uses **16 px** (`UC_EVENTS_DEFAULT_DRAG_THRESHOLD`, `uc_events.h`); `UC_WEBVIEW_DRAG_THRESHOLD=<px>` (a decimal
1..200) overrides it, another value is logged and ignored. The start line names the value and its source,
`ucwebview: touch drag threshold 16 px (default)` or `(UC_WEBVIEW_DRAG_THRESHOLD)`. Hosts read the effective value
with `uc_webview_global_get_drag_threshold()`: remote-ui tells taps from drags with it for its keyboard request; a
0.2.0 library lacks the function and leaves WPE's 8 px unless the variable is set.

Why 16 px: a remote-ui input trace (Remote 3, 117 one-finger presses into the web view) had 61 taps that moved at most
7.1 px, 16 presses of 8.5..13.9 px that WebKit turned into drags, each a lost click (21 % of the 77 short presses), one
press of 17.5 px and 39 scrolls of 77 px and more. The lost clicks were longer presses (67..360 ms, median 306 ms,
against a median of 102 ms for the taps): the finger rolls while it rests. At 16 px all 16 are clicks and every scroll
stays a scroll, with a factor of 4.8 to the shortest one. On the Remote 3's 480x800 panel of 44x75 mm (about 275 dpi) 16
px are about 1.5 mm, close to Android's touch slop of 8 dp (about 14 px at this density). A scroll starts a little later
but loses no distance: WebKit measures the first scroll delta from the press position (`WPEGestureDetector.cpp:66-75`).

The value belongs to the library rather than to a host: it is a property of the panel and the finger that every host
of the library on the remote shares (remote-ui, `ucwebview-test`, a later Flutter plugin), and WebKit reads it from
the display the library owns. The kiosk launcher (WPE's DRM platform) keeps WPE's 8 px. A touch driver without fuzz
leaves libinput's touchscreen hysteresis off (its margin is half the fuzz, libinput 1.23 `evdev-fallback.c:131-151,
1559-1563`), and Qt's evdev handler filters only with its `filtered` option (`qevdevtouchhandler.cpp:162-192`), so
both read the same raw coordinates.

### Memory with cross-site navigation (known limitation)

One open page stays flat (12 h with a web view open). A view that navigates between
sites grows: the library sets no cache model, so WebKit uses `WEB_BROWSER` (`LegacyGlobalSettings.h:65`, 2.54.2), and a
navigation to a site reuses the web process that holds that site's suspended page (`WebProcessPool.cpp:1292`), so each
site keeps one long-lived process in which older pages stay until memory pressure. In the 12 h soaks with three sites
every 5 min the WPE processes grew 23.5 MB/h (Remote Two) and 44.8 MB/h (Remote 3) and the host 1.70-1.77 MB/h (about
146 kB of heap per navigation), without a plateau. WebKit's own handler in the host releases it: at 90 % system memory
use it drops the back/forward cache, the web process cache and the spare process (`WebProcessPool.cpp:1531-1549`; on a
Remote 3 MemAvailable 353 → 596 MB in 2 s). With `DOCUMENT_VIEWER` set before the process pool exists the host heap
stays flat (+2.5 kB per navigation), which also shows that the frame and EGLImage handling does not leak.

Hosts can release that memory or start a visit with a fresh session with `uc_webview_global_clear_data(CACHE | ALL)`
(0.8.0, see "Site settings (0.8.0)"): clearing the memory cache also drops suspended pages and cached processes.
Choosing the cache model in `uc_webview_global_init()` before the process pool exists (`DOCUMENT_VIEWER` costs the
back/forward cache and the memory and disk cache) is not implemented. An ephemeral session for the whole process is a
host without a data directory (`create_session()`).

## What the logs carry

The library logs through GLib into the journal of its host process (`g_message`/`g_warning`; `g_debug` only with
`G_MESSAGES_DEBUG`). Log excerpts and traces are made to be shared, in bug reports, so the
library never logs what the user types or what an editable element of the page holds, whatever the element's input
purpose (a PIN pad can be a plain text field), and it logs URIs only in part.

`UC_WEBVIEW_TRACE=1` (read once by `uc_webview_global_init()`, the switch of remote-ui's input trace as well) adds
these lines, prefixed `ucwebview: trace <view>:` (`uc_trace.c`; the start line ends with `input trace on`):

| Line | Shows | Never shows |
|------|-------|-------------|
| `key printable down` / `up` | a key that types a character (keypad digits and dead keys included) | its keysym, its hardware code (which names the key on a keyboard) |
| `key special Return down (keysym 0xff0d, code 36)` | a non-character key by name: Return, BackSpace, Tab, Escape, Delete, Insert, the arrows, Home, End, Page_Up/Down, KP_Enter, the modifiers, F1..F12 | |
| `ime focus in, hints 0x81, purpose 8` / `ime focus out` | WebKit's report of an editable element gaining or losing the focus, with its hint bits and purpose number | the element's text, its length, the caret offset |
| `ime surrounding text changed` | that the text around the caret changed | text, length, caret and selection offsets (in a password field the caret offset counts the typed characters) |
| `ime commit`, `ime preedit shown` / `ended`, `ime delete-surrounding`, each optionally `ignored (no focused editable element)` | the host's text input call and whether an element took it | text, length, pre-edit caret, deleted range |
| `vsync ...` | the display link's start, stop and rate every 10 s | |
| `rendered after 2840 ms (done)`, `(done, rendering update after 2831 ms)`, `(done without a new frame, ...)`, `(timeout)`, `(cancelled: navigation)` | one per `uc_webview_when_rendered()` result, with the times since the request (0.5.0) | anything of the page |
| `pinch begin at 240,400`, `pinch end, scale 2.00 at 240,400`, `pinch cancelled, scale 1.42` | a pinch's begin and end, no line per update (0.6.0) | |
| `magnification 1.00 -> 2.00 at 240,400`, then `magnification 2.00 rendered after 2840 ms (done)` | the host's `uc_webview_set_magnification()` and its render time, which the library measures with a rendered request of its own while the trace is on (0.6.0) | anything of the page |
| `magnification changed to 1.00 by a navigation or history step` | a change WebKit made by itself (0.6.0) | the page |
| `request 7: alert` (`confirm`, `prompt`, `beforeunload`, `auth`) | a request passed to the host, by kind and id (0.7.0) | the message, default text, realm, origin |
| `request 7: answered yes` / `no` / `yes with text`, `request 8: answered with credentials` / `without credentials` | the host's answer | the text, the user name, the password |
| `request 7: cancelled (navigation)` (`commit`, `web process terminated`, `webkit`, `load`), `request 7: ended (view freed)` | how a request ended without an answer | |
| `dialog: loop guard ended by user input` | the user's tap or key press ended a tripped dialog loop guard (0.7.2) | which input it was |
| `dialog: alert closed by the dialog loop guard`, `default: beforeunload left (no script_dialog callback)`, `default: confirm closed by WebKit's default (the host navigates away)`, `default: auth continued without credentials (no authenticate callback)`, `default: client certificate continued without credentials` | a decision the library took without the host | the dialog's text |

What stays visible is how many key and text input events happened and when; the length of a typed password can be
counted from the `key printable` or `ime commit` lines, its characters cannot be recovered.

Requests (0.7.0) log a line without the trace where the library acts on its own, with URIs as below and never a
dialog's text, a realm, a user name or a password: `ucwebview: new window <URI> loads into this view` /
`refused: only http and https open in this view` / `refused: a form posted to a new window`,
`ucwebview: the page asked to close the view (window.close())`, `ucwebview: download <URI> (<type>) refused`
(`download in a frame ...`), `ucwebview: <URI> (<type>) not shown, WebKit cannot show this type` (`frame <URI> ...`),
the PDF lines above, `ucwebview: <origin> asks for a login, the host takes none: continuing without credentials`,
`ucwebview: <origin> asks for a client certificate: continuing without one`, and once each time the loop guard trips
`ucwebview: dialog loop guard: the page asked again less than 1000 ms after the answer 3 times in a row; ...` and,
when the user's input ends it (0.7.2), `ucwebview: dialog loop guard ended by user input`. A host
that answers with the wrong function gets a warning naming the id (`uc_webview_request_auth(7) refused: a script dialog
takes uc_webview_request_reply()`).

URIs in log lines (`load_uri ... rejected`, `load failed`, `new window`, `download`, `set_cookies`,
`ucwebview-test`'s `load` lines) go through `uc_uri_policy_loggable()`: since 0.10.1 only the origin stays,
`<scheme>://<host>[:<port>]`, never a user name or password, a path, a query or a fragment (OAuth codes, session ids
and reset tokens travel in all three: `/reset/<token>`, `?code=`, `#access_token=`). The port shows when the URI names
one (`https://host:8443`, also `http://example.com:80`), an IP literal keeps its brackets (`http://[::1]:8080`), a URI
without a host is its scheme with a colon (`about:`, `data:`, `file:`, `javascript:`, `blob:`, `mailto:`), one that does
not parse as well (`http:`), a string without a scheme `(no scheme)`; at most 200 bytes. Examples:

| URI | Log line shows |
|-----|----------------|
| `https://user:pw@nonexistent-host.invalid/secret/path?token=1#frag` | `https://nonexistent-host.invalid` |
| `https://host:8443/a/b?c` | `https://host:8443` |
| `http://[2001:db8::1]/a?b` | `http://[2001:db8::1]` |
| `about:blank`, `data:text/html,<p>...` | `about:`, `data:` |

Up to 0.10.0 the path stayed and a query or fragment became `?...` / `#...`; a device log of a failed load showed
`ucwebview: load failed: https://nonexistent-host.invalid/secret/path?...: Error resolving ...`, since 0.10.1 it is
`ucwebview: load failed: https://nonexistent-host.invalid: Error resolving “nonexistent-host.invalid”: Name or service
not known (g-resolver-error-quark 0)`.

Messages the library logs from WebKit, libsoup, GIO or a page's script (the error of a failed load, `JavaScript failed:
...`, `a cookie was not set: ...`, `clearing website data (...) failed: ...`) go through `uc_uri_policy_scrub_text()`
(0.10.1): every URI in them becomes its loggable form, the rest of the text stays. A URI starts at a scheme followed by
`://` or at `about:`, `blob:`, `data:`, `file:`, `javascript:` or `mailto:`, and right after an opening `“` at any
scheme (libsoup 3.4.4 quotes a redirect target whatever its scheme: `Invalid URI “%s” in Location response header`,
`soup-session.c:1147`; it becomes `Invalid URI “ftp://files.example” in Location response header`). It ends at white
space, a quote or an angle bracket (the payload of a `data:` or `javascript:` URI at white space only), sentence
punctuation after it stays. The other messages WebKit 2.54 gives a failed load carry no URI (`WebErrors.cpp:43-128`:
`Not allowed to use restricted network port`, `The URL can’t be shown`, `Frame load interrupted`, ...), and GIO's
resolver and connect errors name the host only (`Error resolving “www.example.com”: ...`, `Could not connect to
127.0.0.1: Connection refused`), which stays as the origin would. Not scrubbed: a file path without a scheme in GIO's
file errors (`Error opening file /path: No such file or directory`); file: URLs are off in production
("URI scheme allowlist") and such a path names a file of the firmware, not something the user typed. The host's
callbacks (`load_changed`, `load_failed_detail`, `new_window`, `response_refused`) still get the full URI and message;
what the host logs is its own rule.

`UC_WEBVIEW_LOG_FULL_URLS=1` keeps path, query, fragment and payloads, in URIs and inside messages, for debugging on a
test device; credentials stay out.

Site settings (0.8.0) log what was set, never a secret: `ucwebview: 2 origin header(s) for <origin>: <names>` (never the
values), `origin headers for <origin> removed`, `ucwebview: user agent mobile`, the default user agent at start (above),
`setting 2 cookie(s) for <URI>` and `2 cookie(s) set` (a refused cookie by its name and the rule, never a value),
`certificate exception for <host>: the certificate with SHA-256 <fingerprint>` or `any certificate`, the TLS failure
lines of "Certificate exceptions", the repeated loads (0.10.3) `<origin>: load cancelled 2 ms after it started, on the
connection of WebKit's preconnect, ...; loading again`, `... again after 2 repeated loads within 10 s; not loading it
again` and `<origin>: not loaded again, another navigation came first`, `TLS certificate errors ignored for every
host` / `checked`, `dark color scheme`,
`clearing website data: all`, `website data cleared: all`. The web process extension logs only when one of the library's
own scripts throws (0.9.0): `ucwebview extension: ucwebview-device-host-shim.js threw (line N)`, the line and never a
value.

Device state (0.9.0) logs the switch with the state, `ucwebview: device API on (screen on, ac, balanced, 100 %): ...`
and `device API off`, the events `ucwebview: device event suspend` and `device event resume, slept 61234 ms` (only
while the API is on), refused input and ignored members (`set_device_state() refused: not a JSON object`,
`set_device_state(): invalid screen, level ignored`, never the JSON text) and, with `G_MESSAGES_DEBUG`, each state
change (`ucwebview: device state screen off, battery, power-saver, 42 %`). The start line names the hidden page
throttling (`hidden pages throttled`).

Outside these rules: `UC_WEBVIEW_CONSOLE_LOG=1` writes the page's own `console.*` output to stdout, which the journal
keeps, and a page can print anything there, form data included; the switch is for test pages. The Web Inspector
(`UC_WEBVIEW_DEVELOPER_EXTRAS=1`) shows the page live. The library does not log page titles.

`tests/test-trace.c` (`make check`) feeds a password session (hardware keys, keypad digit, dead key, commits, a
pre-edit, a deletion, the masked surrounding text, input after the focus left) through the trace and the IME state
machine with every log line of the process captured, and fails on a line outside the formats above or with a typed
character, a typed key's keysym or its hardware code. Its request session (0.7.0) passes markers as messages, default
texts, prompt answers, realms, user names and passwords and fails when one shows up in a line. `tests/test-uri-policy.c`
covers the URI rules, the loggable form and the scrubbing of messages (0.10.1). The request modes of `ucwebview-test`
use marker strings that a run greps for in the library's lines ("Checks on a device").

## ucwebview-test

```
ucwebview-test --selftest
ucwebview-test [--headless] URL [--out frame.png] [--frames N] [--timeout S] [--size WxH] [--scale F]
               [--resize WxH,WxH] [--loop S] [--egl device|surfaceless|none] [--tap X,Y] [--data-dir DIR]
               [--allow-file-urls] [--refresh-rate HZ] [--plain-new] [-v]
ucwebview-test --html FILE [...]
ucwebview-test --ime-test [--out frame.png]
ucwebview-test --ime-swap-test [--ime-swap-variants cross-site,cross-site-email,same-site] [-v]
ucwebview-test --raf-test S [--refresh-rate HZ]
ucwebview-test [URL | --html FILE] --zoom-test 110,125,150+175+200,100 [--out frame.png]
ucwebview-test [URL | --html FILE] --magnify S[@X,Y] [--out frame.png]
ucwebview-test [URL | --html FILE] --pinch FROM:TO[@X,Y] [--prevent-touch] [--out frame.png]
ucwebview-test --dialog-test [--dialog-answer accept|reject|text:S|never] [--reply-in-callback] [--plain-new]
ucwebview-test --auth-test [--auth-scheme basic|digest] [--auth-answer login|cancel|no|never] [--plain-new]
ucwebview-test --window-test [--plain-new]
ucwebview-test --download-test [--plain-new]
ucwebview-test --link-test [--link-variants still,moves,...] [--link-elements frag,link,...] [--size WxH]
ucwebview-test --site-test [--site-tls-cert FILE] [--data-dir DIR [--site-expect-persisted]] [--site-steps ua,cookies,...]
ucwebview-test --device-test [--timeout S]
ucwebview-test --load-fail-test [--load-fail-tls-cert FILE] [--load-fail-steps dns,refused,...] [--load-fail-tls-hosts N]
```

- Creates an EGLDisplay on an EGL device (`EGL_EXT_platform_device`; prefers the device with a real render node, so
  the Panfrost GPU on the remote and Mesa's software device on a build host), initialises the library with it, loads
  the page in a `480x800` view, and for every frame prints
  `frame N: WxH fourcc=XR24 modifier=0x... planes=1 egl-image t=+ms sha256=...`. The frame is read back through a
  GLES2 FBO (`glEGLImageTargetTexture2DOES` + `glReadPixels`, top row first) or converted from the shared memory
  pixels, the previous frame is released when the next arrives (two frames in flight like a scene graph). After
  `--frames N` the last frame is written as RGBA PNG and the program exits 0.
- `--egl none`: no EGLDisplay, shared memory frames only (pure software path).
- `--resize 400x300,800x480`: resizes after each frame; the printed frame sizes must follow.
- `--tap X,Y`: a touch tap after the first frame (drives WebKit's gesture recogniser).
- `--loop 3600`: soak; prints frame/rendered/released counters and `VmRSS` every 10 s.
- `--allow-file-urls`: sets `UC_WEBVIEW_ALLOW_FILE_URLS=1` for the run. Without it (or the variable) a `file:` URL is
  refused like in production: `load FAILED: file:` and exit 1. `--html FILE` uses the base `file:///` only
  when file: URLs are allowed, else `about:blank`.
- The view is created with `uc_webview_new_sized(..., sizeof(uc_webview_callbacks), ...)` like a host built against this
  header (`view: uc_webview_new_sized(), callbacks 136 bytes`; 88 with 0.6.x); `--plain-new` uses `uc_webview_new()`
  instead (`view: uc_webview_new()`), the path of hosts built against a 0.3 header, which never gets pinch reports or
  requests. All seventeen callbacks are set either way. With `--pinch` the tool then prints `pinch-test: FAILED (no
  single pinch from begin to end)` and exits 1; that is the expected result. Such a view keeps WebKit's own gesture
  controller, which turns the two fingers into a scroll, exactly as before 0.6.0: old hosts keep their two-finger drag
  scroll.
- `--refresh-rate HZ`: passes the rate with `uc_webview_global_set_refresh_rate()` right after
  `uc_webview_global_init()`, like remote-ui; `UC_WEBVIEW_REFRESH_RATE` in the environment still wins. The tool prints
  `display refresh rate: 50.000 Hz` (what `uc_webview_global_get_refresh_rate()` reports) before loading.
- `--raf-test S`: loads an internal `requestAnimationFrame` counter page whose one-second windows alternate between
  "moving" (a box moves on every callback, every callback produces a frame) and "idle" (the callbacks change
  nothing, WebKit composites no frame, `LayerTreeHost.cpp:216-217`, so they run at the bare display link rate). Per
  window the page puts rate, interval standard deviation and the number of intervals longer than 1.5x the median
  into its title (`raf:50.00 sd:0.31 long:0 mode:idle`, from the callbacks' own timestamps) and the console. After a
  second of warm-up the tool collects the windows for S seconds (use 10) without reading frames back, then prints
  `raf-test: idle requestAnimationFrame 50.50/s (median of 5 windows, min..max, interval sd 0.30 ms, 0 long
  interval(s)), moving 50.50/s (median of 5 windows), frames 25.50/s, display refresh rate 50.000 Hz, display link
  50.500 Hz (tolerance 1.01): ok` and exits 0, or `MISMATCH` and exits 1 when the idle median is more than
  max(0.5 Hz, 2 %) off the display link rate (display refresh rate + 1 %; or no idle window came). The idle rate is
  the oracle because it depends only on the display link; the moving rate also depends on how fast the GPU
  composites (`frames` counts the frames of both windows, so it is about half the rate).
- Output follows "What the logs carry": `load` lines show URIs as the library logs them, the origin only
  (`UC_WEBVIEW_LOG_FULL_URLS=1` for the full ones), the message of `load failed` scrubbed the same way; the transcript
  of the request modes (`> load:committed:http://127.0.0.1:41813/download`) adds the path of the tool's own test pages,
  which the steps compare, `ime focus=1 hints=0x81 purpose=8` carries no text or caret offset, `-v` prints `ime
  surrounding text changed` without the text. After `uc_webview_global_init()` the tool prints `display refresh rate:
  50.000 Hz` and `touch drag threshold: 16 px`.
- `--ime-test`: loads an internal page with an `<input>`, taps it, expects `ime_focus(1, ...)`, then does
  `commit("abc")`, `delete_surrounding(-1, 1)`, `preedit("z")`, `preedit("")` and expects the page to report the
  value `ab` through its title; exit 0 on success, 1 otherwise.
- `--ime-swap-test` (0.7.5): the focused element across a navigation ("Text input (IME)"). Pages from an HTTP server
  inside the tool (GIO) on `127.0.0.1` and `localhost` with one ephemeral port: two sites, no network needed. Per
  variant, in a view of its own: page A focuses an input itself (`autofocus`, `ime_focus(1)`: `autofocus`); the view
  loses and regains the focus, 300 ms each (`ime_focus(0)`, then `ime_focus(1)` with the same hints: `valid-replay`);
  the host loads page B (`ime_focus(0)` since the navigation, 500 ms after B loaded: `focus-out`); the view loses and
  regains the focus like remote-ui around a dialog (no `ime_focus(1)`: `no-replay`); B focuses a field by script
  (`ime_focus(1)` within 1.5 s, all with the field's purpose: `new-focus`) and blurs it (`ime_focus(0)`: `blur`).
  Variants: `cross-site` (B on `localhost`, a web process swap; B's text field has the state of A's input, the case
  WebKit reports nothing for), `cross-site-email` (an e-mail field, purpose 6) and `same-site` (B on `127.0.0.1`, the
  control). Output per check `ime-swap-test: cross-site: PASS focus-out: 1 ime_focus(0), 0 ime_focus(1) from the
  navigation to 500 ms after page B loaded`, per variant `ime-swap-test: cross-site: 6 passed, 0 failed`, at the end
  `ime-swap-test: 18 passed, 0 failed: ok` and exit 0, else `FAILED` and exit 1 (2: timeout). With `-v` the library's
  debug lines show the swap (`the focused editable element is gone with its web process`) and each replay it kept back.
  The tool needs only API version 1, so it also runs against an older library: 0.7.4 fails `focus-out`, `no-replay`
  and, in `cross-site`, `new-focus`.
- `--zoom-test LEVELS`: loads URL, `--html FILE` or, without either, an internal long page (2000 paragraphs and a
  table of 300 x 6 cells, built by its own script, no network). One second after the load finished it sets each
  level in percent with `uc_webview_set_zoom()` and waits for `uc_webview_when_rendered()`, then the next after
  500 ms, and prints `zoom-test: 125 %: rendered after 2840 ms (done), 2 frame(s) since the zoom`. A step `a+b+c`
  sets its levels back to back and waits once: the time against single steps shows whether WebKit does the work
  for each level (it does, see "Rendered signal"). At the end `zoom-test: 6 step(s) rendered after 120..2840 ms,
  median 900 ms; 0 timeout(s), 0 cancelled: ok`, exit 0, or `FAILED` and exit 1. Frames are not read back unless
  `--out` is given. The default `--timeout` is 30 s plus 62 s per step.
- `--magnify S[@X,Y]` (0.6.0): loads URL, `--html FILE` or, without either, an internal grid page (1200 x 3000 CSS px,
  a label `x,y` every 200 x 100 px, no links), installs counters in the page (clicks, scroll and touch events, read
  back through the title as `ucstate:<n> click:0 scroll:0 touch:0 scale:1 left:0 top:0 maxleft:720 maxtop:2200`) and
  one second after the load calls `uc_webview_set_magnification(S, X, Y)` (default focal point: the view centre) and
  `uc_webview_when_rendered()`. Prints `magnify: 1.00 -> 2.00 at 240,400: rendered after 840 ms (done), 3 frame(s)
  since`, `magnify: magnification 2.00 (uc_webview_get_magnification), page scale 2.00 (visualViewport)`, the scroll
  position before and after and how far the content under the focal point moved (`kept` within one CSS pixel, or at
  the page edge), then `magnify-test: ok` (exit 0) or `FAILED` (exit 1).
- `--pinch FROM:TO[@X,Y]` (0.6.0): the same page and counters, then two synthesized fingers through
  `uc_webview_touch()` like a host: ids 0 and 1, horizontally FROM px apart around X,Y (default: the view centre),
  25 moves of both fingers over 500 ms to TO px apart, finger 1 lifts, then finger 0. Prints every pinch callback
  (`pinch begin at 240.0,400.0`, `pinch end 2.119 at 240.0,400.0`; updates with `-v`), then `pinch: callbacks: 1 begin
  (at 240,400), 44 update(s), 1 end (scale 2.119 at 240,400), 0 cancel` and what the page counted during the gesture
  (`0 click(s), 0 scroll event(s), 54 touch event(s): no tap or scroll leaked`). Like a host it then sets the
  magnification it had times the pinch's scale at the point the pinch began and reports as `--magnify` does;
  `pinch-test: ok` needs one begin and one end, no leak, the render done and the focal point kept.
  `--prevent-touch`: the page's touch listeners call `preventDefault()` like a map; passes when the library reports
  no pinch, nothing leaked and the magnification stayed.
- Requests (0.7.0), in every mode: the tool prints each request (`requests: request 3: confirm from <origin>, message
  given`, `login for <origin>, basic, realm given, retry`) and answers like a host without a dialog, 50 ms later:
  dialogs no, logins without credentials (`--dialog-answer`, `--auth-answer`, `--credentials USER:PASSWORD` change
  that). A URL the library refuses to show before any page committed (a PDF, an attachment) ends the run:
  `requests: refused document <URI> (application/pdf)`, `load refused: <URI>`, exit 1 (0.6.1: `load FAILED`).
  Texts, answers and credentials are never printed.
- `--dialog-test`, `--auth-test`, `--window-test`, `--download-test` (0.7.0): pages from an HTTP server inside the tool
  (libsoup on 127.0.0.1, an ephemeral port; Basic and Digest auth with the realm `uc-realm`, user `ucuser`, password
  `ucsecret`; no network needed). A run is a list of steps; each acts like a host or a user (a load, a script with a
  user gesture, reload, back, SIGKILL of the web process), answers its requests and compares the callbacks it got
  (printed as `> dialog:alert`, `> auth:basic:retry`, `> cancelled`, `> new-window:loaded:<URI>`,
  `> refused:download:<URI>:<type>`, `> load:committed:<URI>`, `> terminated`) and the page's title with the step's
  expectation, then waits 0.3-1.5 s for callbacks that must not come. It also checks every answer: the wrong function,
  an unknown id and a second answer return -1, `request_cancelled()` names a pending request and a late answer after it
  returns -1, no request callback comes from inside `uc_webview_free()`. Output per step `dialog-test: step 2 ok:
  alert, confirm, prompt`; at the end `dialog-test: ok (14 request callback(s), 0 failed check(s))` and exit 0, else
  `CHECK FAILED in step N: ...expected [...], got [...]` and `FAILED`, exit 1. `--plain-new` runs the steps against
  the library's defaults (no request callbacks, `0 request callback(s)`).
  - `--dialog-test`: alert, confirm, prompt (`--dialog-answer accept` (default): `dialogs alert confirm=true
    prompt=""`; `reject`: `confirm=false prompt=null`; `text:S`: `prompt="S"`), six alerts in a row (four reach the
    host, the loop guard closes the rest; the page counts six), 2 s later an alert the guard still closes, a still tap
    (`uc_webview_touch()` down, up 80 ms later) at 8,8, the page's corner outside its body, then six alerts again
    (since 0.7.2 the tap ended the guard: four reach the host again, then it trips again; with a 0.7.1 library this
    step fails, `got []`), beforeunload on reload (`reject`: stay, no load, then leave on a second reload) and on back,
    and an alert left pending for `uc_webview_free()`: `uc_webview_free() with request 14 pending: no callback` (up to
    0.7.1: 10). `never`: an alert pending at a host load
    (`> cancelled` before the load; the page's following confirm and prompt get the default), one pending when the web
    process is killed (`> cancelled`, `> terminated`), a dialog after the reload, and the pending one at free.
    `--reply-in-callback` answers from inside `script_dialog()` (the library closes the dialog from an idle).
  - `--auth-test`: `login` (default): a wrong password, `> auth:basic:retry`, the right one, title `auth-ok`;
    `cancel` (`uc_webview_request_auth(NULL)`) and `no` (`uc_webview_request_reply(0)`): the server's 401 page,
    title `auth-401`, no FAILED; `never`: the page navigates (WebKit cancels the login), then a host load cancels a
    second one. `--auth-scheme digest` the same with Digest. `--plain-new`: the 401 page.
  - `--window-test`: `window.close()` (`> close`), a `target=_blank` link and `window.open('/page3')` load into the
    view (`> new-window:loaded:...`), `window.open()` of a `data:` URL and a form posted to `target=_blank` are refused
    and the page stays, `if (confirm(...)) window.open(...)` from one gesture opens the window after the answer.
  - `--download-test`: the page's frames with an attachment and a PDF (refused silently), an attachment link, `<a
    download>`, an unknown type, a PDF and a host load of an attachment (`> refused:...`, STARTED and FINISHED without
    FAILED); the page stays.
- `--link-test` (0.6.2, `ucwebview-test-links.c`): taps the elements of a built-in page through `uc_webview_touch()`,
  the path remote-ui's touches take, and checks what each tap did. Written when taps in a host ran a page's `onclick`
  handlers (dialogs came up) but no link navigated and `window.open()`/`window.close()` did nothing, while a still tap
  of this tool on a block link navigated. What it found: the taps were fine; inline text had no height because no
  installed font matched the generic families (see "Fonts"), and the page's
  `onclick="open(...)"`/`onclick="close()"` called `document.open()`/`document.close()`.
  - The page comes from an HTTP server inside the tool (GIO's threaded socket service on 127.0.0.1, port chosen by the
    kernel, no network, no libsoup): white background, a 22 px font with line height 2.2, no touch listeners. Elements:
    `font` (0.6.3, no tap: the page's `sans-serif`, `serif` and `monospace` have an ascent and a descent at 22 px,
    `canvas.measureText().fontBoundingBoxAscent/Descent`, which are the primary font's `intAscent()`/`intDescent()`,
    `CanvasRenderingContext2DBase.cpp:3094-3095`; runs first in every variant, because the taps on `frag`, `link` and
    `label` cannot reach text without height), `frag` (link to `#target`), `link` (inline text link to `/second.html`),
    `block` (block link), `check` (checkbox), `label` (the checkbox's label text), `blank` (`target=_blank` link),
    `open` (button calling `window.open()`), `confirmopen` (button calling `window.open()` when `confirm()` returns
    true: in WebKit the click's user gesture lasts the whole handler, a dialog inside it included,
    `EventHandler.cpp:2573`, `UserGestureIndicator.cpp:182-250`, `LocalDOMWindow.cpp:401-415`; with 0.7.0 the tool
    answers "yes" like remote-ui, focus out, the answer 300 ms later, focus in; 0.6 has no dialog API, WebKit's default
    makes `confirm()` return false), `close` (button calling `window.close()`).
  - Every element gets a fresh document (`http://127.0.0.1:PORT/?e=<element>&v=<variant>`); `close` gets a fresh view,
    because WebKit closes a window that script did not open only while its history has one entry
    (`DOMWindow.cpp:140-143`). The page reports the elements' rects through its title 300 ms after the load; the
    tool taps the centre with slot 0, 300 ms later.
  - The page logs every click it sees (capture listener: target id, `isTrusted`, `navigator.userActivation`, pointer
    type), the hash change, the checkbox change and what `open()` and `close()` returned, and puts its state into
    the title: `uclink:<n> hash=#target checked=0 closed=0 log=click:frag:trusted:active:mouse|hashchange`.
  - Checks: `frag` location.hash is `#target`; `link`, `block` the linked page committed (`load_changed`); `check`,
    `label` the box is checked; `blank`, `open`, `confirmopen` with library 0.7.0 `new_window()` or the page committed
    in the view, with 0.6 "not checkable" once the page saw the click (0.6 has no new-window API and WebKit refuses the
    window without a trace); `close` `window.closed` became true (WebKit marks the page closing before it sends the
    close request, `DOMWindow.cpp:145-152`) or, with 0.7.0, `close_requested()`.
  - Variants, each in a view of its own (`--link-variants`, default all):

    | Variant | Taps | Pinch callback (library's gesture controller) | Request callbacks (0.7.0) | Before the tap |
    |---|---|---|---|---| | `still` | down, up 80 ms later | set | set | | | `moves` | down, 4 moves of 16 ms to 3/4 of
    the drag threshold (12.4 px at 16 px), up | set | set | | | `still-nopinch` | still | not set (WebKit's own
    controller) | set | | | `moves-nopinch` | sliding | not set | set | | | `still-norequests` (0.7.0 only) | still |
    set | not set | | | `focus` | still | set | set | the page loads while the view has no focus (`set_focused(0)`), the
    focus comes 300 ms before the tap, like remote-ui's item after the address field | | `keys` | still | set | set | a
    Down key press and release (keysym `0xff54`, code 116 as remote-ui forwards the remote's DPAD) 300 ms before the tap
    | | `dialog` | sliding | set | set | a tap on an `alert()` button; the host answers 300 ms later and takes the
    view's focus out and back in around it like remote-ui's dialog (`uc_webview_set_focused()`) | | `loop` | sliding |
    set | set | once at the start of the variant, a "6 alerts" button (`alert()` six times in a loop), every dialog that
    reaches the host answered like in `dialog`; with 0.7.0 the library's dialog loop guard closes the last ones. Then
    the elements, each on a fresh page of the same view | | `unfocused` | still | set | set | the view has no focus at
    the tap (`set_focused(0)` before every page); WebKit focuses it itself between the synthesized press and release
    (`WPEWebViewPlatform.cpp:468`) |

    Like remote-ui every view gets `set_scale`, `set_mapped(1)`, `set_focused(1)` and a white background; frames
    are released at once, nothing is read back.
  - Output per element `link-test: moves/link (inline text link to /second.html): PASS: the linked page committed;
    the page logged click:link:trusted:active:mouse`, or `FAIL: /second.html?link did not commit in 2.5 s; the page
    logged nothing (no click reached the page)`; `the server got the request` is added to a failure when it did. The
    font element prints `PASS: sans-serif 23/8, serif 23/8, monospace 25/8 px ascent/descent at 22 px` or `FAIL:
    sans-serif 0/0, ... : a family without ascent has no installed font with a family name in its fontconfig pattern,
    WebKit's primary font is an empty typeface (FontCacheSkia.cpp:180-200) and inline text has no height; ...`, and
    an inline element of such a page fails with `its rect is 36x0 px at 118,24, nothing to tap: inline text has no
    height ...` instead of being tapped. Per variant `link-test: moves: 10 passed, 0 failed, 0 not checkable`, at the
    end `link-test: 99 passed, 0 failed, 1 not checkable: ok` (`still-norequests/confirmopen`: without a
    `script_dialog` callback WebKit answers "no"; library 0.6: 63 passed, 27 not checkable). Exit 0 when nothing
    failed, 1 otherwise, 2 on timeout (default 30 s plus 8 s per element and variant).
  - On a system without the fontconfig aliases of "Fonts" the font element and `frag`, `link`, `label` fail as above
    and the other elements pass; with `FONTCONFIG_FILE` pointing at a configuration that has the aliases and includes
    `/etc/fonts/fonts.conf`, every element passes in every variant.
  - On a build host the test ran against WebKitGTK 2.54.0 on a broadway display through the WebKitGTK harness of
    library 0.7.0 with WPE's tap synthesis (a pointer move, press and release at the press position,
    `WPEWebViewPlatform.cpp:433-473`) emulated by GDK mouse events: 89 passed, 1 not checkable (the harness has no keys:
    `keys` taps without them). That checks the page, the server and the verdicts; WPE's gesture path itself runs only
    on a WPE system.
- `--device-test` (0.9.0, `ucwebview-test-device.c`): the device state API like remote-ui, in a view of its own, with
  pages from `uc_webview_load_html()` (base `http://device-test.invalid/` and another site,
  `http://device-test-other.invalid/`, no network) that record every event of `deviceHost`, its battery and
  `document`. The page reports its snapshot in its title by itself whenever it changes, so that the tool runs no
  script in it (one of WebKit's API would count as a user gesture): the attributes, `data-screen` /
  `data-power-profile`, whether `deviceHost` was there before the page's first script, whether an iframe has it,
  `act:` (`navigator.userActivation.hasBeenActive`), `entry:` (how many internal entry point names are on `window`,
  also `window.webkit`), the events in order. Every page has a `beforeunload` handler that asks "leave this page?";
  WebKit puts that question only to a page the user interacted with, and the tool counts the questions that reach its
  `script_dialog` callback (it answers "leave"). Steps: `injected` (the state set before the load, no event),
  `screen-off`, `same-state` (no event), `suspend` (the state, then `freeze` and `suspend`), `resume` (`resume` on both
  with `sleptMs` 61234, then the new state), `power` (`chargingchange`, `on-ac`, `levelchange`, `chargingTime` 0),
  `invalid` (ignored members, refused JSON and events: return values and no event), `reload` (no question after all
  those deliveries; a new document with the current state), `other-site` (a page of another site in another web
  process: the current state), `other-site-change` (a change reaches it there), `home` (back to the first site, a swap
  again), `throttle` (the page's 100 ms interval counts its runs while the view is unmapped for 3 s: at most 6, about
  30 without the throttling), `crash` (the web process killed with SIGKILL; a state change, a suspend and a
  `uc_webview_when_rendered()` while the page is gone start no web process, the tool counts them, and the request ends
  `CANCELLED` from the main context; the next load starts with that state, without events),
  `off-shown`, `off-reload` (no `deviceHost`, no `getBattery`), `on-shown` (nothing until the next load), `on-reload`
  (the state set while the API was off), and two controls that show the activation checks bite: `control-gesture`
  (`uc_webview_run_javascript()`: `act:1`) and `control-leave` (that page asks once), then `rendered` (a page without
  the API, and `uc_webview_when_rendered()` at its `FINISHED`, as a host waits for a loaded page: `DONE`, `act:0`) and
  `rendered-leave` (leaving it asks nothing, the next page the same). `act:0`, `entry:0` and no question in every
  other step. Output `device-test: PASS <step>` or `FAIL <step>` with the expected and the actual snapshot, at the end
  `device-test: 21 passed, 0 failed: ok` (exit 0), else `FAILED` (1) or a timeout (2, default 120 s).
- `--site-test` (0.8.0, `ucwebview-test-site.c`): the site settings like a host, in a view of its own, against libsoup
  servers inside the tool: `http://127.0.0.1:P` (the site), `http://localhost:P` (another site) and, with
  `--site-tls-cert FILE`, HTTPS on `127.0.0.1`, `localhost` and `127.0.0.2` (a self-signed certificate and its key for
  those names in one PEM file, made with `openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes
  -keyout key.pem -out cert.pem -days 3650 -subj /CN=ucwebview-test -addext
  subjectAltName=DNS:localhost,IP:127.0.0.1,IP:127.0.0.2` and `cat cert.pem key.pem`). No network. The servers record
  per request whether the origin headers, which cookies and which User-Agent arrived; the pages report
  `navigator.userAgent`, `document.cookie` and `prefers-color-scheme` through their title. Steps (`--site-steps`,
  default all): `ua` (the default user agent is printed and must end with the Remote's token, `UCRemote/<version>` from
  `/etc/uc-release` or `UC_OS_VERSION`, else `UCRemote`; then MOBILE, DESKTOP, CUSTOM without the token, and back),
  `cookies` (host-only, httpOnly, path and secure cookies, a refused call sets nothing, an expired cookie deletes; with
  `--data-dir` the persistent `uc_keep` is in `cookies.sqlite`, and `--site-expect-persisted` fails unless the previous
  run's came back), `headers` (the page, a script, an image and a fetch of the origin get them; a cross-origin fetch, a
  CORS fetch of a frame of another site, an image of the origin in another site's page, a form another site posts and
  the hop after a redirect away do not; the origin's own form post does; a main-frame redirect of another site into the
  origin does with process swaps (WPE, and the harness by default) and does not without (`UC_HARNESS_PSON=0`, the tool
  reads the variable); since 0.8.3 an image and a frame of another site's page that load the page's own URL, redirected
  into the origin (they got the headers up to 0.8.2), and a redirect into the origin as the first navigation of a new
  view do not (documented); a no-cors GET of a frame of another site inside the origin's page does, the documented
  limit; since 0.8.1 chains of cross-site redirects over a third site on `127.0.0.2`: origin -> other -> third (neither
  the other site nor the third gets them), other -> origin -> other (only the origin's hop), and an image of the
  origin's page redirected over the other site to the third; another site's page sending `X-Uc-Token` itself keeps its
  own value and loses exactly the configured one, the documented limit; count 0 removes), `tls` (no exception, a wrong
  fingerprint, the right one, any certificate of `localhost`, the "ignore" policy on `127.0.0.2`), `scheme` (light, dark
  at once and on a new page, light again), `clear` (CACHE keeps cookies, ALL removes them, also from the file; with
  `--data-dir` `uc_keep` is set again for the next run). Output `site-test: PASS headers-page: the origin's page: both
  headers, expected both headers`, `SKIP tls: no --site-tls-cert`, at the end `site-test: 59 passed, 0 failed, 0
  skipped: ok` (exit 0, 60 with `--site-expect-persisted` since 0.8.3), else `FAILED` (1) or `TIMEOUT` (2). Between the
  TLS attempts the tool loads an http page of the site (see the host run below). Header values, cookie values and the
  token are never printed.
- `--load-fail-test` (0.10.0, `ucwebview-test-loadfail.c`): `load_failed_detail()` like a host, in a view of its
  own, against a libsoup server inside the tool on `127.0.0.1` and, with `--load-fail-tls-cert FILE` (the PEM file of
  `--site-tls-cert`), an HTTPS server with that self-signed certificate. Steps (`--load-fail-steps`, default all):
  `dns` (`http://load-fail-test.invalid/`: HOST_NOT_FOUND, `g-resolver-error-quark`), `refused` (a closed port on
  `127.0.0.1`: CONNECTION_REFUSED, `g-io-error-quark` 39), `unreachable` (`http://255.255.255.255/`, which Linux
  refuses with ENETUNREACH: NO_NETWORK, 38), `timeout` (`http://240.0.0.1/`, reserved, never answers: TIMEOUT after
  WebKit's 10 s preconnect timeout; needs a default route, without one it is NO_NETWORK and fails), `tls` (the
  self-signed certificate: CERTIFICATE, `g-tls-error-quark` 2, `tls_errors` with `G_TLS_CERTIFICATE_UNKNOWN_CA`; then
  `tls-accepted`: with `uc_webview_global_allow_certificate()` for its fingerprint the page shows without detail or
  FAILED), `scheme` (`ftp://...` and `example.com` in one go: two pairs of SCHEME_REFUSED and FAILED from the idle, in
  order, `""` 0 0 `""`), and loads that must bring neither detail nor FAILED: `cancelled` (a load the server never
  answers, replaced by the next one), `attachment` (a refused response), `http-error` (a 404 page), `old-host` (a
  second view created with the struct size of a 0.9 host: FAILED without detail although the field behind that size
  is set), and `tls-race` (0.10.3, with the certificate): 24 HTTPS loads (`--load-fail-tls-hosts N`, 2 to 48) of new
  hosts `127.0.1.1`, `127.0.1.2`, ... (all of `127.0.0.0/8` is loopback on Linux; an http page of the same host before
  each, so that WebKit keeps the web process), every other one with an exception for the certificate's fingerprint:
  the page must show without detail or FAILED, the others must fail once with CERTIFICATE. The HTTPS server runs in a
  thread of its own and counts the connections of each load; after a load call the tool holds its main thread (the
  UI process) for a while, which delays the web process' request against WebKit's preconnect, longer after a load
  whose request took over the preconnect's connection, shorter after one that opened its own, with a step that halves
  at each turn. That keeps the loads around the point where the request comes right after the preconnect rejected the
  certificate ("Load failures (0.10.0)"): about 1.2 ms on an x86_64 build host, where 5 to 11 of the 24 loads then
  take over a rejected connection, which library 0.10.2 reported as TIMEOUT in every run. The check line counts them:
  `load-fail-test: PASS tls-race: 24 new hosts, ... 0 unexpected; the request took over the preconnect's connection 13
  time(s), opened its own 11 time(s), last hold 1250 us; 9 load(s) repeated after a cancellation`. Each failure must
  bring exactly one detail as the event right before its one FAILED, with the same URI. Output
  `load-fail-test: PASS dns-reason: HOST_NOT_FOUND (g-resolver-error-quark 0, tls 0x0, "Error resolving ...")`, at the
  end `load-fail-test: 22 passed, 0 failed, 0 skipped: ok` (exit 0; without the certificate 17 passed and 2 skipped;
  21 passed up to 0.10.2), else `FAILED` (1) or `TIMEOUT` (2, default 360 s). Without the network: `dns` passes with
  TEMPORARY_FAILURE as well. In every other mode the tool prints `load failed: <uri>: <REASON> (<domain> <code>, tls
  0x<flags>): <message>` before `load FAILED`.
- Exit codes: 0 ok, 1 load failure / a refused URL / web process termination / IME mismatch / a zoom step that did not
  render / a failed magnify, pinch, request, link, site or load failure check, 2 EGL failure or timeout, 3 usage.

The PNG checksum comparison between the host and the device is a visual/perceptual one: the host renders with Skia
on llvmpipe or in shared memory, the device with Skia on Panfrost, so the raw `sha256` values differ; the printed
size/fourcc/modifier and the PNG content are what to compare.

## Host checks

```shell
cd lib
make check                 # 176 GLib unit tests (events and the drag threshold, formats, IME state machine, EGL helpers,
                           # URI allowlist and log redaction, refresh rate parsing/precedence/tick arithmetic, the vsync
                           # thread in real time, the input trace with a password session, the rendered and pinch lines,
                           # the ABI copy of the host's callback struct behind a guard page, the rendered request queue:
                           # done/timeout/cancel, frame rule, requests from callbacks; the pinch tracker: forwarding,
                           # threshold, scale and midpoint, end/cancel/page take-over/navigation/lost release; the focal
                           # point arithmetic, the magnification scripts in the C locale and, when the host has
                           # libjavascriptcoregtk, run in JavaScriptCore; the PDF types, the response rule and
                           # UC_WEBVIEW_PDF_VIEWER, and, when the host has WebKitGTK 2.54, the feature identifier in
                           # WebKit's own list; 0.7.0: the request table and ids, every end path with host callbacks
                           # that answer or navigate, dialog answers, the loop guard, origins, the new-window filter,
                           # attachment and response verdicts, the design decisions, the 0.7.0 struct layout and a
                           # 0.6 host's struct behind a guard page, the request trace with markers; 0.7.2: the guard's
                           # end at the user's input; 0.8.0: origins, header names and values, the header table, its
                           # merge and (de)serialization with generations, the per-request decision with 20 cases, the
                           # value rule of 0.8.1 on a fake of the request's headers with 10 cases (test-headers.c);
                           # 0.8.2: UC_OS_VERSION of /etc/uc-release, the file before the environment, the default
                           # user agent with and without a version (test-site.c); 0.8.3: the review's redirect cases,
                           # the spec's reserved header names and 64-byte limit, the extension's check of the table,
                           # RFC 6265 cookie names and values, the 0600 cookie file (test-headers.c, test-site.c); hosts, fingerprints, the TLS exception table and its limits, cookie
                           # checks, user agent presets, data types (test-site.c); 0.9.0: the device state JSON (valid,
                           # partial, 17 invalid members, 46 refused inputs, the 4096-byte and 16-level limits), the
                           # start script with the state, the changes for the extension and their JSON (13 cases, the
                           # decimal comma locale when the host has one), the compiled-in shim against the file, the
                           # event names and, when the host has WebKitGTK 2.54, both throttling identifiers in
                           # WebKit's list (test-device.c); 0.10.0: the load failure reasons of every GError domain
                           # and code WebKit 2.54 reports, other domains, prefixes and case, the arguments from a
                           # GError (NULL, no message, the TLS flags), the scheme refusal, the reason values and,
                           # when the host has WebKitGTK 2.54, WebKit's network error domain string
                           # (test-load-error.c), the 0.10.0 struct layout and a 0.9 host's struct behind a guard page
                           # (test-abi.c); 0.10.1: the origin-only log form of URIs and URIs inside messages, libsoup's
                           # quoted redirect target, data: payloads (test-uri-policy.c)) + header C89/C++, and with
                           # Node the shim (16 tests,
                           # tests/test-device-shim.mjs)
make check-js              # only the shim in Node's vm with a minimal DOM: one function expression that leaves only
                           # deviceHost on window and returns the update function, defaults, the start state,
                           # read-only attributes, no page code inside the update function (a page that replaced
                           # push, postMessage, call, apply, bind and a setter), deferred delivery and event order,
                           # real changes only, invalid values, charging and chargingTime, suspend/resume order and
                           # sleptMs, handler attributes, no documentElement yet, order across tasks
make check-asan            # ABI copy, rendered queue, pinch tracker, request table, IME, header, site and device tests with
                           # ASan and UBSan, 115 tests
python3 -I tests/mk-host-headers.py --expect-version 2.54.2 <wpewebkit-2.54.2 src> /tmp/wpe-hdr   # or: make -C .. wpe-headers
make syntax-check WPE_HOST_INCLUDE=/tmp/wpe-hdr           # -fsyntax-only of all WPE sources, the web process
                                                          # extension (0.8.0) and the test host
                                                          # (without libsoup-3.0 development files on the host add
                                                          # SOUP_CFLAGS=-I<sysroot>/usr/include/libsoup-3.0)
make syntax-check-negative WPE_HOST_INCLUDE=/tmp/wpe-hdr  # must report "rejected as expected (8 errors)"
make check-wpe-fake WPE_HOST_INCLUDE=/tmp/wpe-hdr         # uc_wpe_screen.c against a fake WPEScreen/WPEScreenSyncObserver, 5 tests
```

`make wpe-headers` in the repository root downloads the release tarball once into `.cache/`, checks its SHA-256 and runs
the generator (`scripts/wpe-host-headers.sh`); `make syntax-check` there runs the three WPE targets of this directory
and the launcher's syntax check against it. Without a WPE WebKit on the build host the library itself is only
syntax-checked, not compiled or run; a run on a WPE system (see "Checks on a device" and [CI level
2](../docs/ci-level-2.md)) is the integration test. The scripts of `uc_webview_set_magnification()` and the PDF viewer
switch were also run against WebKitGTK 2.54.0 on a broadway display (same `WebKitWebView.cpp`, `WebPage.cpp` and
WebCore), see "Pinch zoom" and "PDF viewer". The locale case of `tests/test-magnify.c` is skipped on a host without a
decimal-comma locale; `localedef -i de_DE -f UTF-8 <dir>/de_DE.UTF-8` and `LOCPATH=<dir>` run it. The generated headers
stub libsoup and the JavaScriptCore GLib API; the stubs declare the functions the library calls (`jsc_value_is_number`,
`jsc_value_to_double`, since 0.7.0 `soup_message_headers_get_one`, since 0.8.0 the cookie, header and public suffix
functions, since 0.9.0 the JavaScriptCore functions of the web process extension, copied from the installed 2.54.2
headers). Since 0.8.0 the generator also writes the web process extension API (`webkit-web-process-extension.h`,
`WebKitWebPage.h`, `WebKitWebProcessEnumTypes.h`, ...; `PlatformWPE.cmake:266-296, 363-380`), which `uc_extension.c`
needs; it only reads `Source/WebKit/{UIProcess/API/{glib,wpe},WPEPlatform,WebProcess/InjectedBundle/API/{glib,wpe}}`,
`Source/WebKit/PlatformWPE.cmake` and `Source/cmake/OptionsWPE.cmake` of the tarball.

The request modes of `ucwebview-test` (0.7.0) also run on a build host, against WebKitGTK 2.54 instead of WPE:
`tests/webkitgtk-host.c` implements `uc_webview.h` on `libwebkit2gtk-4.1` with the library's request code
(`uc_requests_wk.c`, `uc_requests.c`, `uc_trace.c`, `uc_pdf_policy.c`, `uc_abi.c`) compiled in unchanged against the
generated headers, calls the same hooks as `uc_webview.c` and closes the dialogs the library leaves to WebKit at once,
as WPE does (WebKitGTK would show its own). Frames, input and pinch are WPEPlatform's and not there. It needs the
WebKitGTK 4.1, GTK 3 and libsoup-3.0 runtime libraries (no development files) and a GTK display; broadway is enough:

```shell
make ucwebview-test-gtk WPE_HOST_INCLUDE=/tmp/wpe-hdr SOUP_CFLAGS=-I<sysroot>/usr/include/libsoup-3.0 SOUP_LIBS=
broadwayd :58 &
GDK_BACKEND=broadway BROADWAY_DISPLAY=:58 ./tests/ucwebview-test-gtk --egl none --dialog-test; echo $?   # "dialog-test: ok", 0
```

Since 0.8.0 the harness also implements the site settings with the library's `uc_site_wk.c`, `uc_site.c` and
`uc_headers.c` and the WebKitGTK 4.1 API's context and data manager (where WPE 2.0 has the `WebKitNetworkSession`),
GTK's `gtk-application-prefer-dark-theme` for the color scheme, and `make ucwebview-test-gtk` builds the web process
extension for WebKitGTK's web process (`tests/gtk-ext/libucwebview-extension.so`, `uc_extension.c` with the 4.1 entry
point `webkit_web_extension_initialize_with_user_data`), which the harness loads. Since 0.8.1 the harness creates its
`WebKitWebContext` with `process-swap-on-cross-site-navigation-enabled` TRUE, so that cross-site navigations and
redirects move to another web process as on WPE (WebKitGTK 4.1's default is FALSE); `UC_HARNESS_PSON=0` runs without
process swaps. So `--site-test` runs on a build host:

```shell
make ucwebview-test-gtk WPE_HOST_INCLUDE=/tmp/wpe-hdr SOUP_CFLAGS=-I<sysroot>/usr/include/libsoup-3.0 SOUP_LIBS=
GDK_BACKEND=broadway BROADWAY_DISPLAY=:58 ./tests/ucwebview-test-gtk --egl none --site-test --site-tls-cert site-test.pem \
    --data-dir /tmp/site-data; echo $?                     # "site-test: 59 passed, 0 failed, 0 skipped: ok", 0
GDK_BACKEND=broadway BROADWAY_DISPLAY=:58 ./tests/ucwebview-test-gtk --egl none --site-test --site-tls-cert site-test.pem \
    --data-dir /tmp/site-data --site-expect-persisted; echo $?   # "cookies-persisted" PASS, "60 passed ... ok", 0
UC_HARNESS_PSON=0 GDK_BACKEND=broadway BROADWAY_DISPLAY=:58 ./tests/ucwebview-test-gtk --egl none --site-test
                                                           # the same without process swaps
```

Since 0.9.0 the harness also runs `--device-test`: it compiles `uc_device_wk.c`, `uc_page_wk.c` and `uc_device.c` in
unchanged, loads the extension built for WebKitGTK 4.1 with `uc_ext_device.c`, and hides the GTK window for
`uc_webview_set_mapped(0)`. WebKitGTK throttles hidden pages by default, so on the host the `throttle` step checks the
tool; `UC_HARNESS_NO_THROTTLE=1` switches that off as a negative control. Since 0.10.0 it also runs
`--load-fail-test` (`uc_load_error.c` compiled in, the TLS flags kept in its `load-failed-with-tls-errors` handler).

```shell
make ucwebview-test-gtk WPE_HOST_INCLUDE=/tmp/wpe-hdr SOUP_CFLAGS=-I<sysroot>/usr/include/libsoup-3.0 SOUP_LIBS=
GDK_BACKEND=broadway BROADWAY_DISPLAY=:58 ./tests/ucwebview-test-gtk --egl none --device-test; echo $?   # "21 passed, 0 failed: ok", 0
UC_HARNESS_NO_THROTTLE=1 GDK_BACKEND=broadway BROADWAY_DISPLAY=:58 ./tests/ucwebview-test-gtk --egl none --device-test   # "FAIL throttle", 1
```

The harness also builds with AddressSanitizer and UBSan. That covers the UI process side (`uc_site_wk.c`, `uc_site.c`,
`uc_headers.c`, `uc_requests_wk.c` and the tool); the extension runs in WebKitGTK's uninstrumented web process and is
built without, its logic (`uc_headers.c`) runs under ASan in `make check-asan`:

```shell
make tests/gtk-ext/libucwebview-extension.so WPE_HOST_INCLUDE=/tmp/wpe-hdr SOUP_CFLAGS=... SOUP_LIBS=
make tests/ucwebview-test-gtk WPE_HOST_INCLUDE=/tmp/wpe-hdr SOUP_CFLAGS=... SOUP_LIBS= \
    CFLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS=-fsanitize=address,undefined
ASAN_OPTIONS=detect_leaks=0 GDK_BACKEND=broadway BROADWAY_DISPLAY=:58 ./tests/ucwebview-test-gtk --egl none --site-test
```


One part runs on the host anyway, the screen and its sync observer (`uc_wpe_screen.c`): `check-wpe-fake` runs them
against `tests/fake-wpe-screen.c`, a fake of the two WPEPlatform classes built on the generated 2.54.1 or 2.54.2 headers
(the two classes are the same in both) that reproduces their start/stop/sync and refresh-rate semantics and reports a
settable release, and plays WebKit's part the way `DisplayVBlankMonitorWPE` does (take the observer, add callback, syncs
from another thread, remove, unref), once as 2.54.1 (adopts) and once as 2.54.2 (takes a reference), and checks that
WebKit's reference is the only extra one and that none is left afterwards, also across the version boundary (2.52.9 to
3.0.0). The generated headers of 2.54.1 and 2.54.2 differ only in the version macros, two comment markers in
`WPEClipboard.h` and one include guard in `webkit.h`; every check above passes with both.

## Checks on a device

On a WPE system with the library installed (expected output and exit status in the comments):

```shell
ucwebview-test --selftest; echo $?                                     # "selftest ok", 0 (checks the 16 px drag threshold, the 0.6.0 callbacks, the 0.7.0 struct and request functions, the 0.8.0 site settings and the 0.9.0 device state functions without a view)
ucwebview-test http://localhost/ --frames 10 --out /tmp/f.png; echo $?  # 10 frame lines, PNG shows the page, 0
ucwebview-test http://localhost/ --resize 400x300,800x480 --frames 4    # frames 2.. carry the new sizes
ucwebview-test --ime-test; echo $?                                      # ime focus=1 ..., "input value is "ab"", 0
ucwebview-test --ime-test --plain-new -v; echo $?                       # "view: uc_webview_new()", the same result and "ime surrounding text changed" (the last API version 1 callback), 0
UC_WEBVIEW_TRACE=1 ucwebview-test --ime-test 2>&1 | grep 'ucwebview: trace'  # ime focus in/commit/delete-surrounding/preedit shown, ended; no text
UC_WEBVIEW_TRACE=1 ucwebview-test --ime-test 2>&1 | grep 'ucwebview: trace' | grep -c -e abc -e '"z"'  # 0
ucwebview-test file:///etc/os-release; echo $?                          # "load_uri file:///etc/os-release rejected: file: URLs are disabled ...", "load FAILED", 1
ucwebview-test --allow-file-urls file:///etc/os-release --frames 2; echo $?  # start line "file: URLs allowed", frames, 0
ucwebview-test http://localhost/ --loop 3600 -v | grep soak             # VmRSS and the released counter stable
ucwebview-test --raf-test 10 --refresh-rate 50; echo $?                 # "idle requestAnimationFrame 50.5x/s ... display refresh rate 50.000 Hz, display link 50.500 Hz ...: ok", 0
ucwebview-test --zoom-test 110,125,150,175,200,250,100 --refresh-rate 50; echo $?   # one "rendered after N ms (done)" line per step, "...: ok", 0
ucwebview-test --zoom-test 110,125+150+175,100 --refresh-rate 50       # the burst step takes about as long as its three levels one by one
UC_WEBVIEW_TRACE=1 ucwebview-test --zoom-test 125,100 2>&1 | grep 'ucwebview: trace.*rendered after'   # the library's lines: "rendered after N ms (done...)"
ucwebview-test --magnify 2@100,700; echo $?                            # "rendered after N ms (done)", magnification 2.00, page scale 2.00, "kept", "magnify-test: ok", 0
ucwebview-test --pinch 100:250@240,400; echo $?                        # 1 begin, N updates, 1 end (scale ~2.1), "no tap or scroll leaked", "pinch-test: ok", 0
ucwebview-test --pinch 300:120; echo $?                                # pinch in (scale ~0.43): magnification 0.43 at the centre, ok, 0
ucwebview-test --pinch 100:250 --prevent-touch; echo $?               # "no pinch reported", "pinch-test: ok", 0
ucwebview-test --pinch 100:250 --plain-new; echo $?                    # uc_webview_new(): no callbacks, "pinch-test: FAILED (no single pinch ...)", 1
UC_WEBVIEW_TRACE=1 ucwebview-test --pinch 100:250 2>&1 | grep -E 'trace.*(pinch|magnification)'   # pinch begin/end, magnification 1.00 -> N, magnification N rendered after M ms (done)
ucwebview-test --link-test; echo $?                                    # one PASS line per element and variant, "link-test: 63 passed, 0 failed, 27 not checkable: ok" (0.6.3; 0.7.1: 99 passed, 1 not checkable), 0
ucwebview-test --link-test --link-variants moves --link-elements link,open,close -v   # one variant, a few elements, with the library's debug lines
ucwebview-test --link-test --link-variants still --link-elements font,link,label; echo $?   # "font ...: PASS: sans-serif 23/8, serif 23/8, monospace 25/8 px ascent/descent at 22 px", link and label PASS, 0 (without the fontconfig aliases of "Fonts": "sans-serif 0/0", "its rect is 36x0 px ...", 1)
fc-match sans-serif; fc-match monospace                                # Poppins-Regular.ttf, SpaceMono-Regular.ttf (0.6.3: the package's fontconfig aliases)
fc-pattern -c sans-serif family | grep -c '"Poppins"'                 # 1: "Poppins" is in the substituted pattern, which is what WebKit's Skia font cache needs (0 without the file; fc-match prints Poppins either way)
ucwebview-test --raf-test 10; echo $?                                   # default 60: idle ~60.6/s, ok, 0 (0.1.0, on WebKit's timer: ~61-62/s)
UC_WEBVIEW_REFRESH_RATE=49.999 ucwebview-test --raf-test 10 --refresh-rate 60  # env wins: "display refresh rate: 49.999 Hz", idle ~50.5/s, ok
ucwebview-test https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf --frames 3; echo $?  # start line ends "PDF viewer off"; "ucwebview: PDF https://www.w3.org/.../dummy.pdf (application/pdf) not shown, the PDF viewer is off", "requests: refused document https://www.w3.org/.../dummy.pdf (application/pdf)", "load refused: ..."; no "load FAILED", no "web process terminated"; 1 (0.6.1: "load FAILED"; 0.6.0: viewer, crash after ~2 s)
# PDF in a frame: www.w3.org forbids framing (CSP frame-ancestors), so this uses pdfobject.com (no CSP/XFO)
echo '<iframe src="https://pdfobject.com/pdf/sample.pdf"></iframe>' >/tmp/pdf-frame.html; ucwebview-test --html /tmp/pdf-frame.html --loop 10; echo $?  # "ucwebview: PDF in a frame https://pdfobject.com/pdf/sample.pdf (application/pdf) not shown, the PDF viewer is off", "soak +10 s: ...", no "web process terminated", 0
UC_WEBVIEW_PDF_VIEWER=1 ucwebview-test https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf --loop 10; echo $?  # start line ends "PDF viewer on (UC_WEBVIEW_PDF_VIEWER)", "load finished", the viewer in the frames; until the crash is fixed "web process terminated", 1
ucwebview-test --dialog-test; echo $?                                   # 12x "step N ok" (0.7.2; 9 up to 0.7.1), "dialog loop guard ended by user input" after "tap at 8,8", "uc_webview_free() with request 14 pending: no callback", "dialog-test: ok (14 request callback(s), 0 failed check(s))", 0
ucwebview-test --dialog-test --dialog-answer reject; echo $?            # 13 steps (beforeunload: stay, no load, then leave), "ok (15 ...)", 0
ucwebview-test --dialog-test --dialog-answer 'text:Grüezi 1'; echo $?   # title: dialogs alert confirm=true prompt="Grüezi 1"; "ok (14 ...)", 0
ucwebview-test --dialog-test --dialog-answer never; echo $?             # "> cancelled" before page2 loads; "killed 1 web process(es)", "> cancelled", "> terminated"; a dialog after the reload; "ok (6 ...)", 0
ucwebview-test --dialog-test --reply-in-callback; echo $?               # answers from inside script_dialog(): "ok (14 ...)", 0, no crash
ucwebview-test --dialog-test --plain-new; echo $?                       # uc_webview_new(): WebKit's defaults, beforeunload leaves; "ok (0 request callback(s), ...)", 0
ucwebview-test --auth-test; echo $?                                     # "> auth:basic", "> auth:basic:retry", title auth-ok; "ok (2 ...)", 0
ucwebview-test --auth-test --auth-scheme digest; echo $?                # the same with "> auth:digest", 0
ucwebview-test --auth-test --auth-answer cancel; echo $?                # the server's 401 page (title auth-401), no "load FAILED"; "ok (1 ...)", 0
ucwebview-test --auth-test --auth-scheme digest --auth-answer no; echo $?   # the same through uc_webview_request_reply(0), 0
ucwebview-test --auth-test --auth-answer never; echo $?                 # a page navigation (WebKit cancels) and a host load (the library cancels): 2x "> cancelled"; "ok (4 ...)", 0
ucwebview-test --auth-test --plain-new; echo $?                         # "ucwebview: http://127.0.0.1:<port> asks for a login, the host takes none: continuing without credentials", title auth-401, 0
ucwebview-test --window-test; echo $?                                   # "> close"; new-window:loaded for target=_blank, window.open and confirm-then-open; new-window:refused for data: and the POST form; "ok (7 ...)", 0
ucwebview-test --window-test --plain-new; echo $?                       # the same loads without reports; "ok (0 ...)", 0
ucwebview-test --download-test; echo $?                                 # 5x "> refused:..." (attachment, <a download>, unknown type, PDF, host load), no "load FAILED", the page stays; "ok (5 ...)", 0
ucwebview-test --download-test --plain-new; echo $?                     # nothing reported, no "load FAILED"; "ok (0 ...)", 0
UC_WEBVIEW_TRACE=1 ucwebview-test --dialog-test 2>&1 | grep 'ucwebview: trace' | grep -E 'request|dialog:|default:'   # "request N: alert" ... "answered yes", "dialog: alert closed by the dialog loop guard", "dialog: loop guard ended by user input", "request 14: ended (view freed)"
for m in "--dialog-test --dialog-answer text:uc-answer" "--dialog-test --dialog-answer never" "--auth-test" "--window-test" "--download-test"; do UC_WEBVIEW_TRACE=1 ucwebview-test $m 2>&1; done | grep 'ucwebview: ' | grep -c -e uc-alert -e uc-confirm -e uc-prompt -e uc-default -e uc-answer -e uc-leave -e ucuser -e ucsecret -e ucwrong -e uc-realm   # 0
journalctl -b | grep -v 'sudo\[' | grep -c -e uc-alert -e uc-prompt -e uc-answer -e ucuser -e ucsecret   # 0; sudo logs each command line it runs, test markers included, so its own lines are excluded
pkill -9 WPEWebProcess   # during a run: "web process terminated" and exit 1 (the library recovers with load_uri)
# site settings (0.8.0); site-test.pem as in "ucwebview-test", copied to the device (a test certificate, not a secret)
ucwebview-test --site-test --site-tls-cert /tmp/site-test.pem --data-dir /tmp/site-data; echo $?   # "site-test: the default user agent is \"Mozilla/5.0 (X11; Linux aarch64) ... Safari/605.1.15 UCRemote/<UC_OS_VERSION>\"", "PASS ua-default-remote", "59 passed, 0 failed, 0 skipped: ok", 0
ucwebview-test --site-test --site-tls-cert /tmp/site-test.pem --data-dir /tmp/site-data --site-expect-persisted; echo $?   # "PASS cookies-persisted", "60 passed ... ok", 0 (0.8.3; 0.8.0: headers-redirect-away FAILED, the leak 0.8.1 closes; 0.8.2: 57)
journalctl -u <host unit> -b | grep 'default user agent'   # "... UCRemote/<the firmware version> (UC_OS_VERSION)": inside a sandboxed unit the version comes from EnvironmentFile=/etc/uc-release
ucwebview-test --site-test --site-steps headers -v 2>&1 | grep -E 'site-test|origin header'   # every headers check PASS; fails at once without /usr/lib/ucwebview/web-process-extensions/libucwebview-extension.so
ucwebview-test --data-dir /tmp/site-data --frames 2 http://localhost/ 2>&1 | grep -E 'cookies in|site settings'   # "cookies in /tmp/site-data/cookies.sqlite", "site settings: web process extension /usr/lib/ucwebview/web-process-extensions, TLS errors checked, ..."
nm -D /usr/lib/ucwebview/web-process-extensions/libucwebview-extension.so | grep ' T '   # webkit_web_process_extension_initialize_with_user_data only
journalctl -u <host unit> -b | grep -c -e 'Bearer' -e '<a configured cookie value>'   # 0: the library logs header names and cookie names, never values
nm -D /usr/lib/libucwebview.so.1 | grep ' T ' | grep -v ' uc_webview_'  # empty
nm -D /usr/lib/libucwebview.so.1 | grep -c ' T uc_webview_'               # 51 (0.9.0 and 0.10.0; 48 in 0.8.x)
# device state for web pages (0.9.0)
ucwebview-test --device-test; echo $?                                  # one PASS line per step, "throttle: N ticks ... while hidden" with N <= 6 (about 30 without the throttling), "device-test: 21 passed, 0 failed: ok", 0
# load failure reasons (0.10.0); site-test.pem as for --site-test
ucwebview-test --load-fail-test --load-fail-tls-cert /tmp/site-test.pem; echo $?   # one PASS line per check, "load-fail-test: 22 passed, 0 failed, 0 skipped: ok", 0 (21 up to 0.10.2)
ucwebview-test --load-fail-test --load-fail-tls-cert /tmp/site-test.pem --load-fail-steps tls-race; echo $?   # "PASS tls-race: ... 0 unexpected; ... N load(s) repeated after a cancellation", 0 (0.10.3; 0.10.2: FAIL, TIMEOUT details)
ucwebview-test http://www.example.invalid/ --frames 1; echo $?          # "load failed: http://www.example.invalid: HOST_NOT_FOUND (g-resolver-error-quark 0, tls 0x0): Error resolving ...", "load FAILED: ...", 1
journalctl -u <host unit> -b | grep 'load failed'                   # "ucwebview: load failed: <uri>: <message> (<domain> <code>)"
journalctl -u <host unit> -b | grep -E 'hidden pages throttled|device API'   # the start line ends "hidden pages throttled"; "device API on (...)" for a web view with device_api
readelf -d /usr/lib/libucwebview.so.1 | grep NEEDED  # WPEWebKit-2.0, gio, gobject, glib, gmodule, soup-3.0, EGL, libc
```

`ucwebview-test` runs as any user that may open the render node (`/dev/dri/renderD128`; no DRM master), so next to a
running host UI. It prints its own output in the C locale: a non-ASCII prompt answer shows as `?` on the console
(`CHARSET=UTF-8` prints it correctly), while the page receives the UTF-8 text. Run as a user without a writable HOME,
Mesa and fontconfig print warnings about their caches (`Failed to create ... for shader cache`, `Fontconfig error: No
writable cache directories`); they are harmless.
