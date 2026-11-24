/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * A stand-in for libucwebview.so.1 in the host tests: every function of uc_webview.h with its real signature (the
 * header is included), which records the call in a log and keeps what a test needs to play the library's part (the
 * callbacks of each view, pending when_rendered and done requests, pending page requests). Plain C, no GLib: the
 * tests drive the callbacks themselves, as the library does from the GLib main context.
 *
 * Test control (resolved by the tests with dlsym() on the loader's handle), all prefixed fake_:
 *   fake_log()                 the calls so far, one line each ("load_uri 1 https://example.com/")
 *   fake_log_clear()
 *   fake_view(n)               the n-th view created (0 first), NULL beyond; fake_live_views() those not freed
 *   fake_callbacks(wv, &user, &size)  the callbacks the view was created with, its user pointer and the size passed
 *   fake_fail(name)            makes the function name (without uc_webview_) fail from now on (-1 / NULL); NULL resets
 *   fake_rendered(wv, status)  ends the view's oldest when_rendered request; returns 0, -1 without one
 *   fake_done(status)          ends the oldest set_cookies / clear_data request; returns 0, -1 without one
 *   fake_add_request(wv, id)   marks a page request id as pending (request_reply/_auth refuse others)
 *   fake_set_page(wv, uri, title, back, forward)  what get_uri, get_title and can_go_* report
 *   fake_reset()               forgets every view, the log, the failure and the done requests (between tests)
 * Variants by define:
 *   FAKE_API_VERSION=<n>  uc_webview_api_version() returns n (default UC_WEBVIEW_API_VERSION)
 *   FAKE_INCOMPLETE       lacks uc_webview_key (a required function)
 *   FAKE_OLD              lacks every optional function (a library of the first release)
 *   FAKE_NO_DEVICE_STATE  lacks uc_webview_set_device_state (the device API is incomplete)
 */
#include "uc_webview.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FAKE_API_VERSION
#define FAKE_API_VERSION UC_WEBVIEW_API_VERSION
#endif

#define EXPORT __attribute__((visibility("default")))
#define MAX_VIEWS 64
#define MAX_PENDING 64

struct uc_webview {
    int index; /* 1-based, as the log names it */
    int freed;
    uc_webview_callbacks cb;
    size_t cb_size;
    void *user;
    char uri[512], title[256];
    int can_back, can_forward;
    double magnification;
    uint64_t requests[MAX_PENDING];
    int n_requests;
    struct {
        uc_webview_rendered_cb cb;
        void *user;
    } rendered[MAX_PENDING];
    int n_rendered;
};

static struct uc_webview views[MAX_VIEWS];
static int n_views;
static char log_buf[1 << 16];
static size_t log_len;
static char fail_name[64];
static struct {
    uc_webview_done_cb cb;
    void *user;
} done[MAX_PENDING];
static int n_done;

static void record(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (log_len >= sizeof(log_buf) - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(log_buf + log_len, sizeof(log_buf) - log_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        log_len += (size_t) n < sizeof(log_buf) - log_len ? (size_t) n : sizeof(log_buf) - log_len - 1;
    if (log_len < sizeof(log_buf) - 1)
        log_buf[log_len++] = '\n';
    log_buf[log_len] = '\0';
}

static int failing(const char *name)
{
    return fail_name[0] && strcmp(fail_name, name) == 0;
}

static const char *s(const char *v)
{
    return v ? v : "(null)";
}

/* ---- test control -------------------------------------------------------------------------------------------- */

EXPORT const char *fake_log(void) { return log_buf; }
EXPORT void fake_log_clear(void)
{
    log_len = 0;
    log_buf[0] = '\0';
}
EXPORT uc_webview *fake_view(int n) { return n >= 0 && n < n_views ? &views[n] : NULL; }
EXPORT int fake_live_views(void)
{
    int live = 0;
    for (int i = 0; i < n_views; i++)
        live += !views[i].freed;
    return live;
}
EXPORT const uc_webview_callbacks *fake_callbacks(uc_webview *wv, void **user, size_t *size)
{
    *user = wv->user;
    *size = wv->cb_size;
    return &wv->cb;
}
EXPORT void fake_fail(const char *name) { snprintf(fail_name, sizeof(fail_name), "%s", name ? name : ""); }
EXPORT int fake_rendered(uc_webview *wv, int status)
{
    if (!wv->n_rendered)
        return -1;
    uc_webview_rendered_cb cb = wv->rendered[0].cb;
    void *user = wv->rendered[0].user;
    memmove(&wv->rendered[0], &wv->rendered[1], (size_t) --wv->n_rendered * sizeof(wv->rendered[0]));
    cb(wv, status, user);
    return 0;
}
EXPORT int fake_done(int status)
{
    if (!n_done)
        return -1;
    uc_webview_done_cb cb = done[0].cb;
    void *user = done[0].user;
    memmove(&done[0], &done[1], (size_t) --n_done * sizeof(done[0]));
    cb(status, user);
    return 0;
}
EXPORT void fake_add_request(uc_webview *wv, uint64_t id)
{
    if (wv->n_requests < MAX_PENDING)
        wv->requests[wv->n_requests++] = id;
}
EXPORT void fake_set_page(uc_webview *wv, const char *uri, const char *title, int back, int forward)
{
    snprintf(wv->uri, sizeof(wv->uri), "%s", uri ? uri : "");
    snprintf(wv->title, sizeof(wv->title), "%s", title ? title : "");
    wv->can_back = back;
    wv->can_forward = forward;
}

EXPORT void fake_reset(void)
{
    memset(views, 0, sizeof(views));
    n_views = 0;
    n_done = 0;
    fail_name[0] = '\0';
    fake_log_clear();
}

/* ending a page request: 0 when it was pending */
static __attribute__((unused)) int take_request(uc_webview *wv, uint64_t id)
{
    for (int i = 0; i < wv->n_requests; i++) {
        if (wv->requests[i] == id) {
            wv->requests[i] = wv->requests[--wv->n_requests];
            return 0;
        }
    }
    return -1;
}

/* ---- API version 1 as first released ------------------------------------------------------------------------ */

EXPORT int uc_webview_api_version(void) { return FAKE_API_VERSION; }
EXPORT int uc_webview_global_init(void *egl_display, const char *data_dir, const char *cache_dir)
{
    record("global_init %s %s %s", egl_display ? "egl" : "(null)", s(data_dir), s(cache_dir));
    return failing("global_init") ? -1 : 0;
}
EXPORT void uc_webview_global_shutdown(void) { record("global_shutdown"); }

static uc_webview *create(int width, int height, const uc_webview_callbacks *cb, size_t cb_size, void *user)
{
    uc_webview *wv;

    if (failing("new") || n_views == MAX_VIEWS)
        return NULL;
    wv = &views[n_views];
    memset(wv, 0, sizeof(*wv));
    wv->index = ++n_views;
    wv->magnification = 1.0;
    if (cb)
        memcpy(&wv->cb, cb, cb_size < sizeof(wv->cb) ? cb_size : sizeof(wv->cb));
    wv->cb_size = cb_size;
    wv->user = user;
    record("new %d %dx%d", wv->index, width, height);
    return wv;
}

EXPORT uc_webview *uc_webview_new(int width, int height, const uc_webview_callbacks *cb, void *user)
{
    /* the nine callbacks of API version 1 */
    return create(width, height, cb, offsetof(uc_webview_callbacks, pinch), user);
}
EXPORT void uc_webview_free(uc_webview *wv)
{
    if (!wv)
        return;
    record("free %d", wv->index);
    /* as the library: pending when_rendered requests end CANCELLED inside the call, page requests silently */
    while (wv->n_rendered)
        fake_rendered(wv, UC_WEBVIEW_RENDERED_CANCELLED);
    wv->n_requests = 0;
    wv->freed = 1;
}
EXPORT void uc_webview_resize(uc_webview *wv, int width, int height) { record("resize %d %dx%d", wv->index, width, height); }
EXPORT void uc_webview_set_mapped(uc_webview *wv, int mapped) { record("set_mapped %d %d", wv->index, mapped); }
EXPORT void uc_webview_set_focused(uc_webview *wv, int focused) { record("set_focused %d %d", wv->index, focused); }
EXPORT void uc_webview_set_scale(uc_webview *wv, double scale) { record("set_scale %d %g", wv->index, scale); }
EXPORT void uc_webview_load_uri(uc_webview *wv, const char *uri) { record("load_uri %d %s", wv->index, s(uri)); }
EXPORT void uc_webview_load_html(uc_webview *wv, const char *html, const char *base_uri)
{
    record("load_html %d %s %s", wv->index, s(html), s(base_uri));
}
EXPORT void uc_webview_reload(uc_webview *wv) { record("reload %d", wv->index); }
EXPORT void uc_webview_stop(uc_webview *wv) { record("stop %d", wv->index); }
EXPORT void uc_webview_go_back(uc_webview *wv) { record("go_back %d", wv->index); }
EXPORT void uc_webview_go_forward(uc_webview *wv) { record("go_forward %d", wv->index); }
EXPORT int uc_webview_can_go_back(uc_webview *wv) { return wv->can_back; }
EXPORT int uc_webview_can_go_forward(uc_webview *wv) { return wv->can_forward; }
EXPORT const char *uc_webview_get_uri(uc_webview *wv) { return wv->uri[0] ? wv->uri : NULL; }
EXPORT const char *uc_webview_get_title(uc_webview *wv) { return wv->title[0] ? wv->title : NULL; }
EXPORT void uc_webview_run_javascript(uc_webview *wv, const char *script)
{
    record("run_javascript %d %s", wv->index, s(script));
}
EXPORT void uc_webview_set_zoom(uc_webview *wv, double zoom) { record("set_zoom %d %g", wv->index, zoom); }
EXPORT void uc_webview_set_background_rgba(uc_webview *wv, float r, float g, float b, float a)
{
    record("set_background_rgba %d %.3f %.3f %.3f %.3f", wv->index, (double) r, (double) g, (double) b, (double) a);
}
EXPORT void uc_webview_frame_rendered(uc_webview *wv, void *frame_handle)
{
    record("frame_rendered %d %p", wv->index, frame_handle);
}
EXPORT void uc_webview_frame_release(uc_webview *wv, void *frame_handle)
{
    record("frame_release %d %p", wv->index, frame_handle);
}
EXPORT void uc_webview_touch(uc_webview *wv, int id, int phase, double x, double y, uint32_t ms)
{
    record("touch %d %d %d %g %g %u", wv->index, id, phase, x, y, ms);
}
EXPORT void uc_webview_pointer_button(uc_webview *wv, int button, int pressed, double x, double y, unsigned mods,
                                      uint32_t ms)
{
    record("pointer_button %d %d %d %g %g %u %u", wv->index, button, pressed, x, y, mods, ms);
}
EXPORT void uc_webview_pointer_move(uc_webview *wv, double x, double y, unsigned mods, uint32_t ms)
{
    record("pointer_move %d %g %g %u %u", wv->index, x, y, mods, ms);
}
EXPORT void uc_webview_scroll(uc_webview *wv, double dx, double dy, double x, double y, int precise, uint32_t ms)
{
    record("scroll %d %g %g %g %g %d %u", wv->index, dx, dy, x, y, precise, ms);
}
#ifndef FAKE_INCOMPLETE
EXPORT void uc_webview_key(uc_webview *wv, uint32_t xkb_keysym, uint32_t keycode, int pressed, unsigned mods,
                           uint32_t ms)
{
    record("key %d 0x%x %u %d %u %u", wv->index, xkb_keysym, keycode, pressed, mods, ms);
}
#endif
EXPORT void uc_webview_ime_commit(uc_webview *wv, const char *utf8) { record("ime_commit %d %s", wv->index, s(utf8)); }
EXPORT void uc_webview_ime_preedit(uc_webview *wv, const char *utf8, int cursor)
{
    record("ime_preedit %d %s %d", wv->index, s(utf8), cursor);
}
EXPORT void uc_webview_ime_delete_surrounding(uc_webview *wv, int offset, int n_chars)
{
    record("ime_delete_surrounding %d %d %d", wv->index, offset, n_chars);
}

/* ---- the optional functions of later releases ------------------------------------------------------------------- */

#ifndef FAKE_OLD
static double refresh_rate = 60.0;

EXPORT int uc_webview_global_set_refresh_rate(double hz)
{
    record("global_set_refresh_rate %g", hz);
    if (!(hz >= 20 && hz <= 240))
        return -1;
    refresh_rate = hz;
    return 0;
}
EXPORT double uc_webview_global_get_refresh_rate(void) { return refresh_rate; }
EXPORT int uc_webview_global_get_drag_threshold(void) { return 16; }
EXPORT uc_webview *uc_webview_new_sized(int width, int height, const uc_webview_callbacks *cb, size_t cb_size, void *user)
{
    return create(width, height, cb, cb_size, user);
}
EXPORT int uc_webview_when_rendered(uc_webview *wv, uc_webview_rendered_cb cb, void *user)
{
    if (failing("when_rendered") || !cb || wv->n_rendered == MAX_PENDING)
        return -1;
    record("when_rendered %d", wv->index);
    wv->rendered[wv->n_rendered].cb = cb;
    wv->rendered[wv->n_rendered++].user = user;
    return 0;
}
EXPORT int uc_webview_set_magnification(uc_webview *wv, double scale, double x, double y)
{
    record("set_magnification %d %g %g %g", wv->index, scale, x, y);
    if (failing("set_magnification"))
        return -1;
    wv->magnification = scale;
    return 0;
}
EXPORT double uc_webview_get_magnification(uc_webview *wv) { return wv->magnification; }
EXPORT int uc_webview_request_reply(uc_webview *wv, uint64_t request, int accept, const char *text)
{
    record("request_reply %d %llu %d %s", wv->index, (unsigned long long) request, accept, s(text));
    return take_request(wv, request);
}
EXPORT int uc_webview_request_auth(uc_webview *wv, uint64_t request, const char *user, const char *password)
{
    /* the password is part of the log so that a test can see it reached the library, nothing else logs it */
    record("request_auth %d %llu %s %s", wv->index, (unsigned long long) request, s(user), s(password));
    return take_request(wv, request);
}
EXPORT int uc_webview_set_origin_headers(uc_webview *wv, const char *origin, const char *const *names,
                                         const char *const *values, size_t count)
{
    record("set_origin_headers %d %s %zu", wv->index, s(origin), count);
    for (size_t i = 0; i < count; i++)
        record("  %s: %s", names[i], values[i]);
    return failing("set_origin_headers") ? -1 : 0;
}
EXPORT int uc_webview_set_user_agent(uc_webview *wv, int preset, const char *custom)
{
    record("set_user_agent %d %d %s", wv->index, preset, s(custom));
    return preset == UC_WEBVIEW_USER_AGENT_CUSTOM && !custom ? -1 : 0;
}
EXPORT int uc_webview_global_set_cookies(const char *url, const uc_webview_cookie *cookies, size_t count,
                                         uc_webview_done_cb cb, void *user)
{
    record("global_set_cookies %s %zu", s(url), count);
    for (size_t i = 0; i < count; i++)
        record("  %s=%s domain %s path %s secure %d http_only %d expires %lld", cookies[i].name, cookies[i].value,
               s(cookies[i].domain), s(cookies[i].path), cookies[i].secure, cookies[i].http_only,
               (long long) cookies[i].expires);
    if (failing("global_set_cookies") || n_done == MAX_PENDING)
        return -1;
    if (cb) {
        done[n_done].cb = cb;
        done[n_done++].user = user;
    }
    return 0;
}
EXPORT int uc_webview_global_allow_certificate(const char *host, const char *sha256)
{
    record("global_allow_certificate %s %s", s(host), s(sha256));
    return host && *host ? 0 : -1;
}
EXPORT int uc_webview_global_set_tls_errors_policy(int ignore)
{
    record("global_set_tls_errors_policy %d", ignore);
    return 0;
}
EXPORT int uc_webview_global_set_color_scheme(int scheme)
{
    record("global_set_color_scheme %d", scheme);
    return 0;
}
EXPORT int uc_webview_global_clear_data(unsigned types, uc_webview_done_cb cb, void *user)
{
    record("global_clear_data 0x%x", types);
    if (failing("global_clear_data") || n_done == MAX_PENDING)
        return -1;
    if (cb) {
        done[n_done].cb = cb;
        done[n_done++].user = user;
    }
    return 0;
}
EXPORT int uc_webview_set_device_api(uc_webview *wv, int enabled)
{
    record("set_device_api %d %d", wv->index, enabled);
    return 0;
}
#ifndef FAKE_NO_DEVICE_STATE
EXPORT int uc_webview_set_device_state(uc_webview *wv, const char *json)
{
    record("set_device_state %d %s", wv->index, s(json));
    return json && json[0] == '{' ? 0 : -1;
}
#endif
EXPORT int uc_webview_device_event(uc_webview *wv, const char *type, int64_t slept_ms)
{
    record("device_event %d %s %lld", wv->index, s(type), (long long) slept_ms);
    return 0;
}
EXPORT const char *uc_webview_library_version(void) { return "9.9.9-fake"; }
#endif
