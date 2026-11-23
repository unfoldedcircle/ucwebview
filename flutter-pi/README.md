# flutter-pi plugin

A [flutter-pi](https://github.com/ardera/flutter-pi) plugin that shows web views of libucwebview as Flutter textures
and drives them over two platform channels. A Flutter app uses only SDK APIs: a `MethodChannel`, an `EventChannel`
and the `Texture` widget. The plugin is C (MIT), built into the flutter-pi binary; the library is `dlopen()`ed at
run time, so flutter-pi starts and reports "unavailable" on a system without it.

Status: **spike** (protocol 1, a subset). It creates views, shows their frames, forwards touches and keys, maps and
unmaps, and reports load changes, progress, frame size and web process terminations. Not yet: resize, pointer and
scroll, text input, dialogs and authentication requests, site settings, device state, pinch zoom.

## Layout

| Path | What |
|---|---|
| `core/` | Host-neutral C, no flutter-pi or GLib types: the dlopen loader with optional symbols (`ucfl_loader`), the view table (`ucfl_views`), Flutter's StandardMessageCodec at byte level (`ucfl_codec`) and the channel protocol (`ucfl_protocol`). A later GTK runner plugin for the Linux desktop shares it. |
| `flutterpi/` | The flutter-pi adapter: registration (`FLUTTERPI_PLUGIN`), channels, frames to textures (`uc_webview_plugin.c`), and GLib's main context on flutter-pi's event loop (`ucfp_glib_loop.c`). |
| `uc_webview.cmake` | Adds the sources to flutter-pi's `flutterpi_module`; included by flutter-pi's `CMakeLists.txt` when `BUILD_UC_WEBVIEW_PLUGIN` is on. |
| `patches/` | The patch to flutter-pi that adds that option (default off), made against flutter-pi `8b86c52`. |
| `tests/` | Host tests (GLib, libsystemd) and the golden byte files of the protocol (`tests/fixtures/`). |
| `scripts/build-flutter-pi.sh` | Builds flutter-pi with the plugin on the host, as a firmware build does. |

## Protocol

Two plain SDK channels, both with `StandardMethodCodec`; the full list is in
[`core/ucfl_protocol.h`](core/ucfl_protocol.h).

- `uc.webview/control` (`MethodChannel`): `hello`, `create {width, height, url, pixelRatio, refreshRate}` →
  `{viewId, textureId}`, `touch {viewId, slot, type, x, y, timeMs}`, `key {viewId, keysym, keycode, pressed}`,
  `setMapped {viewId, mapped}`, `dispose {viewId}`, `stats`. Sizes and positions are device pixels.
- `uc.webview/events` (`EventChannel`): maps with `type` and `viewId`: `loadChanged`, `progress`, `frame`,
  `terminated`.

`hello` answers `{protocol: 1, available, reason, library, abi, features}`: `available` false with a `reason` when
flutter-pi renders without OpenGL ES or the library is missing, lacks a function of API version 1 or has another API
version; `features` names the optional parts of the library found by `dlsym()` (`refreshRate`, `newSized`,
`whenRendered`, `pinchZoom`, `requests`, `siteSettings`, `deviceApi`, ...). `library` is the library's version string
once a release exports `uc_webview_library_version()`; up to 0.10.3 it is `null`.

Frames never cross the channels: a view's frames go into the flutter-pi texture whose id `create` returns, shown
with `Texture(textureId: id)`.

## How it works

**Threads.** Every libucwebview call and callback runs on flutter-pi's platform thread, which must therefore
iterate GLib's default main context (`uc_webview.h`, "Threading contract"). flutter-pi runs an sd_event loop there;
`ucfp_glib_loop.c` drives GLib's context from it as an external main loop: the context is acquired once, the fds of
each `g_main_context_query()` go into one epoll set with a timerfd for the query's timeout, and that epoll fd is the
only sd_event source (`flutterpi_sd_event_add_io()`). The longest check and dispatch is logged
(`[uc_webview] longest GLib dispatch so far: ...`) and reported by `stats`: the platform thread holds flutter-pi's
event loop lock while it dispatches, so a dispatch longer than a frame delays vsync replies and platform messages.
An idle web view costs no wake-ups.

**Frames.** `frame_ready` pushes an unresolved texture frame (`texture_push_unresolved_frame()`). When the engine
fetches it, on the raster thread with the engine's GL context current, the plugin binds the frame's EGLImage to a new
GL texture (`glEGLImageTargetTexture2DOES`, `GL_TEXTURE_2D` as remote-ui does, `GL_TEXTURE_EXTERNAL_OES` if the
driver refuses that), or uploads a shared memory frame with `glTexImage2D`; zero copy for DMA-BUF frames. The fetch
counts as "rendered" (`uc_webview_frame_rendered()`): flutter-pi has no per-texture "presented" signal, so WebKit
renders its next frame while this one is drawn and flipped. When the engine drops the frame (it fetched the next
one) or flutter-pi drops one the engine never fetched, the plugin releases it (`uc_webview_frame_release()`) and
deletes the texture in a GL context of its own, shared with flutter-pi's. At most two frames are held. The raster
thread reports to the platform thread through an eventfd, never through `flutterpi_post_platform_task()`, which waits
for flutter-pi's event loop lock and so for a running GLib dispatch.

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

The library (`libucwebview.so.1`) and WPE WebKit are run time dependencies of the system image, not of flutter-pi.

## Run time

- The plugin looks for `libucwebview.so.1` like `dlopen()` does; `UC_WEBVIEW_LIBRARY=<path>` loads another file.
- `UC_WEBVIEW_DATA_DIR=<dir>`: website data (cookies, local storage) persist there; without it the session is
  ephemeral. The library's own variables apply as well (`uc_webview_global_init()` in `uc_webview.h`).
- The refresh rate of the display (`create`'s `refreshRate`, from `FlutterView.display.refreshRate`) is passed to
  the library before the first page load of the process.
- Log lines start with `[uc_webview]`; they never contain URLs.

## Tests

On a Linux host with GLib and libsystemd development files:

```shell
make -C flutter-pi check          # codec, protocol, loader (against fake libraries), view table, GLib loop
make -C flutter-pi check-asan     # the same with AddressSanitizer and UBSan
make -C flutter-pi flutter-pi-build FLUTTER_PI_SRC=<flutter-pi checkout at 8b86c52>
```

The golden byte files in `tests/fixtures/` are the messages as Dart's `StandardMethodCodec` writes and reads them;
`tests/fixtures/gen-fixtures.py` regenerates them with an independent encoder of the codec's rules. They were checked
against flutter-pi's own codec (`platch_encode()`/`platch_decode()` of `src/platformchannel.c`). A Dart test of the
app's channel code is to compare its encoding with the same files (not written yet).
