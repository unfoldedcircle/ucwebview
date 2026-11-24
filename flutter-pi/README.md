# flutter-pi plugin

A [flutter-pi](https://github.com/ardera/flutter-pi) plugin that shows web views of libucwebview as Flutter textures
and drives them over two platform channels. A Flutter app uses only SDK APIs: a `MethodChannel`, an `EventChannel`
and the `Texture` widget. The plugin is C (MIT), built into the flutter-pi binary; the library is `dlopen()`ed at
run time, so flutter-pi starts and reports "unavailable" on a system without it.

What it covers: everything of `uc_webview.h` remote-ui's Qt host uses. Views are created, sized, mapped, focused and
disposed; pages load, navigate, zoom and run scripts; touch, pointer, scroll, keys and text input reach the page; the
page's dialogs and logins come as requests the app answers; site settings (request headers, user agent, cookies,
certificate exceptions, colour scheme, website data) and the device state for web pages are methods; frames show as
textures, with a frame hold for pinch zoom. The channel protocol is in **[PROTOCOL.md](PROTOCOL.md)**.

## Layout

| Path | What |
|---|---|
| `core/` | Host-neutral C, no flutter-pi or GLib types. `ucfl_host` is the plugin's logic: it loads and initialises the library, keeps the views, turns calls into library calls and callbacks into events, tracks the page's requests and the replies that wait for the library. Below it: the dlopen loader with optional symbols and features (`ucfl_loader`), the view table (`ucfl_views`), Flutter's StandardMessageCodec at byte level (`ucfl_codec`), the protocol's messages (`ucfl_protocol`), frame pacing (`ucfl_pacing`) and frame dump files (`ucfl_dump`). A GTK runner plugin for the Linux desktop can share it. |
| `flutterpi/` | The flutter-pi adapter: registration (`FLUTTERPI_PLUGIN`), the channels, a texture per view and its frames (`uc_webview_plugin.c`), and GLib's main context on flutter-pi's event loop (`ucfp_glib_loop.c`). |
| `PROTOCOL.md` | The channel protocol: methods, events, errors, features, versioning, threads, the golden fixtures. |
| `uc_webview.cmake` | Adds the sources to flutter-pi's `flutterpi_module`; included by flutter-pi's `CMakeLists.txt` when `BUILD_UC_WEBVIEW_PLUGIN` is on. |
| `patches/` | The patch to flutter-pi that adds that option (default off), made against flutter-pi `8b86c52`. |
| `tests/` | Host tests (GLib, libsystemd), a fake `libucwebview.so` (`fake-ucwebview.c`) and the golden byte files of the protocol with their manifest (`tests/fixtures/`). |
| `scripts/build-flutter-pi.sh` | Builds flutter-pi with the plugin on the host, as a firmware build does. |

## Protocol in short

Two plain SDK channels, both with `StandardMethodCodec` (details, arguments and errors in [PROTOCOL.md](PROTOCOL.md)):

- `uc.webview/control` (`MethodChannel`):
  - process: `hello`, `initialize`, `setRefreshRate`, `setColorScheme`, `setTlsErrorsPolicy`, `allowCertificate`,
    `setCookies`, `clearData`, `stats`;
  - views: `create` (→ `viewId`, `textureId`), `dispose`, `resize`, `setMapped`, `setFocused`, `setScale`,
    `setBackgroundColor`, `setFrameHold`;
  - page: `loadUri`, `loadHtml`, `reload`, `stop`, `goBack`, `goForward`, `getState`, `runJavaScript`, `setZoom`,
    `setMagnification`, `whenRendered`;
  - input: `touch`, `pointerButton`, `pointerMove`, `scroll`, `key`, `imeCommit`, `imePreedit`,
    `imeDeleteSurrounding`;
  - requests: `replyDialog`, `replyAuth`; site: `setOriginHeaders`, `setUserAgent`; device state: `setDeviceApi`,
    `setDeviceState`, `deviceEvent`.
- `uc.webview/events` (`EventChannel`), maps with `type` and `viewId`: `loadChanged`, `loadFailed`, `progress`,
  `titleChanged`, `historyChanged`, `frame`, `terminated`, `imeFocus`, `imeCursorArea`, `imeSurrounding`,
  `cursorChanged`, `pinch`, `magnificationChanged`, `scriptDialog`, `authenticate`, `requestCancelled`,
  `closeRequested`, `newWindow`, `responseRefused`.

`hello` answers `{protocol: 1, available, reason, library, abi, features}`; `features` names the optional parts of
the loaded library (`refreshRate`, `dragThreshold`, `newSized`, `whenRendered`, `pinchZoom`, `requests`,
`originHeaders`, `userAgent`, `cookies`, `allowCertificate`, `tlsErrorsPolicy`, `colorScheme`, `clearData`,
`deviceApi`), and a method whose feature is missing replies `unsupported`.

Frames never cross the channels: a view's frames go into the flutter-pi texture whose id `create` returns, shown
with `Texture(textureId: id)`.

## How it works

**Threads.** Every libucwebview call and callback runs on flutter-pi's platform thread, which must therefore
iterate GLib's default main context (`uc_webview.h`, "Threading contract"). flutter-pi runs an sd_event loop there;
`ucfp_glib_loop.c` drives GLib's context from it as an external main loop: the context is acquired once, and each
GLib iteration runs as one platform task (`flutterpi_post_platform_task()`), first-in first-out with flutter-pi's own
tasks, the begin-frame of each vsync included, so a busy page and flutter-pi take turns and neither starves the other
or input. While GLib has nothing ready, the fds of its last `g_main_context_query()` wait in one epoll set with a
timerfd for the query's timeout, an sd_event io source (`flutterpi_sd_event_add_io()`) that posts the next iteration.
An fd closed while its file lives on elsewhere (a rendering fence) leaves an epoll entry that cannot be removed by its
number: the set is then replaced by a new one (`glibEpollRebuilds` in `stats`). The longest check and dispatch is logged
(`[uc_webview] longest GLib dispatch so far: ...`) and reported by `stats`: the platform thread holds flutter-pi's
event loop lock while it dispatches, so a dispatch longer than a frame delays vsync replies and platform messages.
An idle web view costs no wake-ups.

**Frames.** `frame_ready` pushes an unresolved texture frame (`texture_push_unresolved_frame()`). When the engine
fetches it, on the raster thread with the engine's GL context current, the plugin binds the frame's EGLImage to a new
GL texture (`glEGLImageTargetTexture2DOES`, `GL_TEXTURE_2D` as remote-ui does, `GL_TEXTURE_EXTERNAL_OES` if the
driver refuses that), or uploads a shared memory frame with `glTexImage2D`; zero copy for DMA-BUF frames. When the
engine drops the frame (it fetched the next one) or flutter-pi drops one the engine never fetched, the plugin releases
it (`uc_webview_frame_release()`) and deletes the texture in a GL context of its own, shared with flutter-pi's. The
raster thread reports to the platform thread through an eventfd, never through `flutterpi_post_platform_task()`, which
waits for flutter-pi's event loop lock and so for a running GLib dispatch.

**Frame pacing.** WebKit composites a frame only after the previous one was reported rendered
(`uc_webview_frame_rendered()`), and flutter-pi starts its frames on the display's vblanks: a web frame is shown one
refresh after the vblank of the first engine frame that fetches it, and only if it arrived, and the engine asked for
a frame, before that vblank. When it is reported decides how long WebKit has for the next one
([`core/ucfl_pacing.h`](core/ucfl_pacing.h)):

- `fetch` (the default): a frame is reported when the engine fetches it, shortly after a vblank, and the next one is
  pushed as it arrives. WebKit has one refresh period minus the fetch delay; each frame that takes longer leaves a
  refresh without a new frame. At most two frames are held.
- `ahead` (`UC_WEBVIEW_FRAME_PACING=ahead`): a frame is reported when it is handed to the engine, and the next one
  waits in the plugin until the engine fetched this one. WebKit works one frame ahead and has about two refresh
  periods per frame, while it still composites exactly one frame per fetch, so a page that draws on every
  `requestAnimationFrame` stays locked to the display. Costs one refresh period of latency while frames stream and one
  more buffer (three held: shown, handed over, waiting).

The trade-off is latency against tolerance of slow frames. While WebKit's turnaround (`webTurnaroundAvgUs`) stays
well below a refresh period, both modes get a new frame to every refresh, and `ahead` only adds a refresh period
between a frame's push and its fetch (`pushToFetchAvgUs`). `ahead` helps a page whose turnaround approaches a refresh
period or varies around it (`fetchGaps` rising under `fetch`).

A report after the page flip that shows the frame would come at the vblank before which the next engine frame has to
be asked for, so every web frame would take two refresh periods; flutter-pi has no such signal for plugins anyway.

`stats` reports the pacing next to the counters: `pacingAhead` (1 or 0), `framesHeld` (frames that waited),
`fetchMilliFps` (the fetch rate over the last second times 1000, each fetched frame being shown at the next page flip),
`fetchGaps` (fetch intervals of 1.5 to 4.5 refresh periods: one to three refreshes without a new frame while the
page drew),
`webTurnaroundAvgUs`/`MaxUs` (report to WebKit's next frame) and `pushToFetchAvgUs`/`MaxUs` (frame handed to the
engine to fetched), both since the previous `stats` reply and without pauses longer than 250 ms, and `monotonicMs`
(the time of the reply, for rates between two replies).

The refresh rate the host reports may be the display mode's nominal one, which can be a few per cent above the rate
the panel really flips at. `fetchMilliFps` is measured, not derived from that rate, so it shows the real rate; the
`fetchGaps` bounds lie half a period away from whole periods and stay right for a rate about 10 % off. Compare
`fetchMilliFps` with the rate a page flip test measures, not with the mode's nominal rate.

**Power.** `setMapped(false)` hides the page (`uc_webview_set_mapped()`): no rendering, no frames, no textures to
draw. Marking a texture frame available makes the engine draw a frame even when the app draws nothing, so an app
unmaps every view it does not show.

## Building it into flutter-pi

1. Copy this directory to `src/plugins/uc_webview` of the flutter-pi source tree, and `lib/uc_webview.h` of the
   same ucwebview release to `src/plugins/uc_webview/core/`.
2. Apply `patches/0001-Add-the-uc_webview-plugin-option.patch` (it also applies on top of other flutter-pi patches
   that do not touch the plugin options of `CMakeLists.txt`).
3. Configure with `-DBUILD_UC_WEBVIEW_PLUGIN=ON`. Needs `glib-2.0` and flutter-pi's EGL/OpenGL ES renderer.

The patch only adds the option and an `include()` of `uc_webview.cmake`, so the plugin's file list can change
without a new patch. flutter-pi's internal headers are not a stable API: build against the flutter-pi commit the
plugin was tested with (`8b86c52`).

In a Buildroot package for flutter-pi this is a configuration option that adds `libglib2` to the dependencies,
`-DBUILD_UC_WEBVIEW_PLUGIN=ON` to the CMake options, the patch to the package's patches, and a post-patch hook that
copies the plugin from the extracted ucwebview source, for example (the hook's lines start with a tab in a `.mk`):

```make
ifeq ($(BR2_PACKAGE_FLUTTER_PI_UC_WEBVIEW_PLUGIN),y)
FLUTTER_PI_DEPENDENCIES += libglib2
# the ucwebview source tree is extracted (not built) before flutter-pi is patched
FLUTTER_PI_PATCH_DEPENDENCIES += uc-webview
FLUTTER_PI_CONF_OPTS += -DBUILD_UC_WEBVIEW_PLUGIN=ON
define FLUTTER_PI_COPY_UC_WEBVIEW_PLUGIN
    rm -rf $(@D)/src/plugins/uc_webview
    cp -a $(UC_WEBVIEW_DIR)/flutter-pi $(@D)/src/plugins/uc_webview
    cp $(UC_WEBVIEW_DIR)/lib/uc_webview.h $(@D)/src/plugins/uc_webview/core/
endef
FLUTTER_PI_POST_PATCH_HOOKS += FLUTTER_PI_COPY_UC_WEBVIEW_PLUGIN
else
FLUTTER_PI_CONF_OPTS += -DBUILD_UC_WEBVIEW_PLUGIN=OFF
endif
```

The library (`libucwebview.so.1`) and WPE WebKit are run time dependencies of the system image, not of flutter-pi:
the plugin needs only `uc_webview.h` at build time and `dlopen()`s `libucwebview.so.1` on the first call that needs
it. Use the header of the ucwebview release whose library the image ships, or an older one: the plugin resolves
every function added after the first release of API version 1 at run time and reports what it found (`hello`'s
`features`), so a newer library works with an older plugin and the other way round.

The service that runs flutter-pi needs what the library needs in a sandbox: WebKit's JavaScript JIT writes executable
memory (no systemd `MemoryDenyWriteExecute=`, or `JavaScriptCoreUseJIT=0` in the environment, see
`uc_webview_global_init()` in `uc_webview.h`), WPE WebKit's helper processes and data (under `/usr/libexec`, `/usr/lib`
and `/usr/share`) must be readable, and `UC_WEBVIEW_DATA_DIR` writable.

## Run time

- The plugin looks for `libucwebview.so.1` like `dlopen()` does; `UC_WEBVIEW_LIBRARY=<path>` loads another file.
- `UC_WEBVIEW_DATA_DIR=<dir>`: website data (cookies, local storage) persist there; without it the session is
  ephemeral. The library's own variables apply as well (`uc_webview_global_init()` in `uc_webview.h`).
- The refresh rate of the display (`initialize`'s or the first `create`'s `refreshRate`, from
  `FlutterView.display.refreshRate`) is passed to the library before the first page load of the process.
- `UC_WEBVIEW_DRAG_THRESHOLD=<px>` (1..200) is read by the library (0.3.0 and later) and by the plugin for an older
  library (`initialize`'s `dragThreshold`; 8 px without it).
- Log lines start with `[uc_webview]`; they carry a URL's origin at most, never its path or query, and never header
  values, cookie values, passwords or page texts (PROTOCOL.md, "Logging and secrets").
- `UC_WEBVIEW_DUMP_DIR=<dir>`: frame dumps, see below.
- `UC_WEBVIEW_FRAME_PACING=fetch|ahead`: see "Frame pacing" above (default `fetch`).

## Stats

The `stats` method returns counters as a map of ints, for tests and diagnostics (a debug overlay, a device check
script through the app). Names may come and go without a protocol change.

| Name | What |
|---|---|
| `views` | views not disposed |
| `pendingRequests`, `pendingReplies` | page requests waiting for the app's answer; `whenRendered`/`setCookies`/`clearData` replies waiting for the library |
| `requestsDefaulted` | requests answered with the default because nobody listened on the event channel |
| `eventsSent`, `eventsDropped` | events sent; events dropped without a listener |
| `framesReady`, `framesFetched`, `framesReleased`, `framesHeld` | frames from the library; fetched by the engine; given back; that waited ("ahead" pacing) |
| `pacingAhead` | 1 with `UC_WEBVIEW_FRAME_PACING=ahead` |
| `fetchMilliFps`, `fetchGaps`, `webTurnaroundAvgUs`/`MaxUs`, `pushToFetchAvgUs`/`MaxUs`, `monotonicMs` | see "Frame pacing" |
| `glibDispatches`, `glibMaxDispatchUs`, `glibTotalDispatchUs`, `glibSlowDispatches`, `glibEpollRebuilds` | GLib iterations on the platform thread, see "Threads" |

A stuck dialog shows as `pendingRequests` that do not go down, a view the app forgot as `views` that only grow, an
event listener that went away as `eventsDropped` that rise.

## Frame dumps

A test aid to check the orientation and the colours of the web frames on a device without anyone looking at its
screen. It shows the frame as the library hands it over, independent of Flutter's scene (a screenshot the app takes
of itself shows what Flutter draws, the texture included).

With `UC_WEBVIEW_DUMP_DIR=<dir>` in flutter-pi's environment:

- The directory is created (one level, mode 0700: frames show page content) when it is missing.
- The first frame of the process is written right away: `<dir>/frame-1.ppm`.
- `touch <dir>/dump` writes the newest frame of each view as the next `<dir>/frame-<n>.ppm` within 500 ms and
  deletes the trigger file. This works for a page that renders nothing new: the newest frame is kept until the next
  one has been shown. `n` counts from 1 per run, so a new run overwrites the files of the previous one.
- Each file is logged: `[uc_webview] frame dumped: <dir>/frame-<n>.ppm <width>x<height> (view <id>, <source>)`, the
  source being `DMA-BUF <fourcc> modifier <modifier>, read as a GL texture` (or `renderbuffer`) or
  `shared memory AR24`. A file appears complete or not at all (written to `.tmp`, then renamed).

The files are binary PPM (P6, 8 bits per channel, no alpha): no library needed on the device. The first row of the
file is the first row of the buffer, which is the top row as the frame is shown (Flutter's external textures have a
top-left origin, and WebKit's frames have their first row at the top): **a page that is upside down in the file is
upside down in the buffer**, and one that is upright in the file but not on the screen is turned by what draws it.
DMA-BUF frames are read through OpenGL ES (the frame's EGLImage bound to a texture, as for drawing, attached to a
framebuffer, `glReadPixels()` as RGBA), so the colours are those the GPU samples with the format the frame was
imported with; shared memory frames are taken from their pixels.

The dump runs on the platform thread in a GL context of the plugin's own, not on the raster thread and not in the
engine's context; it blocks the platform thread for the read-back and the file write (not measured on a device yet).
With the variable set, the platform thread wakes every 500 ms to look for the trigger; without it nothing changes.

Pull and view the files on a computer:

```shell
scp root@<device>:<dir>/frame-1.ppm .
convert frame-1.ppm frame-1.png        # ImageMagick
python3 -c 'from PIL import Image; Image.open("frame-1.ppm").save("frame-1.png")'
```

The directory must be the same file system for the shell that touches the trigger and for flutter-pi: a service
with `PrivateTmp=` has a `/tmp` of its own, and a sandbox that makes the file system read-only needs the directory
in its writable paths.

## Tests

On a Linux host with GLib and libsystemd development files:

```shell
make -C flutter-pi check          # codec, protocol, loader, views, dumps, pacing, host core, GLib loop
make -C flutter-pi check-asan     # the same with AddressSanitizer and UBSan
make -C flutter-pi fixtures       # regenerates tests/fixtures/ (python3)
make -C flutter-pi flutter-pi-build FLUTTER_PI_SRC=<flutter-pi checkout at 8b86c52>
```

The host core is tested against `tests/fake-ucwebview.c`, a stand-in for `libucwebview.so.1` with every function of
`uc_webview.h` that logs its calls and lets a test play the library's part (the callbacks of each view, the
`when_rendered` and done requests, the pending page requests); variants lack the optional functions, a required one,
part of a feature, or have another API version. `tests/test-host.c` covers availability, initialisation, the view
lifecycle, every method's library call, every event, the requests and their defaults, the replies that wait for the
library, missing optional functions, dispose with pending requests and replies, teardown, and that no secret reaches
the log. flutter-pi itself is not part of the host tests: `flutter-pi-build` compiles the adapter against the pinned
flutter-pi (CI does both).

The golden byte files in `tests/fixtures/` are the messages as Dart's `StandardMethodCodec` writes and reads them;
`tests/fixtures/gen-fixtures.py` regenerates them with an independent encoder of the codec's rules. They were checked
against flutter-pi's own codec (`platch_encode()`/`platch_decode()` of `src/platformchannel.c`).
`tests/fixtures/manifest.json` describes each file (PROTOCOL.md, "Golden fixtures") for the contract test of the
app's channel code, which encodes and decodes the same files with the SDK codec.
