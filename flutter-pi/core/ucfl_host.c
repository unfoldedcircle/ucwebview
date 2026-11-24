/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The host-neutral plugin core, see ucfl_host.h.
 */
#include "ucfl_host.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucfl_views.h"

#define REASON_SIZE 512

/* a pending script dialog or login of a view */
struct request {
    struct request *next;
    int64_t id;      /* the protocol's requestId */
    uint64_t lib_id; /* the library's */
    bool auth;
    int dialog_type; /* UC_WEBVIEW_DIALOG_*, dialogs only */
};

enum op_kind {
    OP_RENDERED, /* whenRendered: reply "done", "timeout" or "cancelled" */
    OP_COOKIES,  /* setCookies: reply true/false */
    OP_CLEAR,    /* clearData: reply true/false */
};

/* a call whose reply waits for a library callback */
struct op {
    struct op *next;
    struct ucfl_host *host; /* NULL once the host is gone: the callback only frees the op */
    enum op_kind kind;
    void *token;
};

struct ucfl_view {
    struct ucfl_host *host;
    int64_t id;
    uc_webview *wv;
    void *surface;
    bool can_go_back, can_go_forward;
    struct request *requests;
    bool requests_on; /* the request callbacks were set */
};

struct ucfl_host {
    struct ucfl_host_ops ops;
    void *ctx;
    char *library_path, *data_dir, *unavailable;
    void *egl_display;

    struct ucfl_lib lib;
    bool probed, loaded, initialized;
    char reason[REASON_SIZE];

    struct ucfl_views views;
    bool listening;
    int64_t last_request_id;
    struct op *ops_pending;

    uint64_t events_sent, events_dropped, requests_defaulted;
    bool page_loaded;     /* a view was asked to load something: WebKit has taken its nominal frame rate */
    double rate_passed;   /* the refresh rate the library took last, 0 for none */
};

/* ---- helpers ---------------------------------------------------------------------------------------------------- */

void ucfl_host_log(struct ucfl_host *host, const char *fmt, ...)
{
    char line[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (host->ops.log)
        host->ops.log(host->ctx, line);
    else
        fprintf(stderr, "[uc_webview] %s\n", line);
}

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void respond(struct ucfl_host *host, void *token, struct ucfl_writer *w)
{
    host->ops.respond(host->ctx, token, w->failed ? NULL : w->data, w->failed ? 0 : w->size);
    ucfl_writer_free(w);
}

static void respond_error(struct ucfl_host *host, void *token, const char *code, const char *message)
{
    struct ucfl_writer w;

    ucfl_writer_init(&w);
    ucfl_put_error_envelope(&w, code, message);
    respond(host, token, &w);
}

bool ucfl_host_send_event(struct ucfl_host *host, struct ucfl_writer *w)
{
    bool sent = host->listening && !w->failed;

    if (sent) {
        host->ops.send_event(host->ctx, w->data, w->size);
        host->events_sent++;
    } else {
        host->events_dropped++;
    }
    ucfl_writer_free(w);
    return sent;
}

const struct ucfl_lib *ucfl_host_lib(const struct ucfl_host *host)
{
    return host->loaded ? &host->lib : NULL;
}

static void op_add(struct ucfl_host *host, struct op *op)
{
    op->next = host->ops_pending;
    host->ops_pending = op;
}

static void op_remove(struct ucfl_host *host, struct op *op)
{
    for (struct op **p = &host->ops_pending; *p; p = &(*p)->next) {
        if (*p == op) {
            *p = op->next;
            return;
        }
    }
}

/* ---- library ---------------------------------------------------------------------------------------------------- */

static bool ensure_library(struct ucfl_host *host)
{
    if (host->probed)
        return host->loaded;
    host->probed = true;
    if (host->unavailable) {
        snprintf(host->reason, sizeof(host->reason), "%s", host->unavailable);
    } else {
        host->loaded = ucfl_lib_load(&host->lib, host->library_path ? host->library_path : UC_WEBVIEW_SONAME,
                                     host->reason, sizeof(host->reason));
    }
    if (host->loaded) {
        const char *names[UCFL_FEATURE_COUNT];
        char list[512] = "";
        size_t n = ucfl_feature_names(host->lib.features, names, UCFL_FEATURE_COUNT);

        for (size_t i = 0; i < n; i++) {
            strncat(list, i ? ", " : "", sizeof(list) - strlen(list) - 1);
            strncat(list, names[i], sizeof(list) - strlen(list) - 1);
        }
        ucfl_host_log(host, "web view available: libucwebview API %d, features: %s", UC_WEBVIEW_API_VERSION,
                      n ? list : "none");
    } else {
        ucfl_host_log(host, "web view unavailable: %s", host->reason);
    }
    return host->loaded;
}

/*
 * The display's refresh rate of initialize or create, for uc_webview_global_set_refresh_rate(): WebKit takes its
 * nominal frame rate from the library once, when it sets up the display link at the first page load of the process,
 * and the library takes a rate before and after uc_webview_global_init() (uc_webview.h). So a rate goes to the library
 * until the first load was asked for, whichever call initialised the library (setCookies and clearData pass none).
 * After that only setRefreshRate passes one (it changes the library's tick rate at once, not WebKit's nominal rate).
 */
static void pass_refresh_rate(struct ucfl_host *host, double refresh_rate)
{
    if (!(refresh_rate > 0) || !host->lib.global_set_refresh_rate || host->page_loaded ||
        refresh_rate == host->rate_passed)
        return;
    if (host->lib.global_set_refresh_rate(refresh_rate) == 0)
        host->rate_passed = refresh_rate;
    else
        ucfl_host_log(host, "the library refused the refresh rate %.2f Hz (valid: 20..240)", refresh_rate);
}

static bool ensure_initialized(struct ucfl_host *host, double refresh_rate)
{
    if (!ensure_library(host))
        return false;
    pass_refresh_rate(host, refresh_rate);
    if (host->initialized)
        return true;
    if (host->ops.prepare_init && host->ops.prepare_init(host->ctx, host->reason, sizeof(host->reason)) != 0)
        return false;
    if (host->lib.global_init(host->egl_display, host->data_dir, NULL) != 0) {
        /* the web view stays unavailable, as remote-ui's WebViewLibrary::ensureInitialized() */
        snprintf(host->reason, sizeof(host->reason), "uc_webview_global_init failed");
        host->loaded = false;
        ucfl_host_log(host, "web view unavailable: %s", host->reason);
        return false;
    }
    host->initialized = true;
    ucfl_host_log(host, "web view initialised %s and %s",
                  host->egl_display ? "with the host's EGL display" : "without an EGL display (shared memory frames)",
                  host->data_dir ? "persistent website data" : "an ephemeral session");
    return true;
}

/* remote-ui dragThresholdFromEnv(): UC_WEBVIEW_DRAG_THRESHOLD 1..200, else WPE's 8 px */
static int drag_threshold(struct ucfl_host *host)
{
    const char *env = getenv("UC_WEBVIEW_DRAG_THRESHOLD");
    char *end;
    long px;

    if (host->lib.global_get_drag_threshold) {
        int reported = host->lib.global_get_drag_threshold();
        if (reported >= 1 && reported <= 200)
            return reported;
    }
    px = env ? strtol(env, &end, 10) : 0;
    return env && *env && !*end && px >= 1 && px <= 200 ? (int) px : 8;
}

/* ---- callbacks of the library (GLib dispatch on the platform thread) ---------------------------------------------- */

static void check_history(struct ucfl_view *view)
{
    const struct ucfl_lib *lib = &view->host->lib;
    bool back = lib->can_go_back(view->wv) != 0, forward = lib->can_go_forward(view->wv) != 0;
    struct ucfl_writer w;

    /* a same-document navigation (history.pushState, a fragment link) changes the history without a load event, so
     * remote-ui reads it again on every load event, frame and title change */
    if (back == view->can_go_back && forward == view->can_go_forward)
        return;
    view->can_go_back = back;
    view->can_go_forward = forward;
    ucfl_writer_init(&w);
    ucfl_put_event_history(&w, view->id, back, forward);
    ucfl_host_send_event(view->host, &w);
}

static void on_frame_ready(uc_webview *wv, const uc_webview_frame *frame, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_host *host = view->host;

    if (host->ops.surface_frame && view->surface)
        host->ops.surface_frame(host->ctx, view->surface, frame);
    else
        host->lib.frame_release(wv, frame->handle);
    check_history(view);
}

static void on_load_changed(uc_webview *wv, int event, const char *uri, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    if (event == UC_WEBVIEW_LOAD_FAILED) {
        char origin[256];
        ucfl_log_origin(uri, origin, sizeof(origin));
        ucfl_host_log(view->host, "view %lld: load of %s failed", (long long) view->id, origin);
    }
    ucfl_writer_init(&w);
    ucfl_put_event_load_changed(&w, view->id, event, uri);
    ucfl_host_send_event(view->host, &w);
    check_history(view);
}

static void on_title_changed(uc_webview *wv, const char *title, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_title(&w, view->id, title);
    ucfl_host_send_event(view->host, &w);
    check_history(view);
}

static void on_progress_changed(uc_webview *wv, double progress, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_progress(&w, view->id, progress);
    ucfl_host_send_event(view->host, &w);
}

static void on_web_process_terminated(uc_webview *wv, int reason, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_host_log(view->host, "view %lld: web process terminated (%s)", (long long) view->id,
                  ucfl_termination_name(reason));
    ucfl_writer_init(&w);
    ucfl_put_event_terminated(&w, view->id, reason);
    ucfl_host_send_event(view->host, &w);
}

static void on_ime_focus(uc_webview *wv, int focused, unsigned hints, unsigned purpose, const char *surrounding,
                         int cursor, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_ime_focus(&w, view->id, focused != 0, hints, purpose, surrounding, cursor);
    ucfl_host_send_event(view->host, &w);
}

static void on_ime_cursor_area(uc_webview *wv, int x, int y, int width, int height, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_ime_cursor_area(&w, view->id, x, y, width, height);
    ucfl_host_send_event(view->host, &w);
}

static void on_cursor_changed(uc_webview *wv, const char *css_name, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_cursor(&w, view->id, css_name);
    ucfl_host_send_event(view->host, &w);
}

static void on_ime_surrounding(uc_webview *wv, const char *text, int cursor, int anchor, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_ime_surrounding(&w, view->id, text, cursor, anchor);
    ucfl_host_send_event(view->host, &w);
}

static void on_pinch(uc_webview *wv, int phase, double scale, double x, double y, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_pinch(&w, view->id, phase, scale, x, y);
    ucfl_host_send_event(view->host, &w);
}

static void on_magnification_changed(uc_webview *wv, double magnification, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_magnification(&w, view->id, magnification);
    ucfl_host_send_event(view->host, &w);
}

static struct request *add_request(struct ucfl_view *view, uint64_t lib_id, bool auth, int dialog_type)
{
    struct request *request = calloc(1, sizeof(*request));

    if (!request)
        return NULL;
    request->id = ++view->host->last_request_id;
    request->lib_id = lib_id;
    request->auth = auth;
    request->dialog_type = dialog_type;
    request->next = view->requests;
    view->requests = request;
    return request;
}

static struct request *take_request(struct ucfl_view *view, int64_t id, uint64_t lib_id)
{
    for (struct request **p = &view->requests; *p; p = &(*p)->next) {
        if ((id && (*p)->id == id) || (!id && (*p)->lib_id == lib_id)) {
            struct request *request = *p;
            *p = request->next;
            return request;
        }
    }
    return NULL;
}

/* Answers a request taken out of the view's list with the library's default and frees it: "no" for a dialog, "leave"
 * for the leave-page question, no credentials for a login (remote-ui WebViewRequests::answerIfUnhandled()). */
static void answer_default(struct ucfl_view *view, struct request *request)
{
    const struct ucfl_lib *lib = &view->host->lib;

    if (!request)
        return;
    if (request->auth)
        lib->request_auth(view->wv, request->lib_id, NULL, NULL);
    else
        lib->request_reply(view->wv, request->lib_id, request->dialog_type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, NULL);
    view->host->requests_defaulted++;
    free(request);
}

static void on_script_dialog(uc_webview *wv, uint64_t lib_id, int type, const char *origin, const char *message,
                             const char *default_text, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_host *host = view->host;
    struct request *request = NULL;
    struct ucfl_writer w;

    if (!ucfl_dialog_type_name(type)) {
        /* a dialog type of a newer library: its "no" (uc_webview.h, "Compatibility") */
        host->lib.request_reply(wv, lib_id, 0, NULL);
        return;
    }
    if (host->listening)
        request = add_request(view, lib_id, false, type);
    if (!request) {
        /* nobody to ask: as the library without the callback, "no", and "leave" for the leave-page question */
        host->requests_defaulted++;
        host->lib.request_reply(wv, lib_id, type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, NULL);
        return;
    }
    ucfl_writer_init(&w);
    ucfl_put_event_script_dialog(&w, view->id, request->id, type, origin, message, default_text);
    if (!ucfl_host_send_event(host, &w))
        answer_default(view, take_request(view, request->id, 0)); /* no memory for the event: nobody can answer */
}

/* remote-ui webviewrequest.cpp isLoginScheme(): the schemes that take a user name and password */
static bool is_login_scheme(int scheme)
{
    return scheme == UC_WEBVIEW_AUTH_SCHEME_DEFAULT || scheme == UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC ||
           scheme == UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST || scheme == UC_WEBVIEW_AUTH_SCHEME_NTLM;
}

static void on_authenticate(uc_webview *wv, uint64_t lib_id, const char *origin, const char *realm, int scheme,
                            unsigned flags, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_host *host = view->host;
    struct request *request = NULL;
    struct ucfl_writer w;

    if (is_login_scheme(scheme) && host->listening)
        request = add_request(view, lib_id, true, 0);
    if (!request) {
        if (is_login_scheme(scheme))
            host->requests_defaulted++;
        host->lib.request_auth(wv, lib_id, NULL, NULL); /* without credentials: the server's 401 page */
        return;
    }
    ucfl_writer_init(&w);
    ucfl_put_event_authenticate(&w, view->id, request->id, origin, realm, scheme, flags);
    if (!ucfl_host_send_event(host, &w))
        answer_default(view, take_request(view, request->id, 0));
}

static void on_request_cancelled(uc_webview *wv, uint64_t lib_id, void *user)
{
    struct ucfl_view *view = user;
    struct request *request = take_request(view, 0, lib_id);
    struct ucfl_writer w;

    (void) wv;
    if (!request)
        return; /* answered here a moment ago, or answered by default */
    ucfl_writer_init(&w);
    ucfl_put_event_request_cancelled(&w, view->id, request->id, "page");
    ucfl_host_send_event(view->host, &w);
    free(request);
}

static void on_close_requested(uc_webview *wv, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_close_requested(&w, view->id);
    ucfl_host_send_event(view->host, &w);
}

static void on_new_window(uc_webview *wv, const char *uri, int result, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_new_window(&w, view->id, uri, result);
    ucfl_host_send_event(view->host, &w);
}

static void on_response_refused(uc_webview *wv, const char *uri, const char *mime_type, int reason, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_response_refused(&w, view->id, uri, mime_type, reason);
    ucfl_host_send_event(view->host, &w);
}

static void on_load_failed_detail(uc_webview *wv, const char *uri, int reason, const char *error_domain,
                                  int error_code, unsigned tls_errors, const char *message, void *user)
{
    struct ucfl_view *view = user;
    struct ucfl_writer w;

    (void) wv;
    ucfl_writer_init(&w);
    ucfl_put_event_load_failed(&w, view->id, uri, reason, error_domain, error_code, tls_errors, message);
    ucfl_host_send_event(view->host, &w);
}

/* uc_webview_when_rendered(): also from inside uc_webview_free() (CANCELLED) */
static void on_rendered(uc_webview *wv, int status, void *user)
{
    struct op *op = user;
    struct ucfl_writer w;

    (void) wv;
    if (op->host) {
        op_remove(op->host, op);
        ucfl_writer_init(&w);
        ucfl_put_string_reply(&w, ucfl_rendered_name(status));
        respond(op->host, op->token, &w);
    }
    free(op);
}

/* uc_webview_done_cb of setCookies and clearData */
static void on_done(int status, void *user)
{
    struct op *op = user;
    struct ucfl_writer w;

    if (op->host) {
        op_remove(op->host, op);
        if (status != UC_WEBVIEW_DONE_OK)
            ucfl_host_log(op->host, "%s failed", op->kind == OP_COOKIES ? "setting cookies" : "clearing website data");
        ucfl_writer_init(&w);
        ucfl_put_bool_reply(&w, status == UC_WEBVIEW_DONE_OK);
        respond(op->host, op->token, &w);
    }
    free(op);
}

/* ---- views ------------------------------------------------------------------------------------------------------ */

static void free_requests(struct ucfl_view *view)
{
    while (view->requests) {
        struct request *next = view->requests->next;
        free(view->requests);
        view->requests = next;
    }
}

/* the view is out of the table already */
static void dispose_view(struct ucfl_host *host, struct ucfl_view *view)
{
    if (view->surface && host->ops.surface_free)
        host->ops.surface_free(host->ctx, view->surface);
    view->surface = NULL;
    /* ends the view's when_rendered requests with CANCELLED inside the call (on_rendered replies), and its page
     * requests without a callback */
    host->lib.destroy(view->wv);
    view->wv = NULL;
    free_requests(view);
    free(view);
}

static void handle_create(struct ucfl_host *host, const struct ucfl_call *call, void *token)
{
    uc_webview_callbacks cb = { 0 };
    struct ucfl_view *view;
    struct ucfl_writer w;
    int64_t texture_id = -1;
    char origin[256];
    const unsigned f = host->lib.features;

    if (!ensure_initialized(host, call->create.refresh_rate)) {
        respond_error(host, token, host->loaded ? UCFL_ERROR_FAILED : UCFL_ERROR_UNAVAILABLE, host->reason);
        return;
    }
    view = calloc(1, sizeof(*view));
    if (!view) {
        respond_error(host, token, UCFL_ERROR_FAILED, "out of memory");
        return;
    }
    view->host = host;

    cb.frame_ready = on_frame_ready;
    cb.load_changed = on_load_changed;
    cb.title_changed = on_title_changed;
    cb.progress_changed = on_progress_changed;
    cb.web_process_terminated = on_web_process_terminated;
    cb.ime_focus = on_ime_focus;
    cb.ime_cursor_area = on_ime_cursor_area;
    cb.cursor_changed = on_cursor_changed;
    cb.ime_surrounding = on_ime_surrounding;
    /* the pinch callback switches the library's pinch recognition on: only with the whole feature */
    if ((f & UCFL_FEATURE_PINCH_ZOOM) && call->create.pinch_zoom)
        cb.pinch = on_pinch;
    cb.magnification_changed = on_magnification_changed;
    /* the request callbacks hand the page's dialogs to the host: only when it can answer them */
    view->requests_on = (f & UCFL_FEATURE_REQUESTS) && call->create.requests;
    if (view->requests_on) {
        cb.script_dialog = on_script_dialog;
        cb.authenticate = on_authenticate;
        cb.request_cancelled = on_request_cancelled;
        cb.close_requested = on_close_requested;
        cb.new_window = on_new_window;
        cb.response_refused = on_response_refused;
    }
    cb.load_failed_detail = on_load_failed_detail; /* library 0.10.0; an older one never calls it */

    view->wv = host->lib.create_sized
                   ? host->lib.create_sized(call->create.width, call->create.height, &cb, sizeof(cb), view)
                   : host->lib.create(call->create.width, call->create.height, &cb, view);
    if (!view->wv) {
        free(view);
        respond_error(host, token, UCFL_ERROR_FAILED, "uc_webview_new failed");
        return;
    }
    view->id = ucfl_views_add(&host->views, view);
    if (view->id < 0 ||
        (host->ops.surface_new && host->ops.surface_new(host->ctx, view->id, view->wv, call->create.refresh_rate,
                                                        &texture_id, &view->surface) != 0)) {
        if (view->id >= 0)
            ucfl_views_remove(&host->views, view->id);
        view->surface = NULL;
        dispose_view(host, view);
        respond_error(host, token, UCFL_ERROR_FAILED, "no texture for the view");
        return;
    }
    if (call->create.pixel_ratio > 0)
        host->lib.set_scale(view->wv, call->create.pixel_ratio);
    ucfl_log_origin(call->create.url, origin, sizeof(origin));
    ucfl_host_log(host, "view %lld: %dx%d, texture %lld%s%s", (long long) view->id, call->create.width,
                  call->create.height, (long long) texture_id, call->create.url ? ", loads " : "",
                  call->create.url ? origin : "");
    if (call->create.url && *call->create.url) {
        host->page_loaded = true;
        host->lib.load_uri(view->wv, call->create.url);
    }
    ucfl_writer_init(&w);
    ucfl_put_create_reply(&w, view->id, texture_id);
    respond(host, token, &w);
}

/* Before a navigation of the host: a pending leave-page question is answered here, leave or stay, instead of the
 * library's "stay" for every navigation call (remote-ui WebViewRequests::answerLeavePage()). */
static void answer_leave_page(struct ucfl_view *view, bool leave)
{
    struct request **p = &view->requests;

    while (*p) {
        struct request *request = *p;
        if (request->auth || request->dialog_type != UC_WEBVIEW_DIALOG_BEFORE_UNLOAD) {
            p = &request->next;
            continue;
        }
        *p = request->next;
        view->host->lib.request_reply(view->wv, request->lib_id, leave, NULL);
        struct ucfl_writer w;
        ucfl_writer_init(&w);
        ucfl_put_event_request_cancelled(&w, view->id, request->id, "navigation");
        ucfl_host_send_event(view->host, &w);
        free(request);
    }
}

/* the feature a method needs, 0 for none */
static unsigned feature_of(enum ucfl_method method)
{
    switch (method) {
    case UCFL_METHOD_SET_REFRESH_RATE:
        return UCFL_FEATURE_REFRESH_RATE;
    case UCFL_METHOD_SET_COLOR_SCHEME:
        return UCFL_FEATURE_COLOR_SCHEME;
    case UCFL_METHOD_SET_TLS_ERRORS_POLICY:
        return UCFL_FEATURE_TLS_ERRORS_POLICY;
    case UCFL_METHOD_ALLOW_CERTIFICATE:
        return UCFL_FEATURE_ALLOW_CERTIFICATE;
    case UCFL_METHOD_SET_COOKIES:
        return UCFL_FEATURE_COOKIES;
    case UCFL_METHOD_CLEAR_DATA:
        return UCFL_FEATURE_CLEAR_DATA;
    case UCFL_METHOD_SET_MAGNIFICATION:
        return UCFL_FEATURE_PINCH_ZOOM;
    case UCFL_METHOD_WHEN_RENDERED:
        return UCFL_FEATURE_WHEN_RENDERED;
    case UCFL_METHOD_REPLY_DIALOG:
    case UCFL_METHOD_REPLY_AUTH:
        return UCFL_FEATURE_REQUESTS;
    case UCFL_METHOD_SET_ORIGIN_HEADERS:
        return UCFL_FEATURE_ORIGIN_HEADERS;
    case UCFL_METHOD_SET_USER_AGENT:
        return UCFL_FEATURE_USER_AGENT;
    case UCFL_METHOD_SET_DEVICE_API:
    case UCFL_METHOD_SET_DEVICE_STATE:
    case UCFL_METHOD_DEVICE_EVENT:
        return UCFL_FEATURE_DEVICE_API;
    default:
        return 0;
    }
}

/* ---- method calls ----------------------------------------------------------------------------------------------- */

static void handle_hello(struct ucfl_host *host, struct ucfl_writer *w)
{
    const char *names[UCFL_FEATURE_COUNT];
    bool available = ensure_library(host);
    struct ucfl_hello hello = {
        .available = available,
        .reason = available ? NULL : host->reason,
        .library = available && host->lib.library_version ? host->lib.library_version() : NULL,
        .abi = available ? host->lib.api_version() : -1,
        .features = names,
        .n_features = available ? ucfl_feature_names(host->lib.features, names, UCFL_FEATURE_COUNT) : 0,
    };
    ucfl_put_hello_reply(w, &hello);
}

static void handle_stats(struct ucfl_host *host, struct ucfl_writer *w)
{
    struct ucfl_stat stats[48];
    size_t n = 0, requests = 0, replies = 0;

    for (size_t i = 0; i < host->views.count; i++) {
        const struct ucfl_view *view = host->views.slots[i].data;
        for (const struct request *r = view->requests; r; r = r->next)
            requests++;
    }
    for (const struct op *op = host->ops_pending; op; op = op->next)
        replies++;
    stats[n++] = (struct ucfl_stat) { "views", (int64_t) host->views.count };
    stats[n++] = (struct ucfl_stat) { "pendingRequests", (int64_t) requests };
    stats[n++] = (struct ucfl_stat) { "pendingReplies", (int64_t) replies };
    stats[n++] = (struct ucfl_stat) { "requestsDefaulted", (int64_t) host->requests_defaulted };
    stats[n++] = (struct ucfl_stat) { "eventsSent", (int64_t) host->events_sent };
    stats[n++] = (struct ucfl_stat) { "eventsDropped", (int64_t) host->events_dropped };
    if (host->ops.stats)
        n += host->ops.stats(host->ctx, stats + n, sizeof(stats) / sizeof(stats[0]) - n);
    ucfl_put_stats_reply(w, stats, n);
}

/* a call into the library that returns 0 or -1: null or "refused" */
static void put_result(struct ucfl_writer *w, int result, const char *what)
{
    if (result == 0)
        ucfl_put_null_reply(w);
    else
        ucfl_put_error_envelope(w, UCFL_ERROR_REFUSED, what);
}

static void start_op(struct ucfl_host *host, const struct ucfl_call *call, struct ucfl_view *view, void *token)
{
    struct op *op = calloc(1, sizeof(*op));
    int result;

    if (!op) {
        respond_error(host, token, UCFL_ERROR_FAILED, "out of memory");
        return;
    }
    op->host = host;
    op->token = token;
    switch (call->method) {
    case UCFL_METHOD_WHEN_RENDERED:
        op->kind = OP_RENDERED;
        op_add(host, op);
        result = host->lib.when_rendered(view->wv, on_rendered, op);
        break;
    case UCFL_METHOD_SET_COOKIES: {
        char origin[256];
        op->kind = OP_COOKIES;
        op_add(host, op);
        result = host->lib.global_set_cookies(call->set_cookies.url, call->set_cookies.cookies, call->set_cookies.count,
                                              on_done, op);
        ucfl_log_origin(call->set_cookies.url, origin, sizeof(origin));
        if (result == 0)
            ucfl_host_log(host, "setting %zu cookies for %s", call->set_cookies.count, origin);
        break;
    }
    default: /* UCFL_METHOD_CLEAR_DATA */
        op->kind = OP_CLEAR;
        op_add(host, op);
        result = host->lib.global_clear_data(call->clear_data.types, on_done, op);
        if (result == 0)
            ucfl_host_log(host, "clearing %s of every web view",
                          call->clear_data.types == UC_WEBVIEW_DATA_ALL ? "the website data" : "the HTTP cache");
        break;
    }
    if (result != 0) {
        /* refused: the callback never comes */
        op_remove(host, op);
        free(op);
        respond_error(host, token, UCFL_ERROR_REFUSED, "the library refused the request");
    }
}

static void handle_view_call(struct ucfl_host *host, const struct ucfl_call *call, struct ucfl_view *view,
                             struct ucfl_writer *w)
{
    const struct ucfl_lib *lib = &host->lib;
    uc_webview *wv = view->wv;
    char origin[256];

    switch (call->method) {
    case UCFL_METHOD_RESIZE:
        lib->resize(wv, call->resize.width, call->resize.height);
        break;
    case UCFL_METHOD_SET_MAPPED:
        lib->set_mapped(wv, call->flag.value);
        break;
    case UCFL_METHOD_SET_FOCUSED:
        lib->set_focused(wv, call->flag.value);
        break;
    case UCFL_METHOD_SET_SCALE:
        lib->set_scale(wv, call->number.value);
        break;
    case UCFL_METHOD_SET_BACKGROUND_COLOR:
        lib->set_background_rgba(wv, call->color.r, call->color.g, call->color.b, call->color.a);
        break;
    case UCFL_METHOD_SET_FRAME_HOLD:
        if (host->ops.surface_hold && view->surface)
            host->ops.surface_hold(host->ctx, view->surface, call->flag.value);
        break;
    case UCFL_METHOD_LOAD_URI:
        answer_leave_page(view, true);
        host->page_loaded = true;
        ucfl_log_origin(call->load_uri.uri, origin, sizeof(origin));
        ucfl_host_log(host, "view %lld: load %s", (long long) view->id, origin);
        lib->load_uri(wv, call->load_uri.uri);
        break;
    case UCFL_METHOD_LOAD_HTML:
        answer_leave_page(view, true);
        host->page_loaded = true;
        lib->load_html(wv, call->load_html.html, call->load_html.base_uri);
        break;
    case UCFL_METHOD_RELOAD:
        answer_leave_page(view, true);
        host->page_loaded = true;
        lib->reload(wv);
        break;
    case UCFL_METHOD_STOP:
        answer_leave_page(view, false);
        lib->stop(wv);
        break;
    case UCFL_METHOD_GO_BACK:
        answer_leave_page(view, true);
        host->page_loaded = true;
        lib->go_back(wv);
        break;
    case UCFL_METHOD_GO_FORWARD:
        answer_leave_page(view, true);
        host->page_loaded = true;
        lib->go_forward(wv);
        break;
    case UCFL_METHOD_GET_STATE: {
        /* a string of the library is valid until its next call: copy the uri before the other calls */
        const char *library_uri = lib->get_uri(wv);
        char *uri = library_uri ? strdup(library_uri) : NULL;
        struct ucfl_view_state state = { .uri = uri };

        state.can_go_back = lib->can_go_back(wv) != 0;
        state.can_go_forward = lib->can_go_forward(wv) != 0;
        state.has_magnification = (lib->features & UCFL_FEATURE_PINCH_ZOOM) != 0;
        state.magnification = state.has_magnification ? lib->get_magnification(wv) : 1.0;
        state.title = lib->get_title(wv);
        if (library_uri && !uri)
            ucfl_put_error_envelope(w, UCFL_ERROR_FAILED, "out of memory");
        else
            ucfl_put_state_reply(w, &state);
        free(uri);
        return;
    }
    case UCFL_METHOD_RUN_JAVASCRIPT:
        lib->run_javascript(wv, call->run_javascript.script);
        break;
    case UCFL_METHOD_SET_ZOOM:
        lib->set_zoom(wv, call->number.value);
        break;
    case UCFL_METHOD_SET_MAGNIFICATION:
        put_result(w, lib->set_magnification(wv, call->set_magnification.scale, call->set_magnification.x,
                                             call->set_magnification.y),
                   "the library refused the magnification");
        return;
    case UCFL_METHOD_TOUCH:
        lib->touch(wv, call->touch.slot, call->touch.type, call->touch.x, call->touch.y, call->touch.time_ms);
        break;
    case UCFL_METHOD_POINTER_BUTTON:
        lib->pointer_button(wv, call->pointer_button.button, call->pointer_button.pressed, call->pointer_button.x,
                            call->pointer_button.y, call->pointer_button.modifiers, call->pointer_button.time_ms);
        break;
    case UCFL_METHOD_POINTER_MOVE:
        lib->pointer_move(wv, call->pointer_move.x, call->pointer_move.y, call->pointer_move.modifiers,
                          call->pointer_move.time_ms);
        break;
    case UCFL_METHOD_SCROLL:
        lib->scroll(wv, call->scroll.dx, call->scroll.dy, call->scroll.x, call->scroll.y, call->scroll.precise,
                    call->scroll.time_ms);
        break;
    case UCFL_METHOD_KEY:
        lib->key(wv, call->key.keysym, call->key.keycode, call->key.pressed, call->key.modifiers, call->key.time_ms);
        break;
    case UCFL_METHOD_IME_COMMIT:
        lib->ime_commit(wv, call->ime_commit.text);
        break;
    case UCFL_METHOD_IME_PREEDIT:
        lib->ime_preedit(wv, call->ime_preedit.text, call->ime_preedit.cursor);
        break;
    case UCFL_METHOD_IME_DELETE_SURROUNDING:
        lib->ime_delete_surrounding(wv, call->ime_delete_surrounding.offset, call->ime_delete_surrounding.count);
        break;
    case UCFL_METHOD_REPLY_DIALOG:
    case UCFL_METHOD_REPLY_AUTH: {
        bool auth = call->method == UCFL_METHOD_REPLY_AUTH;
        int64_t id = auth ? call->reply_auth.request_id : call->reply_dialog.request_id;
        struct request *request = take_request(view, id, 0);
        int result;

        if (!request) {
            ucfl_put_error_envelope(w, UCFL_ERROR_NO_REQUEST, "unknown or ended requestId");
            return;
        }
        if (request->auth != auth) {
            /* put back: a login answered with replyDialog or the other way round */
            request->next = view->requests;
            view->requests = request;
            ucfl_put_error_envelope(w, UCFL_ERROR_NO_REQUEST,
                                    auth ? "requestId is a script dialog: replyDialog" : "requestId is a login: replyAuth");
            return;
        }
        result = auth ? lib->request_auth(wv, request->lib_id, call->reply_auth.user, call->reply_auth.password)
                      : lib->request_reply(wv, request->lib_id, call->reply_dialog.accept, call->reply_dialog.text);
        if (result != 0) {
            /* an id the library ended (request_cancelled follows) or a string it refused: still pending */
            request->next = view->requests;
            view->requests = request;
            ucfl_put_error_envelope(w, UCFL_ERROR_REFUSED, "the library refused the answer");
            return;
        }
        free(request);
        break;
    }
    case UCFL_METHOD_SET_ORIGIN_HEADERS:
        ucfl_log_origin(call->set_origin_headers.origin, origin, sizeof(origin));
        put_result(w,
                   lib->set_origin_headers(wv, call->set_origin_headers.origin, call->set_origin_headers.names,
                                           call->set_origin_headers.values, call->set_origin_headers.count),
                   "the library refused the headers");
        /* names and values are never logged: a value is a token or a key */
        ucfl_host_log(host, "view %lld: %zu request headers for %s", (long long) view->id,
                      call->set_origin_headers.count, origin);
        return;
    case UCFL_METHOD_SET_USER_AGENT:
        put_result(w, lib->set_user_agent(wv, call->set_user_agent.preset, call->set_user_agent.custom),
                   "the library refused the user agent");
        return;
    case UCFL_METHOD_SET_DEVICE_API:
        put_result(w, lib->set_device_api(wv, call->flag.value), "the library refused the device API");
        return;
    case UCFL_METHOD_SET_DEVICE_STATE:
        put_result(w, lib->set_device_state(wv, call->set_device_state.json), "the library refused the device state");
        return;
    case UCFL_METHOD_DEVICE_EVENT:
        put_result(w, lib->device_event(wv, call->device_event.type, call->device_event.slept_ms),
                   "the library refused the device event");
        return;
    default:
        break;
    }
    ucfl_put_null_reply(w);
}

static void handle_call(struct ucfl_host *host, const struct ucfl_call *call, void *token)
{
    struct ucfl_writer w;
    struct ucfl_view *view = NULL;
    unsigned feature = feature_of(call->method);

    ucfl_writer_init(&w);
    switch (call->method) {
    case UCFL_METHOD_HELLO:
        handle_hello(host, &w);
        respond(host, token, &w);
        return;
    case UCFL_METHOD_STATS:
        handle_stats(host, &w);
        respond(host, token, &w);
        return;
    default:
        break;
    }

    if (!ensure_library(host)) {
        respond_error(host, token, UCFL_ERROR_UNAVAILABLE, host->reason);
        return;
    }
    if (feature && !(host->lib.features & feature)) {
        char message[96];
        snprintf(message, sizeof(message), "the library lacks the feature %s", ucfl_feature_name(feature));
        respond_error(host, token, UCFL_ERROR_UNSUPPORTED, message);
        return;
    }
    if (ucfl_method_has_view(call->method)) {
        view = ucfl_views_get(&host->views, call->view_id);
        if (!view) {
            respond_error(host, token, UCFL_ERROR_NO_VIEW, "unknown viewId");
            return;
        }
    }

    switch (call->method) {
    case UCFL_METHOD_INITIALIZE:
        if (!ensure_initialized(host, call->initialize.refresh_rate)) {
            respond_error(host, token, host->loaded ? UCFL_ERROR_FAILED : UCFL_ERROR_UNAVAILABLE, host->reason);
            return;
        }
        ucfl_put_initialize_reply(&w, drag_threshold(host),
                                  host->lib.global_get_refresh_rate ? host->lib.global_get_refresh_rate() : -1);
        break;
    case UCFL_METHOD_SET_REFRESH_RATE:
        if (host->lib.global_set_refresh_rate(call->set_refresh_rate.hz) == 0) {
            host->rate_passed = call->set_refresh_rate.hz;
            ucfl_put_null_reply(&w);
        } else {
            ucfl_put_error_envelope(&w, UCFL_ERROR_REFUSED, "the library refused the refresh rate (valid: 20..240)");
        }
        break;
    case UCFL_METHOD_SET_COLOR_SCHEME:
        put_result(&w, host->lib.global_set_color_scheme(call->set_color_scheme.scheme),
                   "the library refused the colour scheme");
        break;
    case UCFL_METHOD_SET_TLS_ERRORS_POLICY:
        put_result(&w, host->lib.global_set_tls_errors_policy(call->set_tls_errors_policy.ignore),
                   "the library refused the policy");
        break;
    case UCFL_METHOD_ALLOW_CERTIFICATE:
        /* the library logs the exception it takes, or why it refused it */
        put_result(&w, host->lib.global_allow_certificate(call->allow_certificate.host, call->allow_certificate.sha256),
                   "the library refused the certificate exception");
        break;
    case UCFL_METHOD_SET_COOKIES:
    case UCFL_METHOD_CLEAR_DATA:
        /* both need the network session: the data of an earlier run is on disk */
        if (!ensure_initialized(host, 0)) {
            respond_error(host, token, host->loaded ? UCFL_ERROR_FAILED : UCFL_ERROR_UNAVAILABLE, host->reason);
            return;
        }
        start_op(host, call, NULL, token);
        return;
    case UCFL_METHOD_CREATE:
        handle_create(host, call, token);
        return;
    case UCFL_METHOD_DISPOSE:
        ucfl_views_remove(&host->views, view->id);
        ucfl_host_log(host, "view %lld: disposed", (long long) view->id);
        dispose_view(host, view);
        ucfl_put_null_reply(&w);
        break;
    case UCFL_METHOD_WHEN_RENDERED:
        start_op(host, call, view, token);
        return;
    default:
        handle_view_call(host, call, view, &w);
        break;
    }
    respond(host, token, &w);
}

void ucfl_host_handle_control(struct ucfl_host *host, const uint8_t *msg, size_t size, void *token)
{
    struct ucfl_call call;
    const char *error;
    int err = ucfl_parse_call(msg, size, false, &call, &error);

    if (err == ENOTSUP) {
        host->ops.respond(host->ctx, token, NULL, 0); /* not implemented: MissingPluginException */
    } else if (err) {
        respond_error(host, token,
                      err == EINVAL    ? UCFL_ERROR_BAD_ARGUMENTS
                      : err == EBADMSG ? UCFL_ERROR_BAD_MESSAGE
                                       : UCFL_ERROR_FAILED,
                      error);
    } else {
        handle_call(host, &call, token);
    }
    ucfl_call_clear(&call);
    /* the library may have attached sources from this thread, which does not wake the context */
    if (host->ops.after_call)
        host->ops.after_call(host->ctx);
}

void ucfl_host_handle_events(struct ucfl_host *host, const uint8_t *msg, size_t size, void *token)
{
    struct ucfl_writer w;
    struct ucfl_call call;
    const char *error;
    int err = ucfl_parse_call(msg, size, true, &call, &error);

    ucfl_writer_init(&w);
    if (err == ENOTSUP) {
        ucfl_call_clear(&call);
        host->ops.respond(host->ctx, token, NULL, 0);
        return;
    }
    if (err) {
        ucfl_put_error_envelope(&w, UCFL_ERROR_BAD_MESSAGE, error);
    } else {
        host->listening = call.method == UCFL_METHOD_LISTEN;
        /* nobody can answer the pending requests any more: the page would wait forever */
        if (!host->listening) {
            for (size_t i = 0; i < host->views.count; i++) {
                struct ucfl_view *view = host->views.slots[i].data;
                while (view->requests)
                    answer_default(view, take_request(view, view->requests->id, 0));
            }
        }
        ucfl_put_null_reply(&w);
    }
    ucfl_call_clear(&call);
    respond(host, token, &w);
}

/* ---- lifecycle -------------------------------------------------------------------------------------------------- */

struct ucfl_host *ucfl_host_new(const struct ucfl_host_ops *ops, void *ctx, const struct ucfl_host_config *config)
{
    struct ucfl_host *host = calloc(1, sizeof(*host));

    if (!host)
        return NULL;
    host->ops = *ops;
    host->ctx = ctx;
    host->egl_display = config->egl_display;
    host->library_path = dup_or_null(config->library_path);
    host->data_dir = dup_or_null(config->data_dir && *config->data_dir ? config->data_dir : NULL);
    host->unavailable = dup_or_null(config->unavailable);
    ucfl_views_init(&host->views);
    if ((config->library_path && !host->library_path) || (config->unavailable && !host->unavailable) ||
        (config->data_dir && *config->data_dir && !host->data_dir)) {
        ucfl_host_free(host);
        return NULL;
    }
    return host;
}

void ucfl_host_free(struct ucfl_host *host)
{
    if (!host)
        return;
    /* the engine is going: replies that still wait are never sent, and callbacks that come later only free their op */
    for (struct op *op = host->ops_pending; op; op = op->next)
        op->host = NULL;
    host->ops_pending = NULL;
    while (host->views.count) {
        struct ucfl_view *view = ucfl_views_remove(&host->views, host->views.slots[0].id);
        dispose_view(host, view);
    }
    ucfl_views_free(&host->views);
    free(host->library_path);
    free(host->data_dir);
    free(host->unavailable);
    free(host);
}
