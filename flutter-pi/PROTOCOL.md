# The uc.webview channel protocol

How a Flutter app drives libucwebview's web views through the plugin in this directory: two plain SDK platform
channels and the `Texture` widget, no pub package, no FFI. This document is the contract between the plugin (C, the
host-neutral core in [`core/`](core/)) and the app's Dart code; [`core/ucfl_protocol.h`](core/ucfl_protocol.h) and
[`core/ucfl_host.h`](core/ucfl_host.h) implement it, the golden files in [`tests/fixtures/`](tests/fixtures/) pin
every message down to the byte.

Protocol version: **1**.

## Contents

- [Channels](#channels)
- [Handshake and versioning](#handshake-and-versioning)
- [Threads and order](#threads-and-order)
- [Units and values](#units-and-values)
- [Errors](#errors)
- [Methods](#methods): [process](#process), [view lifecycle](#view-lifecycle), [navigation and page](#navigation-and-page),
  [input](#input), [text input](#text-input), [requests](#requests), [site settings](#site-settings),
  [device state](#device-state)
- [Events](#events)
- [Requests in detail](#requests-in-detail)
- [Frames, hold and pinch zoom](#frames-hold-and-pinch-zoom)
- [Logging and secrets](#logging-and-secrets)
- [Limits](#limits)
- [Golden fixtures](#golden-fixtures)
- [The Qt host and this protocol](#the-qt-host-and-this-protocol)

## Channels

| Channel | Dart type | Codec | Direction |
|---|---|---|---|
| `uc.webview/control` | `MethodChannel` | `StandardMethodCodec` | calls down, one reply each |
| `uc.webview/events` | `EventChannel` | `StandardMethodCodec` | `listen`/`cancel` down, events up |

```dart
const control = MethodChannel('uc.webview/control');
const events = EventChannel('uc.webview/events');
```

Every method takes one argument, a `Map<String, Object?>`, or `null` for a method without arguments. Every event is
a `Map<String, Object?>` with `type` (the event's name) and, for the events of a view, `viewId`.

Frames never cross the channels: each view renders into a flutter-pi texture whose id `create` returns, shown with
`Texture(textureId: id)`.

Without the plugin (another embedder, a flutter-pi without it) every call throws `MissingPluginException`: the app
treats the web view as unavailable.

## Handshake and versioning

`hello` first. Its reply:

| Key | Type | |
|---|---|---|
| `protocol` | `int` | the protocol version of the plugin, `1` |
| `available` | `bool` | web views can be created |
| `reason` | `String?` | why not, `null` when available (a log text, not for users) |
| `library` | `String?` | `uc_webview_library_version()` of the loaded library (`"0.11.0"`, for logs); `null` when none was loaded or the library is older than 0.11.0, which has none |
| `abi` | `int?` | `uc_webview_api_version()` of the loaded library, `null` when none was loaded |
| `features` | `List<String>` | the optional parts the library has, see below |

`hello` loads the library (once per process) but does not initialise it: it is cheap and has no side effects.

**Features.** Within API version 1 the library gains functions from release to release, and the host and the library
update independently (a custom UI on an older or newer firmware). The plugin resolves each optional function and
names what is there; a method that needs a missing feature replies with the error `unsupported`. The rules are the
ones of remote-ui's `WebViewLibrary`:

| Feature | Library | Needs | Methods and events that depend on it |
|---|---|---|---|
| `refreshRate` | 0.2.0 | `uc_webview_global_set_refresh_rate` | `setRefreshRate`; `initialize`'s `refreshRate` |
| `dragThreshold` | 0.3.0 | `uc_webview_global_get_drag_threshold` | `initialize`'s `dragThreshold` is the library's (else the environment's or 8) |
| `newSized` | 0.4.0 | `uc_webview_new_sized` | every callback after API version 1 reaches the library through it |
| `whenRendered` | 0.5.0 | `uc_webview_when_rendered` | `whenRendered` |
| `pinchZoom` | 0.6.0 | `new_sized`, `set_magnification`, `get_magnification`, `when_rendered` | `setMagnification`; events `pinch`, `magnificationChanged`; `getState`'s `magnification` |
| `requests` | 0.7.0 | `new_sized`, `request_reply`, `request_auth` | `replyDialog`, `replyAuth`; events `scriptDialog`, `authenticate`, `requestCancelled`, `closeRequested`, `newWindow`, `responseRefused` |
| `originHeaders` | 0.8.0 | `uc_webview_set_origin_headers` | `setOriginHeaders` |
| `userAgent` | 0.8.0 | `uc_webview_set_user_agent` | `setUserAgent` |
| `cookies` | 0.8.0 | `uc_webview_global_set_cookies` | `setCookies` |
| `allowCertificate` | 0.8.0 | `uc_webview_global_allow_certificate` | `allowCertificate` |
| `tlsErrorsPolicy` | 0.8.0 | `uc_webview_global_set_tls_errors_policy` | `setTlsErrorsPolicy` |
| `colorScheme` | 0.8.0 | `uc_webview_global_set_color_scheme` | `setColorScheme` |
| `clearData` | 0.8.0 | `uc_webview_global_clear_data` | `clearData` |
| `deviceApi` | 0.9.0 | `set_device_api`, `set_device_state`, `device_event` | `setDeviceApi`, `setDeviceState`, `deviceEvent` |

The `loadFailed` event (library 0.10.0) has no feature: no function tells 0.10.0 apart. With an older library it
never comes, and a `loadChanged` `failed` has no reason (show a generic text).

**Versioning rules.**

- These changes keep the protocol version: a new method, a new event type, a new optional argument (absent means
  the old behaviour), a new key in a reply or an event, a new string value of an enumeration (load event, reason,
  scheme, ...), a new feature name.
- So the Dart side ignores event types and keys it does not know, maps unknown enumeration strings to an "unknown"
  case, and treats `MissingPluginException` from a method an older plugin lacks as "not supported".
- The plugin ignores argument keys it does not know.
- Anything else (a removed or renamed method, event or key, a changed type or meaning) bumps `protocol`. A Dart side
  that gets a `protocol` it does not know treats the web view as unavailable.

## Threads and order

- The plugin handles every message on flutter-pi's platform thread, which also iterates GLib's default main context
  for the library (one GLib iteration per platform task, see [README.md](README.md), "How it works"). Library calls
  and callbacks never run concurrently.
- Calls are handled in the order Dart sends them. Most reply before the next call is handled. `whenRendered`,
  `setCookies` and `clearData` reply when the library reports, so their replies can come after the replies of later
  calls.
- Events are sent in the order the library reports them. An event caused by a call (for example `imeFocus` from
  `setFocused`, `requestCancelled` from `loadUri`) is sent before that call's reply. Dart receives the replies and
  the events on its platform message queue in the order they were sent.
- Events are sent only while the event channel has a listener (between `listen` and `cancel`); others are dropped
  and counted (`eventsDropped` in `stats`). Listen before creating views.

## Units and values

- Sizes and positions are **device pixels** of the view, origin top left (`create`'s `width`/`height`, touch and
  pointer positions, `setMagnification`'s focal point, `imeCursorArea`, `pinch`). `pixelRatio` (`create`,
  `setScale`) is the CSS-to-device pixel factor.
- Times (`timeMs`) are milliseconds from any monotonic clock, for double click and gesture timing; absent means now.
  Only the low 32 bits count.
- Doubles accept a Dart `int` as well.
- Strings are UTF-8 and must not contain a NUL character.

## Errors

A failed call replies with a `PlatformException` whose `code` is one of:

| Code | When |
|---|---|
| `badMessage` | the message is no `StandardMethodCodec` method call |
| `badArguments` | an argument is missing, has the wrong type or is out of range; `message` names it |
| `unavailable` | no usable library (`hello`'s `reason` in `message`), or `uc_webview_global_init()` failed |
| `unsupported` | the library lacks the feature the method needs; `message` names it |
| `noView` | `viewId` is unknown or disposed (ids are never reused) |
| `noRequest` | `requestId` is unknown, answered or ended, or of the other kind (a login answered with `replyDialog`) |
| `refused` | the library refused the call (it logs why) |
| `failed` | anything else: no memory, no texture, the library could not create the view |

`message` is a short English text for logs, never shown to users. `details` is `null`.

An unknown method gets the empty reply, which Dart reports as `MissingPluginException`.

## Methods

`→` gives the reply; `null` unless stated. "View methods" take `viewId: int` (from `create`) in addition to the
arguments listed and reply `noView` for an unknown one.

### Process

| Method | Arguments | Reply | Library |
|---|---|---|---|
| `hello` | — | see [Handshake](#handshake-and-versioning) | loads the library |
| `initialize` | `refreshRate: double?` | `{dragThreshold: int, refreshRate: double?}` | `uc_webview_global_set_refresh_rate()` (before the init), `uc_webview_global_init()`, `_get_drag_threshold()`, `_get_refresh_rate()` |
| `setRefreshRate` | `hz: double` (20..240) | | `uc_webview_global_set_refresh_rate()`; `refused` outside the range |
| `setColorScheme` | `scheme: "light" \| "dark"` | | `uc_webview_global_set_color_scheme()` |
| `setTlsErrorsPolicy` | `ignore: bool` | | `uc_webview_global_set_tls_errors_policy()`: `true` accepts every invalid certificate |
| `allowCertificate` | `host: String`, `sha256: String?` | | `uc_webview_global_allow_certificate()`; `sha256` null: any certificate of `host` |
| `setCookies` | `url: String`, `cookies: List<Map>` | `bool` when done | `uc_webview_global_set_cookies()`, see below |
| `clearData` | `scope: "cache" \| "all"` | `bool` when done | `uc_webview_global_clear_data()` |
| `stats` | — | `Map<String, int>` | counters, see below |

- **Initialisation** happens once per process, at `initialize` or at the first `create`, `setCookies` or
  `clearData`. It sets up the GLib iteration and calls `uc_webview_global_init()` with flutter-pi's EGL display and
  `UC_WEBVIEW_DATA_DIR`. The `refreshRate` of `initialize` and `create` goes to the library
  (`uc_webview_global_set_refresh_rate()`) until the first page load was asked for (a `create` with `url`, `loadUri`,
  `loadHtml`, `reload`, `goBack`, `goForward`), before or after the initialisation, whichever call initialised the
  library: WebKit takes its nominal frame rate once, at the first page load of the process (pass
  `View.of(context).display.refreshRate`). Later only `setRefreshRate` passes a rate; it changes the library's tick
  rate at once, not WebKit's nominal rate. `dragThreshold` is the touch distance in view pixels from which WebKit
  scrolls instead of tapping: a host that tells taps from drags itself (the keyboard on a tap into a focused field)
  uses it. `setRefreshRate`, `setColorScheme`, `setTlsErrorsPolicy` and `allowCertificate` may come before the
  initialisation; the library keeps the values.
- **Cookies**: each map has `name: String`, `value: String`, `domain: String?` (null: host-only), `path: String?`
  (null: `/`), `secure: bool?`, `httpOnly: bool?` (default false), `expires: int?` (seconds since 1970 UTC; null: a
  session cookie). At most 64. The reply comes when WebKit's network process has them (`true`) or failed (`false`);
  load the page after it. The library's rules (RFC 6265 names and values, no public suffix) apply: a cookie that
  breaks one makes the call `refused`, and none is set.
- **clearData**: `cache` is the memory and HTTP disk cache, `all` all website data (cookies, storage, caches, ...).
  Logins answered with `replyAuth`, certificate exceptions and origin headers stay. Pages that are shown stay as they
  are; reload them.
- **stats**: `views`, `pendingRequests`, `pendingReplies`, `requestsDefaulted`, `eventsSent`, `eventsDropped` of the
  core and the flutter-pi adapter's frame and GLib figures ([README.md](README.md), "Frame pacing"). For tests and
  diagnostics; names may be added or removed without a protocol change.

### View lifecycle

| Method | Arguments | Reply | Library |
|---|---|---|---|
| `create` | `width: int`, `height: int` (1..16384), `url: String?`, `pixelRatio: double?` (0..16], `refreshRate: double?`, `pinchZoom: bool?` (true), `requests: bool?` (true) | `{viewId: int, textureId: int}` | `uc_webview_new_sized()` (`uc_webview_new()` before 0.4.0), `_set_scale()`, `_load_uri()` |
| `dispose` | view | | `uc_webview_free()` |
| `resize` | view, `width: int`, `height: int` | | `uc_webview_resize()`; the next frame has the new size |
| `setMapped` | view, `mapped: bool` | | `uc_webview_set_mapped()`: `false` hides the page (no rendering, no frames, throttled timers) |
| `setFocused` | view, `focused: bool` | | `uc_webview_set_focused()`: keyboard focus; may send `imeFocus` before the reply |
| `setScale` | view, `scale: double` (0..16] | | `uc_webview_set_scale()` |
| `setBackgroundColor` | view, `color: int` (0xAARRGGBB, `Color.toARGB32()`) | | `uc_webview_set_background_rgba()`: shown where the page is transparent |
| `setFrameHold` | view, `hold: bool` | | none: see [Frame hold](#frames-hold-and-pinch-zoom) |

- A new view is mapped and focused. It renders nothing until it loads something.
- `pinchZoom: false` creates the view without the library's two-finger pinch recognition (two fingers then behave
  as in library 0.5.0); `requests: false` without the request callbacks (the library then decides alone: dialogs
  close at once, logins continue without credentials, see `uc_webview.h`). Both are ignored without the feature.
- `dispose` frees the view at once: its texture goes, a `whenRendered` that waits replies `cancelled` (before the
  dispose's reply), and pending requests end without an event (the app closes their dialogs itself).
- **Unmap every view the app does not show** (`setMapped(false)`): a texture with a new frame makes the engine draw
  a frame even when nothing else changed, and a mapped page keeps animating.

### Navigation and page

| Method | Arguments | Reply | Library |
|---|---|---|---|
| `loadUri` | view, `uri: String` | | `uc_webview_load_uri()`: http, https, about, data (file with `UC_WEBVIEW_ALLOW_FILE_URLS=1`); others fail with `loadFailed` `schemeRefused` and `loadChanged` `failed` |
| `loadHtml` | view, `html: String`, `baseUri: String?` | | `uc_webview_load_html()` |
| `reload` | view | | `uc_webview_reload()` |
| `stop` | view | | `uc_webview_stop()` |
| `goBack` | view | | `uc_webview_go_back()` (only when there is somewhere to go) |
| `goForward` | view | | `uc_webview_go_forward()` |
| `getState` | view | `{uri: String?, title: String?, canGoBack: bool, canGoForward: bool, magnification: double?}` | `uc_webview_get_uri()`, `_get_title()`, `_can_go_back()`, `_can_go_forward()`, `_get_magnification()` (null without `pinchZoom`) |
| `runJavaScript` | view, `script: String` | | `uc_webview_run_javascript()`: result discarded; runs as a user gesture |
| `setZoom` | view, `zoom: double` (0..16] | | `uc_webview_set_zoom()`: page zoom, the page is laid out again; WebKit keeps it across navigations |
| `setMagnification` | view, `scale: double` (0.25..10), `x: double`, `y: double` | | `uc_webview_set_magnification()` (`pinchZoom`); `refused` when the library refuses |
| `whenRendered` | view | `"done" \| "timeout" \| "cancelled"` when the library reports | `uc_webview_when_rendered()` (`whenRendered`); `refused` with 32 pending |

- The navigation calls answer a pending leave-page question first: `loadUri`, `loadHtml`, `reload`, `goBack` and
  `goForward` with "leave", `stop` with "stay" (see [Requests in detail](#requests-in-detail)).
- `whenRendered` replies `done` once the page finished a rendering update that started after the call and a frame
  reached the plugin, `timeout` after 60 s (an unmapped view, a busy page), `cancelled` when the document went away
  or the view was disposed. For a busy indicator while a zoom step re-renders a large page.

### Input

| Method | Arguments | Library |
|---|---|---|
| `touch` | view, `slot: int`, `type: int` (0 down, 1 move, 2 up, 3 cancel), `x: double`, `y: double`, `timeMs: int?` | `uc_webview_touch()` |
| `pointerButton` | view, `button: int` (1 primary, 2 middle, 3 secondary), `pressed: bool`, `x`, `y`, `modifiers: int?`, `timeMs: int?` | `uc_webview_pointer_button()` |
| `pointerMove` | view, `x`, `y`, `modifiers: int?`, `timeMs: int?` | `uc_webview_pointer_move()` |
| `scroll` | view, `dx: double`, `dy: double`, `x`, `y`, `precise: bool?` (false), `timeMs: int?` | `uc_webview_scroll()`: `precise` pixels, else wheel ticks; positive `dy` scrolls towards the top |
| `key` | view, `keysym: int`, `keycode: int?`, `pressed: bool`, `modifiers: int?`, `timeMs: int?` | `uc_webview_key()` |

All reply `null`.

- `slot` is the touch sequence id; WPE's own DRM platform gives WebKit the lowest free slot (0 for one finger), and
  WebKit's gesture controller keys its sequences by it. Ids above 2^31 are taken modulo 2^31. Every point that went
  down must end with up or cancel, also when another widget takes the gesture over: a sequence that never ends blocks
  later taps.
- `keysym` is an XKB keysym (0xff0d Return, 0x61 `a`), `keycode` the evdev code + 8 or absent. The key names of
  the remote's buttons and their keysyms are in `lib/README.md`, "Key names".
- `modifiers` are `UC_WEBVIEW_MOD_*` bits: 1 Control, 2 Shift, 4 Alt, 8 Meta, 16 Caps Lock, 256..4096 buttons 1..5
  held.
- Pass the user's input only: a touch down, a button or key press ends the library's dialog loop guard.

### Text input

| Method | Arguments | Library |
|---|---|---|
| `imeCommit` | view, `text: String` | `uc_webview_ime_commit()`: inserts at the caret |
| `imePreedit` | view, `text: String?`, `cursor: int?` | `uc_webview_ime_preedit()`: composition text; null or "" ends it |
| `imeDeleteSurrounding` | view, `offset: int`, `count: int` | `uc_webview_ime_delete_surrounding()`: UTF-16 code units from the caret (`-1, 1` is a backspace), as Dart's `String` counts |

Valid between `imeFocus` `focused: true` and `focused: false`; ignored otherwise.

### Requests

| Method | Arguments | Library |
|---|---|---|
| `replyDialog` | view, `requestId: int`, `accept: bool`, `text: String?` | `uc_webview_request_reply()` (`requests`) |
| `replyAuth` | view, `requestId: int`, `user: String?`, `password: String?` | `uc_webview_request_auth()` (`requests`); `user` null continues without credentials |

See [Requests in detail](#requests-in-detail).

### Site settings

| Method | Arguments | Library |
|---|---|---|
| `setOriginHeaders` | view, `origin: String`, `headers: Map<String, String>` | `uc_webview_set_origin_headers()` (`originHeaders`): extra request headers for one origin; an empty map removes them; at most 32 |
| `setUserAgent` | view, `preset: "default" \| "mobile" \| "desktop" \| "custom"`, `custom: String?` | `uc_webview_set_user_agent()` (`userAgent`); `custom` is required with `"custom"` |

Both take effect with the next request: set them before `loadUri`. The library's rules for names, values and the
user agent text apply (`uc_webview.h`); a refused call (`refused`) changes nothing.

### Device state

| Method | Arguments | Library |
|---|---|---|
| `setDeviceApi` | view, `enabled: bool` | `uc_webview_set_device_api()` (`deviceApi`): pages loaded afterwards get `navigator.getBattery()` and `window.deviceHost` |
| `setDeviceState` | view, `json: String` | `uc_webview_set_device_state()`: a JSON object with `screen` (`on`, `dim`, `off`), `powerSource` (`ac`, `battery`), `powerProfile` (`power-saver`, `balanced`, `performance`), `level` (0..100); members left out keep their value |
| `deviceEvent` | view, `type: "suspend" \| "resume"`, `sleptMs: int?` | `uc_webview_device_event()` |

`suspend` right before a standby (after the standby's state), `resume` after the wake (before the new state) with the
time slept. See the package README of the library, "Device state for web pages".

### Event channel

| Method | |
|---|---|
| `listen` | events are sent from now on (Dart: the first `receiveBroadcastStream().listen()`); arguments are ignored |
| `cancel` | events are dropped from now on; every pending request gets its default at once, and requests that arrive meanwhile too |

## Events

All on `uc.webview/events`, all with `type` and `viewId: int`.

| `type` | Keys | Library callback |
|---|---|---|
| `loadChanged` | `event: "started" \| "redirected" \| "committed" \| "finished" \| "failed"`, `uri: String?` | `load_changed` |
| `loadFailed` | `uri: String?`, `reason`, `errorDomain: String`, `errorCode: int`, `tlsErrors: int`, `message: String` | `load_failed_detail` (0.10.0), right before `loadChanged` `failed` |
| `progress` | `progress: double` (0..1) | `progress_changed` |
| `titleChanged` | `title: String?` | `title_changed` |
| `historyChanged` | `canGoBack: bool`, `canGoForward: bool` | the plugin reads them after every load event, frame and title change and sends a change |
| `frame` | `width: int`, `height: int`, `zeroCopy: bool` | the first frame, and a frame whose size or kind changed (`zeroCopy` false: shared memory frames uploaded with `glTexImage2D`) |
| `terminated` | `reason: "crashed" \| "exceededMemoryLimit" \| "byApi"` | `web_process_terminated`: the view is blank; `loadUri` or `reload` recover |
| `imeFocus` | `focused: bool`, `hints: int`, `purpose`, `surrounding: String?`, `cursor: int` | `ime_focus` |
| `imeCursorArea` | `x`, `y`, `width`, `height: int` | `ime_cursor_area`: the caret in view pixels |
| `imeSurrounding` | `text: String`, `cursor: int`, `anchor: int` | `ime_surrounding`: text around the caret, character offsets |
| `cursorChanged` | `cursor: String` | `cursor_changed`: a CSS cursor name |
| `pinch` | `phase: "begin" \| "update" \| "end" \| "cancel"`, `scale: double`, `x: double`, `y: double` | `pinch` (`pinchZoom`) |
| `magnificationChanged` | `magnification: double` | `magnification_changed`: a navigation or history step changed it |
| `scriptDialog` | `requestId: int`, `dialogType: "alert" \| "confirm" \| "prompt" \| "beforeUnload"`, `origin: String?`, `message: String?`, `defaultText: String?` | `script_dialog` (`requests`) |
| `authenticate` | `requestId: int`, `origin: String?`, `realm: String?`, `scheme`, `retry: bool`, `proxy: bool` | `authenticate` (`requests`) |
| `requestCancelled` | `requestId: int`, `reason: "page" \| "navigation"` | `request_cancelled`, or the plugin answered a leave-page question |
| `closeRequested` | — | `close_requested`: `window.close()` |
| `newWindow` | `uri: String?`, `loaded: bool` | `new_window`: `true` it loads into this view now, `false` refused |
| `responseRefused` | `uri: String?`, `mimeType: String?`, `reason: "download" \| "unsupported"` | `response_refused`: nothing is saved, the page stays |

Enumeration values:

- `loadFailed` `reason`: `other`, `hostNotFound`, `noNetwork`, `connectionRefused`, `timeout`, `certificate`,
  `schemeRefused`. `tlsErrors` are `GTlsCertificateFlags` bits (with `certificate`), `errorDomain` and `errorCode`
  GLib's, `message` untranslated and technical (it can contain the URL: show it as details only, shortened).
- `imeFocus` `purpose`: `freeForm`, `alpha`, `digits`, `number`, `phone`, `url`, `email`, `name`, `password`, `pin`,
  `terminal`, `search`. `hints` are `UC_WEBVIEW_IME_HINT_*` bits: 1 spellcheck, 2 no spellcheck, 4 word completion,
  8 lowercase, 16 uppercase chars, 32 uppercase words, 64 uppercase sentences, **128 inhibit on-screen keyboard**
  (`inputmode="none"`, or focused without a user gesture), 256 vertical writing, 512 emoji, 1024 no emoji, 2048
  private.
- `authenticate` `scheme`: `default`, `basic`, `digest`, `ntlm` (only these reach the app, see below).
- Every enumeration may get values in later versions; an unknown one arrives as `"unknown"` or as a new string.

## Requests in detail

Script dialogs (`alert()`, `confirm()`, `prompt()`, the "leave this page?" question of `beforeunload`) and logins
(HTTP Basic, Digest, NTLM of a server or proxy) are events with a `requestId`, answered by `replyDialog` or
`replyAuth`. The page waits until then: no script, timer or rendering update of the page runs while a dialog is
open.

- Each request ends exactly once: by the app's answer, or by `requestCancelled` when it went away first (the page
  navigated, the web process ended, WebKit gave up the login). After `requestCancelled` the app closes its dialog
  without answering; an answer to an ended request replies `noRequest`.
- **Answers**: `replyDialog` `accept: true` is "yes" (alert closed, `confirm()` true, `prompt()` returns `text`,
  leave the page), `false` "no" (`confirm()` false, `prompt()` null, stay). `replyAuth` with `user` sends the
  credentials (kept for the process only, never logged); `user` null continues without them and the server's 401
  page loads. Rejected credentials bring a new `authenticate` with `retry: true`.
- **Defaults without the app** (as remote-ui's item does): while nobody listens on the event channel, a dialog is
  answered at once with "no", the leave-page question with "leave", a login without credentials. The same happens to
  every request still pending when the listener cancels (no `requestCancelled` follows: nobody listens), and to a
  request whose event could not be sent (no memory): the page never waits for an answer nobody can give. Each counts
  in `requestsDefaulted`.
- **Leave-page question and navigation**: when the app calls `loadUri`, `loadHtml`, `reload`, `goBack` or
  `goForward` while the question is pending, the plugin answers it "leave" first (the user goes elsewhere), for
  `stop` "stay", and sends `requestCancelled` with `reason: "navigation"`. The library would otherwise answer
  "stay" for every navigation call and the next navigation would ask again.
- **Answered by the plugin, no event**: a dialog type of a newer library ("no"), and logins of a scheme that takes no
  user name: Negotiate (Kerberos), HTML form, server trust and unknown schemes (without credentials).
- **dispose** ends the view's requests without events.
- `origin` of a dialog is the page's main-frame origin (WebKit does not say which frame asked; `null` for
  `about:blank`, `data:`); `message` is the page's text, any length; `message` and `defaultText` are `null` for
  `beforeUnload`: show a fixed text.
- `newWindow`, `closeRequested` and `responseRefused` need no answer.

Requests need the `requests` feature and `create`'s `requests` not false.

## Frames, hold and pinch zoom

Each view has one texture. The plugin binds every DMA-BUF frame's EGLImage to a GL texture when the engine fetches
it (zero copy), uploads shared memory frames, and tells the library a frame was rendered when the engine fetched it
(README.md, "Frame pacing").

**Frame hold** (`setFrameHold`, as remote-ui's `FrameQueue::setHeld()`): while held, the texture keeps its current
frame; new frames are reported rendered at once (WebKit renders on) and the newest waits; when the hold ends it is
shown. The hold exists for pinch zoom:

1. `pinch` `begin`/`update`: the app scales its `Texture` widget around the gesture (a `Transform`), no new frame
   is needed.
2. `pinch` `end`: `setFrameHold(true)`, `setMagnification(scale, x, y)` with the app's limits (remote-ui: 1..4),
   then `whenRendered`.
3. Its reply (`done`, or `timeout`/`cancelled`): the app drops its scaling and calls `setFrameHold(false)` in the same
   frame; the first frame after the hold is sharp at the new magnification. Dropping the scaling and ending the hold
   are separate messages, so the old frame may show unscaled for one display frame.

`pinch` `cancel` ends the gesture without a magnification: drop the scaling (and the hold, if set).

## Logging and secrets

The plugin's log lines (`[uc_webview] ...` on stderr) carry the origin of a URL at most (`scheme://host[:port]`),
never its path, query, fragment or user info, and never header values, cookie values, user names, passwords, typed
text or dialog texts. A URL whose user info is not clearly delimited (an `@` after the first `/`, `?`, `#` or `\`,
as in `https://user:pa/ss@host/`), or whose authority is no `host[:port]`, is logged as `(invalid)`; so is a URL with
an `@` in its path or query. The Dart side keeps the same rule for its own logs.

What is wiped in memory: the plugin's own copies of `replyAuth`'s `password`, `setOriginHeaders`' header values and
`setCookies`' cookie values are overwritten before they are freed (also when a later argument of the call is
invalid). Not wiped: the engine's buffers of the platform messages (flutter-pi frees them), the copies the library
and WebKit keep, and other strings such as `replyAuth`'s `user`, `replyDialog`'s `text` and the text input of
`imeCommit`/`imePreedit`.

## Limits

- The plugin does not call `uc_webview_global_shutdown()` when flutter-pi exits: views are freed, WebKit's helper
  processes exit with the last view, and the process ends right after (as remote-ui).
- Frames are shown without an explicit GPU fence: the library hands over a DMA-BUF frame once WebKit's rendering is
  complete, and the engine samples it as remote-ui's scene graph does (implicit synchronisation of the DMA-BUF).
- A file descriptor GLib polls that epoll refuses (a regular file, `EPERM`) is polled on every GLib iteration (logged
  once); an epoll entry that cannot be removed or changed makes the plugin rebuild its epoll set (`glibEpollRebuilds`
  in `stats`), a few system calls per rebuild.
- Requests of the page reach the app only while it listens on the event channel.

## Golden fixtures

[`tests/fixtures/`](tests/fixtures/) has one file per message (85 files), the bytes as Dart's codec writes them,
and [`manifest.json`](tests/fixtures/manifest.json), which describes each:

```json
{"file": "call-touch.bin", "kind": "call", "channel": "uc.webview/control", "method": "touch",
 "arguments": {"viewId": 1, "slot": 7, "type": 1, "x": 12.5, "y": 700.25, "timeMs": 123456}}
{"file": "reply-create.bin", "kind": "reply", "channel": "uc.webview/control", "method": "create",
 "value": {"viewId": 1, "textureId": 2}}
{"file": "reply-error-no-view.bin", "kind": "error", "channel": "uc.webview/control", "method": "*",
 "code": "noView", "message": "unknown viewId", "details": null}
{"file": "event-progress.bin", "kind": "event", "channel": "uc.webview/events", "event": "progress",
 "value": {"type": "progress", "viewId": 1, "progress": 0.5}}
```

- `kind` `call`: the bytes of `StandardMethodCodec().encodeMethodCall(MethodCall(method, arguments))`.
- `kind` `reply`: `encodeSuccessEnvelope(value)`; `error`: `encodeErrorEnvelope(code: , message: , details: )`;
  `event`: `encodeSuccessEnvelope(value)` (what an `EventChannel` delivers).
- `method` `*`: the reply of any method of that shape.
- JSON numbers with a fraction (`1.0`) are Dart doubles, others ints; map key order is the encoding order.
- File names: `call-<method>[-variant].bin`, `reply-<method or value>[-variant].bin`, `reply-error-<code>.bin`,
  `event-<type>[-variant].bin`.

The C side checks every call fixture parses into the values of the manifest (`tests/test-protocol.c`) and produces
every reply and event fixture byte for byte, from the protocol functions and from the host core driven by a fake
library (`tests/test-host.c`). A Dart contract test does the reverse: for each entry it encodes the value with the
SDK codec and compares the bytes with the file, and decodes the file and compares it with the value. The files are
generated by `tests/fixtures/gen-fixtures.py` (`make -C flutter-pi fixtures`), an encoder independent of both codecs
that was checked against flutter-pi's own (`src/platformchannel.c`).

## The Qt host and this protocol

remote-ui's Qt `WebView` item uses the same library on the GUI thread; this protocol exposes every library function
and callback it uses, so that the Flutter app can do what the item does in Dart. What stays native in the plugin, as
in the item: the frame queue and pacing, the frame hold, the feature detection, the request bookkeeping with its
defaults and the leave-page rule above. What moves to Dart: the load indicator and failure texts, zoom levels and
the zoom-render indicator, the pinch scaling and limits, key names to keysyms, the touch slot allocation and
cancellation, the keyboard rules (tap reopen, inhibit hint), the site settings of a web view entity (when to set
headers, cookies, certificate exceptions, the automatic login with configured credentials), the device state source
and the login's kept user name for a retry.
