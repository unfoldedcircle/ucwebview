# wpe-launcher: a WPE WebKit kiosk on WPEPlatform DRM

A fullscreen WPE WebKit 2.54 browser for an HTML kiosk UI that runs directly on DRM/KMS through the WPEPlatform DRM
module (`WPE_PLATFORM=drm`), without a Wayland compositor. C11, GLib/GIO, about 700 lines plus host tests. It needs
WPE WebKit 2.54 built with the WPEPlatform API and its DRM platform (and the headless platform for the smoke test).

Unlike [libucwebview](../lib/README.md), which runs a web view inside another application, the launcher owns the
display: it is the only DRM master, takes input from libinput, and draws the page fullscreen.

The system integration (systemd unit, D-Bus policy, environment files) belongs to the system image; "Integration"
below lists what it needs.

## Command line

```
wpe-launcher [--headless] [--timeout SEC] [--no-dbus] [--version] [URL]
```

- `URL` overrides `KIOSK_URL`; a bare path becomes a `file://` URI.
- `--headless`: sets `WPE_PLATFORM=headless`, loads the URL once and exits **0** on `load-changed` FINISHED,
  **1** on `load-failed`, a web process termination or after `--timeout` seconds (default 30). A smoke test of the
  WebKit installation; it implies `--no-dbus`.
- `--version`: `wpe-launcher <version> (WPE WebKit 2.54.2, WPEPlatform 2.54.2)` (the versions at run time), exit 0.
- Exit codes: 0 normal (SIGTERM/SIGINT, D-Bus `Quit`, headless success), 1 runtime failure (no display, display
  disconnected, headless failure), 2 bad command line.

## Environment

The launcher reads these keys (`1`, `true`, `yes`, `on` are true); a systemd unit typically loads them from an
`EnvironmentFile=`:

| Key | Default | Meaning |
|-----|---------|---------|
| `WPE_PLATFORM` | unset | WPEPlatform module (`drm`, `headless`); a kiosk unit sets `drm`. The process **aborts** if the named module is missing or fails to connect (`WPEDisplay.cpp:302-311`); unset, the first module that connects wins |
| `KIOSK_URL` | `http://localhost/` | start page |
| `WPE_LAUNCHER_DEVELOPER_EXTRAS` | `0` | `WebKitSettings:enable-developer-extras` (Web Inspector) |
| `WPE_LAUNCHER_CONSOLE_LOG` | unset | `enable-write-console-messages-to-stdout`: page console to stdout (the journal) |
| `WPE_LAUNCHER_BG_COLOR` | `black` | `webkit_web_view_set_background_color()` before the first paint (`webkit_color_parse` syntax) |
| `WPE_LAUNCHER_DATA_DIR` | `$XDG_DATA_HOME/wpe-launcher` | `webkit_network_session_new(data, data/cache)`; falls back to an ephemeral session with a warning if the directory cannot be created |
| `WEBKIT_INSPECTOR_SERVER` | unset | passed through to WebKit, e.g. `0.0.0.0:9222`; needs developer extras. Open `inspector://<ip>:9222` in GNOME Web |
| `G_MESSAGES_DEBUG` | unset | `all` for `g_debug()` output; normal operation logs at `g_message` level |
| `WPE_DRM_SCALE` | unset | DRM screen scale factor 0.05..20 (`WPEDisplayDRM.cpp:382-390`) |
| `WPE_DRM_DISABLE_ATOMIC` | unset | any value: legacy KMS instead of atomic (`WPEDisplayDRM.cpp:214`); also turns explicit sync off |
| `WPE_DRM_DEVICE` | unset | render device handed to the web process (`WPEDisplay.cpp:699-706`); the KMS node is always auto-detected |
| `WPE_BUFFER_FORMAT` | unset | `FOURCC:modifier-hex[:rendering\|mapping\|scanout]` forces the web process buffer format (`WPEToplevel.cpp:622-645`), see [Buffer formats](#buffer-formats-and-afbc) |
| `WPE_USE_EXPLICIT_SYNC` | unset | `0` disables explicit sync (`WPEDisplay.cpp:725-727`) |
| `JavaScriptCoreUseJIT` | unset | `0` turns JavaScriptCore's JIT off; leave it unset unless the kernel enforces W^X for the process (systemd's `MemoryDenyWriteExecute=` would need it) |

## Behaviour

- Display: `wpe_display_get_default()` (borrowed, already connected), `WebKitWebView` via `g_object_new()` with the
  construct-only `display` property (WPE's tutorial browser, `WebKitWebView.cpp:1370`), `WebKitNetworkSession` for
  website data, toplevel title `wpe-launcher`, `wpe_toplevel_fullscreen()` requested (the DRM toplevel is always
  the whole screen and has no fullscreen vfunc, `WPEToplevelDRM.cpp:70-73`, so the request returning FALSE is
  normal there).
- Startup log: display type, WPE versions, screen size/scale/refresh, render device, toplevel size, data dir, every
  `load-changed` step with the URL. The DRM module picks the KMS device by udev enumeration of `card*` nodes with
  CRTCs/connectors/encoders on `seat0` (`WPEDisplayDRM.cpp:130-178`), for example `card1` (rockchip-drm, `DSI-1`) on
  an RK3566; the render device reported to the web process is the first device with a render node
  (`card0`/`renderD128`, panfrost there; `WPEDisplayDRM.cpp:180-205, 371-372, 486-490`). It becomes DRM master with
  `drmSetMaster()` (`WPEDisplayDRM.cpp:330`); as the first and only opener of the KMS node an unprivileged process is
  master implicitly and may call SET_MASTER without `CAP_SYS_ADMIN` (kernel 6.6 `drm_auth.c:162` sets `was_master`,
  `drm_auth.c:238` honours it).
- Input: libinput over udev on `seat0` (`WPEDRMSeat.cpp:62-66`), devices opened `O_RDWR` (libinput 1.23
  `evdev.c:2407`). Touch, keypads (a libinput keyboard) and USB keyboards are delivered to the page. No input method.
- Kiosk resilience: `web-process-terminated` and `load-failed` (except `WEBKIT_NETWORK_ERROR_CANCELLED`) schedule
  a reload of the last requested URL with exponential back-off 1, 2, 4, ... 30 s, reset by a successful load;
  `window.close()` returns to `KIOSK_URL`; display `disconnected` exits 1 (the service manager restarts it);
  SIGTERM/SIGINT exit 0.
- D-Bus: name `org.unfoldedcircle.WebKiosk`, object `/org/unfoldedcircle/WebKiosk`, interface
  `org.unfoldedcircle.WebKiosk` with `Open(s uri)`, `Reload()`, `GetUri() -> s`, `Quit()` on the system bus.
  Optional at run time: if the bus is unreachable or the name cannot be owned, a warning is logged and the browser
  runs without it.

  ```shell busctl call org.unfoldedcircle.WebKiosk /org/unfoldedcircle/WebKiosk org.unfoldedcircle.WebKiosk Open s
  https://wpewebkit.org/ busctl call org.unfoldedcircle.WebKiosk /org/unfoldedcircle/WebKiosk
  org.unfoldedcircle.WebKiosk GetUri busctl call org.unfoldedcircle.WebKiosk /org/unfoldedcircle/WebKiosk
  org.unfoldedcircle.WebKiosk Reload busctl introspect org.unfoldedcircle.WebKiosk /org/unfoldedcircle/WebKiosk ```

## Integration

What a system image provides around the binary:

- A service user with access to the KMS node and the input devices (systemd: `DevicePolicy=closed` with
  `DeviceAllow=char-drm rw` and `char-input rw`), and nothing else holding DRM master on the KMS node (a second UI
  must be stopped first, e.g. with `Conflicts=`).
- `/run/udev` readable for libinput's seat assignment.
- A writable data directory (`WPE_LAUNCHER_DATA_DIR`, or `HOME`/`XDG_DATA_HOME`) and a writable cache home.
- The XKB data (see below) and, for the smoke test, WebKit's headless platform.
- A D-Bus system bus policy that lets the service user own `org.unfoldedcircle.WebKiosk` and lets the intended
  callers call it; without one the launcher runs without its control interface.

## Buffer formats and AFBC

A Weston + Cog stack on an RK3566 made the kernel log thousands of
`rockchip-drm display-subsystem: [drm] *ERROR* Unsupported format modifier 0x800000000000351` (ARM AFBC
16x16|YTR|SPARSE|TILED|SC chosen by Mesa). How the WPEPlatform DRM module negotiates formats in 2.54:

1. The module reads the **primary plane's** `IN_FORMATS` blob (`WPEDRM.cpp:132-162`; without
   `DRM_CAP_ADDFB2_MODIFIERS` only `LINEAR`, `WPEDRM.cpp:169-170`) and publishes exactly those format/modifier pairs
   as the preferred buffer formats with usage `SCANOUT` (`WPEDisplayDRM.cpp:461-471`).
2. The web process intersects that list with the EGL `EXT_image_dma_buf_import_modifiers` list of the render device
   and allocates with `gbm_bo_create_with_modifiers2()` on the render device (`AcceleratedSurface.cpp:280-298,
   670-700`). Mesa 24.0.9 panfrost picks the best modifier **from the supplied list only**
   (`pan_resource.c:775-785`), so it can never hand the display controller a modifier the plane did not advertise.
3. The launcher process imports the dma-buf into the KMS node's GBM device with the same modifier and creates the
   framebuffer with `drmModeAddFB2WithModifiers()` (`WPEViewDRM.cpp:144-189`, `WPEDRM.cpp:215-243`).
4. Kernel 6.6.23 VOP2 accepts `LINEAR` and any modifier in the plane's own list, and logs the error above for
   everything else (`rockchip_drm_vop2.c:445-479`). On RK3566 the **primary planes (Smart/Esmart windows) advertise
   only `DRM_FORMAT_MOD_LINEAR`**; AFBC modifiers exist only on the Cluster overlay windows
   (`rockchip_vop2_reg.c:59-78, 140-230`).

Expected result on such a device: XRGB8888/ARGB8888 (and the other primary plane formats) with the `LINEAR` modifier
only, so the error must not appear (`journalctl -k | grep -c 'Unsupported format modifier'` stays at 0). If it does,
force linear scanout buffers with `WPE_BUFFER_FORMAT=XR24:0:scanout` (`XR24` = `DRM_FORMAT_XRGB8888`, modifier `0` =
`DRM_FORMAT_MOD_LINEAR`, usage `scanout`; `WPEToplevel.cpp:622-645`). There is no separate "disable modifiers" switch
in 2.54; `WPE_DRM_DISABLE_ATOMIC` only affects the commit path.

## XKB data

The DRM seat creates an XKB keymap for `evdev/pc105/us` at connect time (`WPEDRMSeat.cpp:111`,
`WPEKeymapXKB.cpp:216-226`) and uses the result without a NULL check; libxkbcommon's `xkb_state_new()`
dereferences the keymap (`state.c:594`). Without `/usr/share/X11/xkb` (xkeyboard-config, MIT, about 4 MB) the DRM
platform crashes at startup.

## Rotation

The DRM module renders in the panel's native orientation and does no output transform; a touch controller reports
in the same orientation. A page for a panel mounted in another orientation rotates itself (CSS), and touch
hit-testing follows the CSS transform. A libinput calibration matrix that compensates a compositor's output transform
(Weston's `transform=rotate-90`) must not be installed: it would rotate touch against an unrotated framebuffer.

## Host checks

```shell
cd launcher
make check                                   # GLib unit tests: config parsing, D-Bus interface on a GTestDBus session bus
make -C .. wpe-headers                       # WPE WebKit 2.54.2 headers in ../.cache/wpe-host-include-2.54.2
make syntax-check WPE_HOST_INCLUDE=$PWD/../.cache/wpe-host-include-2.54.2   # -fsyntax-only of the sources
```

`make check` needs GLib/GIO development files and `dbus-daemon` (GTestDBus starts a private bus). The headers come
from [../lib/tests/mk-host-headers.py](../lib/tests/mk-host-headers.py), which the library uses as well.

## Debugging on a device

```shell
journalctl -u <launcher unit> -f                     # launcher + WebKit (the helper processes log to the same journal)
WPE_PLATFORM=headless wpe-launcher --headless about:blank; echo $?   # smoke test, expect 0
cat /sys/kernel/debug/dri/1/clients                  # KMS node clients: wpe-launcher (master); GPU clients are on dri/0
journalctl -k | grep -c 'Unsupported format modifier'   # expect 0
```

- "Display of type drm was not found": the libWPEWebKit-2.0 was built without `ENABLE_WPE_PLATFORM_DRM` (the DRM
  platform is compiled into the library in 2.54; there is no module directory).
- "Failed to become DRM master": another process holds the KMS node (a second UI, a test application).
- "Failed to initialize input": udev/libinput could not assign `seat0`; check `/run/udev` access.
- Segfault right after "WPE display ... connected" with a keyboard device present: XKB data missing.
