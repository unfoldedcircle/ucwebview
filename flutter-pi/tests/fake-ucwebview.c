/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * A stand-in for libucwebview.so.1 for tests/test-loader.c: the functions of uc_webview.h the loader resolves, with
 * their real signatures (the header is included), doing nothing. Variants by define:
 *   FAKE_API_VERSION=<n>  uc_webview_api_version() returns n (default UC_WEBVIEW_API_VERSION)
 *   FAKE_INCOMPLETE       lacks uc_webview_key (a required function)
 *   FAKE_OLD              lacks every optional function (a library before 0.2.0)
 */
#include "uc_webview.h"

#ifndef FAKE_API_VERSION
#define FAKE_API_VERSION UC_WEBVIEW_API_VERSION
#endif

#define EXPORT __attribute__((visibility("default")))

static int fake_view;

EXPORT int uc_webview_api_version(void) { return FAKE_API_VERSION; }
EXPORT int uc_webview_global_init(void *egl_display, const char *data_dir, const char *cache_dir)
{
    (void) egl_display, (void) data_dir, (void) cache_dir;
    return 0;
}
EXPORT uc_webview *uc_webview_new(int width, int height, const uc_webview_callbacks *cb, void *user)
{
    (void) width, (void) height, (void) cb, (void) user;
    return (uc_webview *) &fake_view;
}
EXPORT void uc_webview_free(uc_webview *wv) { (void) wv; }
EXPORT void uc_webview_resize(uc_webview *wv, int width, int height) { (void) wv, (void) width, (void) height; }
EXPORT void uc_webview_set_mapped(uc_webview *wv, int mapped) { (void) wv, (void) mapped; }
EXPORT void uc_webview_set_focused(uc_webview *wv, int focused) { (void) wv, (void) focused; }
EXPORT void uc_webview_set_scale(uc_webview *wv, double scale) { (void) wv, (void) scale; }
EXPORT void uc_webview_load_uri(uc_webview *wv, const char *uri) { (void) wv, (void) uri; }
EXPORT void uc_webview_frame_rendered(uc_webview *wv, void *frame_handle) { (void) wv, (void) frame_handle; }
EXPORT void uc_webview_frame_release(uc_webview *wv, void *frame_handle) { (void) wv, (void) frame_handle; }
EXPORT void uc_webview_touch(uc_webview *wv, int id, int phase, double x, double y, uint32_t ms)
{
    (void) wv, (void) id, (void) phase, (void) x, (void) y, (void) ms;
}
#ifndef FAKE_INCOMPLETE
EXPORT void uc_webview_key(uc_webview *wv, uint32_t xkb_keysym, uint32_t keycode, int pressed, unsigned mods, uint32_t ms)
{
    (void) wv, (void) xkb_keysym, (void) keycode, (void) pressed, (void) mods, (void) ms;
}
#endif

#ifndef FAKE_OLD
EXPORT int uc_webview_global_set_refresh_rate(double hz) { return hz >= 20 && hz <= 240 ? 0 : -1; }
EXPORT uc_webview *uc_webview_new_sized(int width, int height, const uc_webview_callbacks *cb, size_t cb_size, void *user)
{
    (void) cb_size;
    return uc_webview_new(width, height, cb, user);
}
EXPORT int uc_webview_set_device_api(uc_webview *wv, int enabled) { (void) wv, (void) enabled; return 0; }
EXPORT const char *uc_webview_library_version(void) { return "9.9.9-fake"; }
#endif
