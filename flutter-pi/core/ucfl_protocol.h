/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The uc.webview channel protocol (PROTOCOL.md), shared by every host adapter: parsing of the method calls into
 * checked C values, and the replies and events as complete messages.
 *
 * Two plain SDK channels:
 *  - UCFL_CHANNEL_CONTROL, a MethodChannel with StandardMethodCodec: calls down, one reply each.
 *  - UCFL_CHANNEL_EVENTS, an EventChannel (StandardMethodCodec): "listen"/"cancel" calls down, events up as success
 *    envelopes; every event is a map with "type" and, for the events of a view, "viewId".
 * Frames never cross the channels: a view's pixels reach the UI as a texture whose id create returns.
 *
 * This file knows names and value ranges only; what a call does is ucfl_host.c's.
 * Thread: any, no global state.
 */
#ifndef UCFL_PROTOCOL_H
#define UCFL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uc_webview.h"
#include "ucfl_codec.h"

#define UCFL_PROTOCOL_VERSION 1
#define UCFL_CHANNEL_CONTROL "uc.webview/control"
#define UCFL_CHANNEL_EVENTS "uc.webview/events"

/* the error codes of the replies (PROTOCOL.md, "Errors") */
#define UCFL_ERROR_BAD_MESSAGE "badMessage"     /* not a method call of StandardMethodCodec */
#define UCFL_ERROR_BAD_ARGUMENTS "badArguments" /* an argument is missing or invalid; the message names it */
#define UCFL_ERROR_UNAVAILABLE "unavailable"    /* no usable library (hello's reason) */
#define UCFL_ERROR_UNSUPPORTED "unsupported"    /* the library lacks the feature the method needs */
#define UCFL_ERROR_NO_VIEW "noView"             /* unknown or disposed viewId */
#define UCFL_ERROR_NO_REQUEST "noRequest"       /* unknown, answered or ended requestId */
#define UCFL_ERROR_REFUSED "refused"            /* the library refused the call (its log says why) */
#define UCFL_ERROR_FAILED "failed"              /* anything else: initialisation, memory, the host's texture */

/* Every method; the names are in ucfl_method_name(). */
enum ucfl_method {
    UCFL_METHOD_UNKNOWN = 0,
    /* process */
    UCFL_METHOD_HELLO,
    UCFL_METHOD_INITIALIZE,
    UCFL_METHOD_SET_REFRESH_RATE,
    UCFL_METHOD_SET_COLOR_SCHEME,
    UCFL_METHOD_SET_TLS_ERRORS_POLICY,
    UCFL_METHOD_ALLOW_CERTIFICATE,
    UCFL_METHOD_SET_COOKIES,
    UCFL_METHOD_CLEAR_DATA,
    UCFL_METHOD_STATS,
    /* view lifecycle */
    UCFL_METHOD_CREATE,
    UCFL_METHOD_DISPOSE,
    UCFL_METHOD_RESIZE,
    UCFL_METHOD_SET_MAPPED,
    UCFL_METHOD_SET_FOCUSED,
    UCFL_METHOD_SET_SCALE,
    UCFL_METHOD_SET_BACKGROUND_COLOR,
    UCFL_METHOD_SET_FRAME_HOLD,
    /* navigation and page */
    UCFL_METHOD_LOAD_URI,
    UCFL_METHOD_LOAD_HTML,
    UCFL_METHOD_RELOAD,
    UCFL_METHOD_STOP,
    UCFL_METHOD_GO_BACK,
    UCFL_METHOD_GO_FORWARD,
    UCFL_METHOD_GET_STATE,
    UCFL_METHOD_RUN_JAVASCRIPT,
    UCFL_METHOD_SET_ZOOM,
    UCFL_METHOD_SET_MAGNIFICATION,
    UCFL_METHOD_WHEN_RENDERED,
    /* input */
    UCFL_METHOD_TOUCH,
    UCFL_METHOD_POINTER_BUTTON,
    UCFL_METHOD_POINTER_MOVE,
    UCFL_METHOD_SCROLL,
    UCFL_METHOD_KEY,
    UCFL_METHOD_IME_COMMIT,
    UCFL_METHOD_IME_PREEDIT,
    UCFL_METHOD_IME_DELETE_SURROUNDING,
    /* requests */
    UCFL_METHOD_REPLY_DIALOG,
    UCFL_METHOD_REPLY_AUTH,
    /* site settings */
    UCFL_METHOD_SET_ORIGIN_HEADERS,
    UCFL_METHOD_SET_USER_AGENT,
    /* device state */
    UCFL_METHOD_SET_DEVICE_API,
    UCFL_METHOD_SET_DEVICE_STATE,
    UCFL_METHOD_DEVICE_EVENT,
    /* the EventChannel's calls */
    UCFL_METHOD_LISTEN,
    UCFL_METHOD_CANCEL,
    UCFL_METHOD_COUNT
};

/* At most this many headers per setOriginHeaders and cookies per setCookies (the library's own limits: 32, 64). */
#define UCFL_MAX_HEADERS 32
#define UCFL_MAX_COOKIES 64
/* allocations a parsed call owns at most: strings and arrays (cookies: 4 strings each, plus the array) */
#define UCFL_CALL_OWNED_MAX (4 * UCFL_MAX_COOKIES + 2 * UCFL_MAX_HEADERS + 8)

/*
 * A parsed call. Only view_id (for the methods of a view) and the member of the union of method are set. Strings are
 * NUL terminated UTF-8 copies (a string with a NUL byte is refused), NULL for an optional argument that is absent or
 * null; ucfl_call_clear() frees them.
 */
struct ucfl_call {
    enum ucfl_method method;
    int64_t view_id;
    union {
        struct {
            double refresh_rate; /* 0 when not given */
        } initialize;
        struct {
            double hz;
        } set_refresh_rate;
        struct {
            int scheme; /* UC_WEBVIEW_COLOR_SCHEME_* */
        } set_color_scheme;
        struct {
            bool ignore;
        } set_tls_errors_policy;
        struct {
            char *host;
            char *sha256; /* NULL: any certificate of host */
        } allow_certificate;
        struct {
            char *url;
            uc_webview_cookie *cookies; /* count elements, NULL for 0 */
            size_t count;
        } set_cookies;
        struct {
            unsigned types; /* UC_WEBVIEW_DATA_CACHE or UC_WEBVIEW_DATA_ALL */
        } clear_data;
        struct {
            int width, height;
            char *url;           /* NULL when not given */
            double pixel_ratio;  /* 0 when not given */
            double refresh_rate; /* 0 when not given */
            bool pinch_zoom;     /* default true */
            bool requests;       /* default true */
        } create;
        struct {
            int width, height;
        } resize;
        struct {
            bool value;
        } flag; /* setMapped (mapped), setFocused (focused), setFrameHold (hold), setDeviceApi (enabled) */
        struct {
            double value;
        } number; /* setScale (scale), setZoom (zoom) */
        struct {
            float r, g, b, a; /* 0..1 */
        } color;
        struct {
            char *uri;
        } load_uri;
        struct {
            char *html;
            char *base_uri; /* NULL: about:blank */
        } load_html;
        struct {
            char *script;
        } run_javascript;
        struct {
            double scale, x, y;
        } set_magnification;
        struct {
            int slot;
            int type; /* UC_WEBVIEW_TOUCH_* */
            double x, y;
            uint32_t time_ms;
        } touch;
        struct {
            int button; /* UC_WEBVIEW_BUTTON_* */
            bool pressed;
            double x, y;
            unsigned modifiers;
            uint32_t time_ms;
        } pointer_button;
        struct {
            double x, y;
            unsigned modifiers;
            uint32_t time_ms;
        } pointer_move;
        struct {
            double dx, dy, x, y;
            bool precise;
            uint32_t time_ms;
        } scroll;
        struct {
            uint32_t keysym, keycode;
            bool pressed;
            unsigned modifiers;
            uint32_t time_ms;
        } key;
        struct {
            char *text;
        } ime_commit;
        struct {
            char *text; /* NULL ends the pre-edit */
            int cursor;
        } ime_preedit;
        struct {
            int offset, count;
        } ime_delete_surrounding;
        struct {
            int64_t request_id;
            bool accept;
            char *text;
        } reply_dialog;
        struct {
            int64_t request_id;
            char *user; /* NULL: continue without credentials */
            char *password;
        } reply_auth;
        struct {
            char *origin;
            const char *names[UCFL_MAX_HEADERS];
            const char *values[UCFL_MAX_HEADERS];
            size_t count;
        } set_origin_headers;
        struct {
            int preset; /* UC_WEBVIEW_USER_AGENT_* */
            char *custom;
        } set_user_agent;
        struct {
            char *json;
        } set_device_state;
        struct {
            char *type; /* "suspend" or "resume" */
            int64_t slept_ms;
        } device_event;
    };
    void *owned[UCFL_CALL_OWNED_MAX];
    size_t n_owned;
};

/* Decodes and checks a method call of channel UCFL_CHANNEL_CONTROL (events = false) or UCFL_CHANNEL_EVENTS (true).
 * Returns 0; EBADMSG for a message that is no method call; ENOTSUP for an unknown method (call->method UNKNOWN);
 * EINVAL for invalid arguments, *error then names the problem (a static string); ENOMEM. call is cleared on every
 * error but ENOTSUP; clear it with ucfl_call_clear() after use (also after ENOTSUP). */
int ucfl_parse_call(const uint8_t *msg, size_t size, bool events, struct ucfl_call *call, const char **error);
void ucfl_call_clear(struct ucfl_call *call);

/* the name of a method as Dart calls it, "" for UNKNOWN */
const char *ucfl_method_name(enum ucfl_method method);
/* methods that act on a view (viewId argument) */
bool ucfl_method_has_view(enum ucfl_method method);

/* ---- replies (complete messages, envelope included) ---------------------------------------------------------------- */

struct ucfl_hello {
    bool available;
    const char *reason;  /* why not available, NULL otherwise */
    const char *library; /* the library's version string, NULL when it has none */
    int abi;             /* uc_webview_api_version() of the loaded library, < 0 when none was loaded */
    const char *const *features;
    size_t n_features;
};

/* one counter of the stats reply */
struct ucfl_stat {
    const char *name;
    int64_t value;
};

/* the getState reply */
struct ucfl_view_state {
    const char *uri, *title; /* NULL before the first load */
    bool can_go_back, can_go_forward;
    bool has_magnification; /* the library has get_magnification */
    double magnification;
};

void ucfl_put_hello_reply(struct ucfl_writer *w, const struct ucfl_hello *hello);
/* refresh_rate < 0: null (a library without global_get_refresh_rate) */
void ucfl_put_initialize_reply(struct ucfl_writer *w, int drag_threshold, double refresh_rate);
void ucfl_put_create_reply(struct ucfl_writer *w, int64_t view_id, int64_t texture_id);
void ucfl_put_state_reply(struct ucfl_writer *w, const struct ucfl_view_state *state);
void ucfl_put_null_reply(struct ucfl_writer *w);
void ucfl_put_bool_reply(struct ucfl_writer *w, bool value);
void ucfl_put_string_reply(struct ucfl_writer *w, const char *value);
void ucfl_put_stats_reply(struct ucfl_writer *w, const struct ucfl_stat *stats, size_t count);

/* ---- events (complete messages, envelope included) ----------------------------------------------------------------- */

void ucfl_put_event_load_changed(struct ucfl_writer *w, int64_t view_id, int load_event, const char *uri);
void ucfl_put_event_load_failed(struct ucfl_writer *w, int64_t view_id, const char *uri, int reason,
                                const char *error_domain, int error_code, unsigned tls_errors, const char *message);
void ucfl_put_event_progress(struct ucfl_writer *w, int64_t view_id, double progress);
void ucfl_put_event_title(struct ucfl_writer *w, int64_t view_id, const char *title);
void ucfl_put_event_history(struct ucfl_writer *w, int64_t view_id, bool can_go_back, bool can_go_forward);
void ucfl_put_event_frame(struct ucfl_writer *w, int64_t view_id, int width, int height, bool zero_copy);
void ucfl_put_event_terminated(struct ucfl_writer *w, int64_t view_id, int reason);
void ucfl_put_event_ime_focus(struct ucfl_writer *w, int64_t view_id, bool focused, unsigned hints, unsigned purpose,
                              const char *surrounding, int cursor);
void ucfl_put_event_ime_cursor_area(struct ucfl_writer *w, int64_t view_id, int x, int y, int width, int height);
void ucfl_put_event_ime_surrounding(struct ucfl_writer *w, int64_t view_id, const char *text, int cursor, int anchor);
void ucfl_put_event_cursor(struct ucfl_writer *w, int64_t view_id, const char *css_name);
void ucfl_put_event_pinch(struct ucfl_writer *w, int64_t view_id, int phase, double scale, double x, double y);
void ucfl_put_event_magnification(struct ucfl_writer *w, int64_t view_id, double magnification);
void ucfl_put_event_script_dialog(struct ucfl_writer *w, int64_t view_id, int64_t request_id, int dialog_type,
                                  const char *origin, const char *message, const char *default_text);
void ucfl_put_event_authenticate(struct ucfl_writer *w, int64_t view_id, int64_t request_id, const char *origin,
                                 const char *realm, int scheme, unsigned flags);
/* reason: "page" (the library ended it), "disposed" (the view went with the request pending) */
void ucfl_put_event_request_cancelled(struct ucfl_writer *w, int64_t view_id, int64_t request_id, const char *reason);
void ucfl_put_event_close_requested(struct ucfl_writer *w, int64_t view_id);
void ucfl_put_event_new_window(struct ucfl_writer *w, int64_t view_id, const char *uri, int result);
void ucfl_put_event_response_refused(struct ucfl_writer *w, int64_t view_id, const char *uri, const char *mime_type,
                                     int reason);

/* ---- names of the library's constants in the events and replies ---------------------------------------------------- */
/* Values a newer library adds get "unknown" (uc_webview.h, "Compatibility": hosts ignore what they do not know). */

const char *ucfl_load_event_name(int load_event);
const char *ucfl_load_failure_name(int reason);
const char *ucfl_termination_name(int reason);
const char *ucfl_ime_purpose_name(unsigned purpose);
const char *ucfl_pinch_phase_name(int phase);
const char *ucfl_dialog_type_name(int type); /* NULL for a type the library added later: answer it with "no" */
const char *ucfl_auth_scheme_name(int scheme);
const char *ucfl_response_refused_name(int reason);
const char *ucfl_rendered_name(int status);

/*
 * The origin of uri for log lines, "scheme://host[:port]", without user info, path, query and fragment; the scheme
 * alone ("data:", "about:") for a URI without an authority; "(none)" for NULL and "(invalid)" for a string without a
 * scheme. Secrets in a URL (a token in the query or path, credentials) never reach the log this way.
 */
void ucfl_log_origin(const char *uri, char *out, size_t out_size);

#endif /* UCFL_PROTOCOL_H */
