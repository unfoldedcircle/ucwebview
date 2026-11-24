/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The uc.webview channel protocol, see ucfl_protocol.h and PROTOCOL.md.
 */
#include "ucfl_protocol.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    bool events; /* a call of the event channel */
    bool view;   /* has a viewId */
} methods[UCFL_METHOD_COUNT] = {
    [UCFL_METHOD_UNKNOWN] = { "", false, false },
    [UCFL_METHOD_HELLO] = { "hello", false, false },
    [UCFL_METHOD_INITIALIZE] = { "initialize", false, false },
    [UCFL_METHOD_SET_REFRESH_RATE] = { "setRefreshRate", false, false },
    [UCFL_METHOD_SET_COLOR_SCHEME] = { "setColorScheme", false, false },
    [UCFL_METHOD_SET_TLS_ERRORS_POLICY] = { "setTlsErrorsPolicy", false, false },
    [UCFL_METHOD_ALLOW_CERTIFICATE] = { "allowCertificate", false, false },
    [UCFL_METHOD_SET_COOKIES] = { "setCookies", false, false },
    [UCFL_METHOD_CLEAR_DATA] = { "clearData", false, false },
    [UCFL_METHOD_STATS] = { "stats", false, false },
    [UCFL_METHOD_CREATE] = { "create", false, false },
    [UCFL_METHOD_DISPOSE] = { "dispose", false, true },
    [UCFL_METHOD_RESIZE] = { "resize", false, true },
    [UCFL_METHOD_SET_MAPPED] = { "setMapped", false, true },
    [UCFL_METHOD_SET_FOCUSED] = { "setFocused", false, true },
    [UCFL_METHOD_SET_SCALE] = { "setScale", false, true },
    [UCFL_METHOD_SET_BACKGROUND_COLOR] = { "setBackgroundColor", false, true },
    [UCFL_METHOD_SET_FRAME_HOLD] = { "setFrameHold", false, true },
    [UCFL_METHOD_LOAD_URI] = { "loadUri", false, true },
    [UCFL_METHOD_LOAD_HTML] = { "loadHtml", false, true },
    [UCFL_METHOD_RELOAD] = { "reload", false, true },
    [UCFL_METHOD_STOP] = { "stop", false, true },
    [UCFL_METHOD_GO_BACK] = { "goBack", false, true },
    [UCFL_METHOD_GO_FORWARD] = { "goForward", false, true },
    [UCFL_METHOD_GET_STATE] = { "getState", false, true },
    [UCFL_METHOD_RUN_JAVASCRIPT] = { "runJavaScript", false, true },
    [UCFL_METHOD_SET_ZOOM] = { "setZoom", false, true },
    [UCFL_METHOD_SET_MAGNIFICATION] = { "setMagnification", false, true },
    [UCFL_METHOD_WHEN_RENDERED] = { "whenRendered", false, true },
    [UCFL_METHOD_TOUCH] = { "touch", false, true },
    [UCFL_METHOD_POINTER_BUTTON] = { "pointerButton", false, true },
    [UCFL_METHOD_POINTER_MOVE] = { "pointerMove", false, true },
    [UCFL_METHOD_SCROLL] = { "scroll", false, true },
    [UCFL_METHOD_KEY] = { "key", false, true },
    [UCFL_METHOD_IME_COMMIT] = { "imeCommit", false, true },
    [UCFL_METHOD_IME_PREEDIT] = { "imePreedit", false, true },
    [UCFL_METHOD_IME_DELETE_SURROUNDING] = { "imeDeleteSurrounding", false, true },
    [UCFL_METHOD_REPLY_DIALOG] = { "replyDialog", false, true },
    [UCFL_METHOD_REPLY_AUTH] = { "replyAuth", false, true },
    [UCFL_METHOD_SET_ORIGIN_HEADERS] = { "setOriginHeaders", false, true },
    [UCFL_METHOD_SET_USER_AGENT] = { "setUserAgent", false, true },
    [UCFL_METHOD_SET_DEVICE_API] = { "setDeviceApi", false, true },
    [UCFL_METHOD_SET_DEVICE_STATE] = { "setDeviceState", false, true },
    [UCFL_METHOD_DEVICE_EVENT] = { "deviceEvent", false, true },
    [UCFL_METHOD_LISTEN] = { "listen", true, false },
    [UCFL_METHOD_CANCEL] = { "cancel", true, false },
};

const char *ucfl_method_name(enum ucfl_method method)
{
    return method > UCFL_METHOD_UNKNOWN && method < UCFL_METHOD_COUNT ? methods[method].name : "";
}

bool ucfl_method_has_view(enum ucfl_method method)
{
    return method > UCFL_METHOD_UNKNOWN && method < UCFL_METHOD_COUNT && methods[method].view;
}

/* ---- argument helpers ------------------------------------------------------------------------------------------- */

#define NEED(cond, msg)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            *error = (msg);                                                                                            \
            return EINVAL;                                                                                             \
        }                                                                                                              \
    } while (0)

#define TRY(expr)                                                                                                      \
    do {                                                                                                               \
        int err_ = (expr);                                                                                             \
        if (err_)                                                                                                      \
            return err_;                                                                                               \
    } while (0)

/* present and not null */
static bool has(const struct ucfl_value *args, const char *key)
{
    const struct ucfl_value *v = ucfl_map_get(args, key);
    return v && v->type != UCFL_NULL;
}

static bool int_in(const struct ucfl_value *v, int64_t min, int64_t max, int64_t *out)
{
    int64_t i;
    if (!ucfl_value_get_int(v, &i) || i < min || i > max)
        return false;
    *out = i;
    return true;
}

static bool get_int(const struct ucfl_value *args, const char *key, int64_t min, int64_t max, int64_t *out)
{
    return int_in(ucfl_map_get(args, key), min, max, out);
}

static bool get_number(const struct ucfl_value *args, const char *key, double *out)
{
    double v;
    if (!ucfl_value_get_number(ucfl_map_get(args, key), &v) || !isfinite(v))
        return false;
    *out = v;
    return true;
}

static bool get_bool(const struct ucfl_value *args, const char *key, bool *out)
{
    return ucfl_value_get_bool(ucfl_map_get(args, key), out);
}

/* an optional bool: absent or null gives fallback */
static bool get_bool_or(const struct ucfl_value *args, const char *key, bool fallback, bool *out)
{
    *out = fallback;
    return !has(args, key) || get_bool(args, key, out);
}

/* a copy of a string value the call owns; 0, EINVAL (no string, a NUL inside) or ENOMEM */
static int own_string(struct ucfl_call *call, const struct ucfl_value *v, char **out)
{
    char *s;

    if (!v || v->type != UCFL_STRING || memchr(v->str.data, '\0', v->str.len))
        return EINVAL;
    if (call->n_owned >= UCFL_CALL_OWNED_MAX)
        return ENOMEM;
    s = ucfl_value_dup_string(v);
    if (!s)
        return ENOMEM;
    call->owned[call->n_owned++] = s;
    *out = s;
    return 0;
}

static void *own_alloc(struct ucfl_call *call, size_t n, size_t size)
{
    void *p;

    if (call->n_owned >= UCFL_CALL_OWNED_MAX)
        return NULL;
    p = calloc(n, size);
    if (p)
        call->owned[call->n_owned++] = p;
    return p;
}

/* a required string argument; *error names it */
static int string_arg(struct ucfl_call *call, const struct ucfl_value *args, const char *key, char **out,
                      const char *message, const char **error)
{
    int err = own_string(call, ucfl_map_get(args, key), out);
    if (err == EINVAL)
        *error = message;
    return err;
}

/* an optional string argument: absent or null leaves *out NULL */
static int optional_string_arg(struct ucfl_call *call, const struct ucfl_value *args, const char *key, char **out,
                               const char *message, const char **error)
{
    *out = NULL;
    return has(args, key) ? string_arg(call, args, key, out, message, error) : 0;
}

/* timeMs, optional: a uint32 wrap of Dart's int (the library takes the low 32 bits as a monotonic time) */
static int time_arg(const struct ucfl_value *args, uint32_t *out, const char **error)
{
    int64_t v = 0;
    NEED(!has(args, "timeMs") || get_int(args, "timeMs", 0, INT64_MAX, &v), "timeMs: an int >= 0 is expected");
    *out = (uint32_t) v;
    return 0;
}

static int modifiers_arg(const struct ucfl_value *args, unsigned *out, const char **error)
{
    int64_t v = 0;
    NEED(!has(args, "modifiers") || get_int(args, "modifiers", 0, UINT32_MAX, &v),
         "modifiers: an int of UC_WEBVIEW_MOD_* bits is expected");
    *out = (unsigned) v;
    return 0;
}

static int position_arg(const struct ucfl_value *args, double *x, double *y, const char **error)
{
    NEED(get_number(args, "x", x) && get_number(args, "y", y), "x, y: finite numbers are required");
    return 0;
}

/* ---- per method ----------------------------------------------------------------------------------------------- */

static int parse_cookies(struct ucfl_call *call, const struct ucfl_value *list, const char **error)
{
    uc_webview_cookie *cookies;

    NEED(list && list->type == UCFL_LIST, "cookies: a list of maps is required");
    NEED(list->list.count <= UCFL_MAX_COOKIES, "cookies: at most 64 cookies");
    call->set_cookies.count = list->list.count;
    if (!list->list.count)
        return 0;
    cookies = own_alloc(call, list->list.count, sizeof(*cookies));
    if (!cookies)
        return ENOMEM;
    call->set_cookies.cookies = cookies;
    for (size_t i = 0; i < list->list.count; i++) {
        const struct ucfl_value *c = &list->list.items[i];
        uc_webview_cookie *out = &cookies[i];
        char *s;
        bool flag;
        int64_t expires = -1;

        NEED(c->type == UCFL_MAP, "cookies: a list of maps is required");
        TRY(string_arg(call, c, "name", &s, "cookies[].name: a string is required", error));
        out->name = s;
        TRY(string_arg(call, c, "value", &s, "cookies[].value: a string is required", error));
        out->value = s;
        TRY(optional_string_arg(call, c, "domain", &s, "cookies[].domain: a string is expected", error));
        out->domain = s;
        TRY(optional_string_arg(call, c, "path", &s, "cookies[].path: a string is expected", error));
        out->path = s;
        NEED(get_bool_or(c, "secure", false, &flag), "cookies[].secure: a bool is expected");
        out->secure = flag;
        NEED(get_bool_or(c, "httpOnly", false, &flag), "cookies[].httpOnly: a bool is expected");
        out->http_only = flag;
        NEED(!has(c, "expires") || get_int(c, "expires", INT64_MIN, INT64_MAX, &expires),
             "cookies[].expires: an int (seconds since 1970) or null is expected");
        out->expires = expires;
    }
    return 0;
}

static int parse_headers(struct ucfl_call *call, const struct ucfl_value *map, const char **error)
{
    NEED(map && map->type == UCFL_MAP, "headers: a map of strings is required");
    NEED(map->map.count <= UCFL_MAX_HEADERS, "headers: at most 32 headers");
    for (size_t i = 0; i < map->map.count; i++) {
        char *name, *value;
        int err = own_string(call, &map->map.entries[2 * i], &name);
        if (!err)
            err = own_string(call, &map->map.entries[2 * i + 1], &value);
        if (err == EINVAL)
            *error = "headers: a map of strings is required";
        if (err)
            return err;
        call->set_origin_headers.names[i] = name;
        call->set_origin_headers.values[i] = value;
    }
    call->set_origin_headers.count = map->map.count;
    return 0;
}

static int parse_args(const struct ucfl_value *args, struct ucfl_call *call, const char **error)
{
    int64_t v;
    double d;

    if (ucfl_method_has_view(call->method))
        NEED(get_int(args, "viewId", 1, INT64_MAX, &call->view_id), "viewId: a positive int is required");

    switch (call->method) {
    case UCFL_METHOD_INITIALIZE:
        NEED(!has(args, "refreshRate") || get_number(args, "refreshRate", &call->initialize.refresh_rate),
             "refreshRate: a number is expected");
        return 0;
    case UCFL_METHOD_SET_REFRESH_RATE:
        NEED(get_number(args, "hz", &call->set_refresh_rate.hz), "hz: a number is required");
        return 0;
    case UCFL_METHOD_SET_COLOR_SCHEME: {
        const struct ucfl_value *scheme = ucfl_map_get(args, "scheme");
        NEED(ucfl_value_is_string(scheme, "light") || ucfl_value_is_string(scheme, "dark"),
             "scheme: \"light\" or \"dark\" is required");
        call->set_color_scheme.scheme =
            ucfl_value_is_string(scheme, "dark") ? UC_WEBVIEW_COLOR_SCHEME_DARK : UC_WEBVIEW_COLOR_SCHEME_LIGHT;
        return 0;
    }
    case UCFL_METHOD_SET_TLS_ERRORS_POLICY:
        NEED(get_bool(args, "ignore", &call->set_tls_errors_policy.ignore), "ignore: a bool is required");
        return 0;
    case UCFL_METHOD_ALLOW_CERTIFICATE:
        TRY(string_arg(call, args, "host", &call->allow_certificate.host, "host: a string is required", error));
        return optional_string_arg(call, args, "sha256", &call->allow_certificate.sha256,
                                   "sha256: a string is expected", error);
    case UCFL_METHOD_SET_COOKIES:
        TRY(string_arg(call, args, "url", &call->set_cookies.url, "url: a string is required", error));
        return parse_cookies(call, ucfl_map_get(args, "cookies"), error);
    case UCFL_METHOD_CLEAR_DATA: {
        const struct ucfl_value *scope = ucfl_map_get(args, "scope");
        NEED(ucfl_value_is_string(scope, "cache") || ucfl_value_is_string(scope, "all"),
             "scope: \"cache\" or \"all\" is required");
        call->clear_data.types = ucfl_value_is_string(scope, "all") ? UC_WEBVIEW_DATA_ALL : UC_WEBVIEW_DATA_CACHE;
        return 0;
    }
    case UCFL_METHOD_CREATE:
        NEED(get_int(args, "width", 1, 16384, &v), "width: an int 1..16384 is required");
        call->create.width = (int) v;
        NEED(get_int(args, "height", 1, 16384, &v), "height: an int 1..16384 is required");
        call->create.height = (int) v;
        TRY(optional_string_arg(call, args, "url", &call->create.url, "url: a string without NUL is expected", error));
        NEED(!has(args, "pixelRatio") || (get_number(args, "pixelRatio", &call->create.pixel_ratio) &&
                                           call->create.pixel_ratio > 0 && call->create.pixel_ratio <= 16),
             "pixelRatio: a number in (0, 16] is expected");
        NEED(!has(args, "refreshRate") || get_number(args, "refreshRate", &call->create.refresh_rate),
             "refreshRate: a number is expected");
        NEED(get_bool_or(args, "pinchZoom", true, &call->create.pinch_zoom), "pinchZoom: a bool is expected");
        NEED(get_bool_or(args, "requests", true, &call->create.requests), "requests: a bool is expected");
        return 0;
    case UCFL_METHOD_RESIZE:
        NEED(get_int(args, "width", 1, 16384, &v), "width: an int 1..16384 is required");
        call->resize.width = (int) v;
        NEED(get_int(args, "height", 1, 16384, &v), "height: an int 1..16384 is required");
        call->resize.height = (int) v;
        return 0;
    case UCFL_METHOD_SET_MAPPED:
        NEED(get_bool(args, "mapped", &call->flag.value), "mapped: a bool is required");
        return 0;
    case UCFL_METHOD_SET_FOCUSED:
        NEED(get_bool(args, "focused", &call->flag.value), "focused: a bool is required");
        return 0;
    case UCFL_METHOD_SET_FRAME_HOLD:
        NEED(get_bool(args, "hold", &call->flag.value), "hold: a bool is required");
        return 0;
    case UCFL_METHOD_SET_DEVICE_API:
        NEED(get_bool(args, "enabled", &call->flag.value), "enabled: a bool is required");
        return 0;
    case UCFL_METHOD_SET_SCALE:
        NEED(get_number(args, "scale", &d) && d > 0 && d <= 16, "scale: a number in (0, 16] is required");
        call->number.value = d;
        return 0;
    case UCFL_METHOD_SET_ZOOM:
        NEED(get_number(args, "zoom", &d) && d > 0 && d <= 16, "zoom: a number in (0, 16] is required");
        call->number.value = d;
        return 0;
    case UCFL_METHOD_SET_BACKGROUND_COLOR:
        NEED(get_int(args, "color", 0, UINT32_MAX, &v), "color: an int 0xAARRGGBB is required");
        call->color.a = (float) ((v >> 24) & 0xff) / 255.0f;
        call->color.r = (float) ((v >> 16) & 0xff) / 255.0f;
        call->color.g = (float) ((v >> 8) & 0xff) / 255.0f;
        call->color.b = (float) (v & 0xff) / 255.0f;
        return 0;
    case UCFL_METHOD_LOAD_URI:
        return string_arg(call, args, "uri", &call->load_uri.uri, "uri: a string is required", error);
    case UCFL_METHOD_LOAD_HTML:
        TRY(string_arg(call, args, "html", &call->load_html.html, "html: a string is required", error));
        return optional_string_arg(call, args, "baseUri", &call->load_html.base_uri, "baseUri: a string is expected",
                                   error);
    case UCFL_METHOD_RUN_JAVASCRIPT:
        return string_arg(call, args, "script", &call->run_javascript.script, "script: a string is required", error);
    case UCFL_METHOD_SET_MAGNIFICATION:
        NEED(get_number(args, "scale", &call->set_magnification.scale) &&
                 call->set_magnification.scale >= UC_WEBVIEW_MAGNIFICATION_MIN &&
                 call->set_magnification.scale <= UC_WEBVIEW_MAGNIFICATION_MAX,
             "scale: a number 0.25..10 is required");
        return position_arg(args, &call->set_magnification.x, &call->set_magnification.y, error);
    case UCFL_METHOD_TOUCH:
        /* Dart's pointer ids count up for the whole process; the library takes an int */
        NEED(get_int(args, "slot", 0, INT64_MAX, &v), "slot: an int >= 0 is required");
        call->touch.slot = (int) (v & 0x7fffffff);
        NEED(get_int(args, "type", UC_WEBVIEW_TOUCH_DOWN, UC_WEBVIEW_TOUCH_CANCEL, &v),
             "type: 0 (down), 1 (move), 2 (up) or 3 (cancel) is required");
        call->touch.type = (int) v;
        TRY(position_arg(args, &call->touch.x, &call->touch.y, error));
        return time_arg(args, &call->touch.time_ms, error);
    case UCFL_METHOD_POINTER_BUTTON:
        NEED(get_int(args, "button", 1, 3, &v), "button: 1 (primary), 2 (middle) or 3 (secondary) is required");
        call->pointer_button.button = (int) v;
        NEED(get_bool(args, "pressed", &call->pointer_button.pressed), "pressed: a bool is required");
        TRY(position_arg(args, &call->pointer_button.x, &call->pointer_button.y, error));
        TRY(modifiers_arg(args, &call->pointer_button.modifiers, error));
        return time_arg(args, &call->pointer_button.time_ms, error);
    case UCFL_METHOD_POINTER_MOVE:
        TRY(position_arg(args, &call->pointer_move.x, &call->pointer_move.y, error));
        TRY(modifiers_arg(args, &call->pointer_move.modifiers, error));
        return time_arg(args, &call->pointer_move.time_ms, error);
    case UCFL_METHOD_SCROLL:
        NEED(get_number(args, "dx", &call->scroll.dx) && get_number(args, "dy", &call->scroll.dy),
             "dx, dy: finite numbers are required");
        TRY(position_arg(args, &call->scroll.x, &call->scroll.y, error));
        NEED(get_bool_or(args, "precise", false, &call->scroll.precise), "precise: a bool is expected");
        return time_arg(args, &call->scroll.time_ms, error);
    case UCFL_METHOD_KEY:
        NEED(get_int(args, "keysym", 0, UINT32_MAX, &v), "keysym: an XKB keysym is required");
        call->key.keysym = (uint32_t) v;
        v = 0;
        NEED(!has(args, "keycode") || get_int(args, "keycode", 0, UINT32_MAX, &v),
             "keycode: an XKB key code is expected");
        call->key.keycode = (uint32_t) v;
        NEED(get_bool(args, "pressed", &call->key.pressed), "pressed: a bool is required");
        TRY(modifiers_arg(args, &call->key.modifiers, error));
        return time_arg(args, &call->key.time_ms, error);
    case UCFL_METHOD_IME_COMMIT:
        return string_arg(call, args, "text", &call->ime_commit.text, "text: a string is required", error);
    case UCFL_METHOD_IME_PREEDIT:
        TRY(optional_string_arg(call, args, "text", &call->ime_preedit.text, "text: a string is expected", error));
        v = 0;
        NEED(!has(args, "cursor") || get_int(args, "cursor", 0, INT32_MAX, &v), "cursor: an int >= 0 is expected");
        call->ime_preedit.cursor = (int) v;
        return 0;
    case UCFL_METHOD_IME_DELETE_SURROUNDING:
        NEED(get_int(args, "offset", INT32_MIN, INT32_MAX, &v), "offset: an int is required");
        call->ime_delete_surrounding.offset = (int) v;
        NEED(get_int(args, "count", 0, INT32_MAX, &v), "count: an int >= 0 is required");
        call->ime_delete_surrounding.count = (int) v;
        return 0;
    case UCFL_METHOD_REPLY_DIALOG:
        NEED(get_int(args, "requestId", 1, INT64_MAX, &call->reply_dialog.request_id),
             "requestId: a positive int is required");
        NEED(get_bool(args, "accept", &call->reply_dialog.accept), "accept: a bool is required");
        return optional_string_arg(call, args, "text", &call->reply_dialog.text, "text: a string is expected", error);
    case UCFL_METHOD_REPLY_AUTH:
        NEED(get_int(args, "requestId", 1, INT64_MAX, &call->reply_auth.request_id),
             "requestId: a positive int is required");
        TRY(optional_string_arg(call, args, "user", &call->reply_auth.user, "user: a string is expected", error));
        return optional_string_arg(call, args, "password", &call->reply_auth.password,
                                   "password: a string is expected", error);
    case UCFL_METHOD_SET_ORIGIN_HEADERS:
        TRY(string_arg(call, args, "origin", &call->set_origin_headers.origin, "origin: a string is required", error));
        return parse_headers(call, ucfl_map_get(args, "headers"), error);
    case UCFL_METHOD_SET_USER_AGENT: {
        static const char *const presets[] = { "default", "mobile", "desktop", "custom" };
        const struct ucfl_value *preset = ucfl_map_get(args, "preset");
        call->set_user_agent.preset = -1;
        for (int i = 0; i < 4; i++) {
            if (ucfl_value_is_string(preset, presets[i]))
                call->set_user_agent.preset = i; /* UC_WEBVIEW_USER_AGENT_DEFAULT .. _CUSTOM */
        }
        NEED(call->set_user_agent.preset >= 0, "preset: \"default\", \"mobile\", \"desktop\" or \"custom\" is required");
        TRY(optional_string_arg(call, args, "custom", &call->set_user_agent.custom, "custom: a string is expected",
                                error));
        NEED(call->set_user_agent.preset != UC_WEBVIEW_USER_AGENT_CUSTOM || call->set_user_agent.custom,
             "custom: a string is required with the preset \"custom\"");
        return 0;
    }
    case UCFL_METHOD_SET_DEVICE_STATE:
        return string_arg(call, args, "json", &call->set_device_state.json, "json: a string (a JSON object) is required",
                          error);
    case UCFL_METHOD_DEVICE_EVENT: {
        const struct ucfl_value *type = ucfl_map_get(args, "type");
        NEED(ucfl_value_is_string(type, "suspend") || ucfl_value_is_string(type, "resume"),
             "type: \"suspend\" or \"resume\" is required");
        TRY(string_arg(call, args, "type", &call->device_event.type, "type: a string is required", error));
        NEED(!has(args, "sleptMs") || get_int(args, "sleptMs", 0, INT64_MAX, &call->device_event.slept_ms),
             "sleptMs: an int >= 0 is expected");
        return 0;
    }
    default:
        return 0; /* hello, stats, dispose, reload, stop, goBack, goForward, getState, whenRendered, listen, cancel */
    }
}

int ucfl_parse_call(const uint8_t *msg, size_t size, bool events, struct ucfl_call *call, const char **error)
{
    struct ucfl_value method, args;
    int err;

    memset(call, 0, sizeof(*call));
    *error = NULL;
    err = ucfl_decode_method_call(msg, size, &method, &args);
    if (err) {
        *error = "not a method call of StandardMethodCodec";
    } else {
        err = ENOTSUP;
        for (int i = UCFL_METHOD_UNKNOWN + 1; i < UCFL_METHOD_COUNT; i++) {
            if (methods[i].events == events && ucfl_value_is_string(&method, methods[i].name)) {
                call->method = (enum ucfl_method) i;
                /* the arguments are a map, or null for a method without any */
                if (args.type != UCFL_MAP && args.type != UCFL_NULL) {
                    *error = "the arguments must be a map";
                    err = EINVAL;
                } else {
                    err = parse_args(&args, call, error);
                }
                break;
            }
        }
    }
    ucfl_value_clear(&method);
    ucfl_value_clear(&args);
    if (err && err != ENOTSUP)
        ucfl_call_clear(call);
    if (err == ENOMEM)
        *error = "out of memory";
    return err;
}

/* overwrites a secret before its memory is freed */
static void wipe(const char *s)
{
    /* through a volatile pointer: a memset() right before free() may be left out by the compiler */
    for (volatile char *p = (volatile char *) s; p && *p; p++)
        *p = '\0';
}

void ucfl_call_clear(struct ucfl_call *call)
{
    switch (call->method) {
    case UCFL_METHOD_REPLY_AUTH:
        wipe(call->reply_auth.password);
        break;
    case UCFL_METHOD_SET_ORIGIN_HEADERS:
        for (size_t i = 0; i < call->set_origin_headers.count; i++)
            wipe(call->set_origin_headers.values[i]);
        break;
    case UCFL_METHOD_SET_COOKIES:
        for (size_t i = 0; i < call->set_cookies.count && call->set_cookies.cookies; i++)
            wipe(call->set_cookies.cookies[i].value);
        break;
    default:
        break;
    }
    for (size_t i = 0; i < call->n_owned; i++)
        free(call->owned[i]);
    memset(call, 0, sizeof(*call));
}

/* ---- replies ---------------------------------------------------------------------------------------------------- */

void ucfl_put_hello_reply(struct ucfl_writer *w, const struct ucfl_hello *hello)
{
    ucfl_put_success_envelope(w);
    ucfl_put_map(w, 6);
    ucfl_put_string(w, "protocol");
    ucfl_put_int(w, UCFL_PROTOCOL_VERSION);
    ucfl_put_string(w, "available");
    ucfl_put_bool(w, hello->available);
    ucfl_put_string(w, "reason");
    ucfl_put_string(w, hello->reason);
    ucfl_put_string(w, "library");
    ucfl_put_string(w, hello->library);
    ucfl_put_string(w, "abi");
    if (hello->abi >= 0)
        ucfl_put_int(w, hello->abi);
    else
        ucfl_put_null(w);
    ucfl_put_string(w, "features");
    ucfl_put_list(w, hello->n_features);
    for (size_t i = 0; i < hello->n_features; i++)
        ucfl_put_string(w, hello->features[i]);
}

void ucfl_put_initialize_reply(struct ucfl_writer *w, int drag_threshold, double refresh_rate)
{
    ucfl_put_success_envelope(w);
    ucfl_put_map(w, 2);
    ucfl_put_string(w, "dragThreshold");
    ucfl_put_int(w, drag_threshold);
    ucfl_put_string(w, "refreshRate");
    if (refresh_rate >= 0)
        ucfl_put_double(w, refresh_rate);
    else
        ucfl_put_null(w);
}

void ucfl_put_create_reply(struct ucfl_writer *w, int64_t view_id, int64_t texture_id)
{
    ucfl_put_success_envelope(w);
    ucfl_put_map(w, 2);
    ucfl_put_string(w, "viewId");
    ucfl_put_int(w, view_id);
    ucfl_put_string(w, "textureId");
    ucfl_put_int(w, texture_id);
}

void ucfl_put_state_reply(struct ucfl_writer *w, const struct ucfl_view_state *state)
{
    ucfl_put_success_envelope(w);
    ucfl_put_map(w, 5);
    ucfl_put_string(w, "uri");
    ucfl_put_string(w, state->uri);
    ucfl_put_string(w, "title");
    ucfl_put_string(w, state->title);
    ucfl_put_string(w, "canGoBack");
    ucfl_put_bool(w, state->can_go_back);
    ucfl_put_string(w, "canGoForward");
    ucfl_put_bool(w, state->can_go_forward);
    ucfl_put_string(w, "magnification");
    if (state->has_magnification)
        ucfl_put_double(w, state->magnification);
    else
        ucfl_put_null(w);
}

void ucfl_put_null_reply(struct ucfl_writer *w)
{
    ucfl_put_success_envelope(w);
    ucfl_put_null(w);
}

void ucfl_put_bool_reply(struct ucfl_writer *w, bool value)
{
    ucfl_put_success_envelope(w);
    ucfl_put_bool(w, value);
}

void ucfl_put_string_reply(struct ucfl_writer *w, const char *value)
{
    ucfl_put_success_envelope(w);
    ucfl_put_string(w, value);
}

void ucfl_put_stats_reply(struct ucfl_writer *w, const struct ucfl_stat *stats, size_t count)
{
    ucfl_put_success_envelope(w);
    ucfl_put_map(w, count);
    for (size_t i = 0; i < count; i++) {
        ucfl_put_string(w, stats[i].name);
        ucfl_put_int(w, stats[i].value);
    }
}

/* ---- events ----------------------------------------------------------------------------------------------------- */

/* the start of an event: envelope, map header, type and viewId */
static void event_start(struct ucfl_writer *w, const char *type, int64_t view_id, size_t n_more)
{
    ucfl_put_success_envelope(w);
    ucfl_put_map(w, 2 + n_more);
    ucfl_put_string(w, "type");
    ucfl_put_string(w, type);
    ucfl_put_string(w, "viewId");
    ucfl_put_int(w, view_id);
}

static void put_entry_string(struct ucfl_writer *w, const char *key, const char *value)
{
    ucfl_put_string(w, key);
    ucfl_put_string(w, value);
}

static void put_entry_int(struct ucfl_writer *w, const char *key, int64_t value)
{
    ucfl_put_string(w, key);
    ucfl_put_int(w, value);
}

static void put_entry_double(struct ucfl_writer *w, const char *key, double value)
{
    ucfl_put_string(w, key);
    ucfl_put_double(w, value);
}

static void put_entry_bool(struct ucfl_writer *w, const char *key, bool value)
{
    ucfl_put_string(w, key);
    ucfl_put_bool(w, value);
}

void ucfl_put_event_load_changed(struct ucfl_writer *w, int64_t view_id, int load_event, const char *uri)
{
    event_start(w, "loadChanged", view_id, 2);
    put_entry_string(w, "event", ucfl_load_event_name(load_event));
    put_entry_string(w, "uri", uri);
}

void ucfl_put_event_load_failed(struct ucfl_writer *w, int64_t view_id, const char *uri, int reason,
                                const char *error_domain, int error_code, unsigned tls_errors, const char *message)
{
    event_start(w, "loadFailed", view_id, 6);
    put_entry_string(w, "uri", uri);
    put_entry_string(w, "reason", ucfl_load_failure_name(reason));
    put_entry_string(w, "errorDomain", error_domain ? error_domain : "");
    put_entry_int(w, "errorCode", error_code);
    put_entry_int(w, "tlsErrors", tls_errors);
    put_entry_string(w, "message", message ? message : "");
}

void ucfl_put_event_progress(struct ucfl_writer *w, int64_t view_id, double progress)
{
    event_start(w, "progress", view_id, 1);
    put_entry_double(w, "progress", progress);
}

void ucfl_put_event_title(struct ucfl_writer *w, int64_t view_id, const char *title)
{
    event_start(w, "titleChanged", view_id, 1);
    put_entry_string(w, "title", title);
}

void ucfl_put_event_history(struct ucfl_writer *w, int64_t view_id, bool can_go_back, bool can_go_forward)
{
    event_start(w, "historyChanged", view_id, 2);
    put_entry_bool(w, "canGoBack", can_go_back);
    put_entry_bool(w, "canGoForward", can_go_forward);
}

void ucfl_put_event_frame(struct ucfl_writer *w, int64_t view_id, int width, int height, bool zero_copy)
{
    event_start(w, "frame", view_id, 3);
    put_entry_int(w, "width", width);
    put_entry_int(w, "height", height);
    put_entry_bool(w, "zeroCopy", zero_copy);
}

void ucfl_put_event_terminated(struct ucfl_writer *w, int64_t view_id, int reason)
{
    event_start(w, "terminated", view_id, 1);
    put_entry_string(w, "reason", ucfl_termination_name(reason));
}

void ucfl_put_event_ime_focus(struct ucfl_writer *w, int64_t view_id, bool focused, unsigned hints, unsigned purpose,
                              const char *surrounding, int cursor)
{
    event_start(w, "imeFocus", view_id, 5);
    put_entry_bool(w, "focused", focused);
    put_entry_int(w, "hints", hints);
    put_entry_string(w, "purpose", ucfl_ime_purpose_name(purpose));
    put_entry_string(w, "surrounding", surrounding);
    put_entry_int(w, "cursor", cursor);
}

void ucfl_put_event_ime_cursor_area(struct ucfl_writer *w, int64_t view_id, int x, int y, int width, int height)
{
    event_start(w, "imeCursorArea", view_id, 4);
    put_entry_int(w, "x", x);
    put_entry_int(w, "y", y);
    put_entry_int(w, "width", width);
    put_entry_int(w, "height", height);
}

void ucfl_put_event_ime_surrounding(struct ucfl_writer *w, int64_t view_id, const char *text, int cursor, int anchor)
{
    event_start(w, "imeSurrounding", view_id, 3);
    put_entry_string(w, "text", text ? text : "");
    put_entry_int(w, "cursor", cursor);
    put_entry_int(w, "anchor", anchor);
}

void ucfl_put_event_cursor(struct ucfl_writer *w, int64_t view_id, const char *css_name)
{
    event_start(w, "cursorChanged", view_id, 1);
    put_entry_string(w, "cursor", css_name ? css_name : "default");
}

void ucfl_put_event_pinch(struct ucfl_writer *w, int64_t view_id, int phase, double scale, double x, double y)
{
    event_start(w, "pinch", view_id, 4);
    put_entry_string(w, "phase", ucfl_pinch_phase_name(phase));
    put_entry_double(w, "scale", scale);
    put_entry_double(w, "x", x);
    put_entry_double(w, "y", y);
}

void ucfl_put_event_magnification(struct ucfl_writer *w, int64_t view_id, double magnification)
{
    event_start(w, "magnificationChanged", view_id, 1);
    put_entry_double(w, "magnification", magnification);
}

void ucfl_put_event_script_dialog(struct ucfl_writer *w, int64_t view_id, int64_t request_id, int dialog_type,
                                  const char *origin, const char *message, const char *default_text)
{
    const char *name = ucfl_dialog_type_name(dialog_type);

    event_start(w, "scriptDialog", view_id, 5);
    put_entry_int(w, "requestId", request_id);
    put_entry_string(w, "dialogType", name ? name : "unknown");
    put_entry_string(w, "origin", origin);
    put_entry_string(w, "message", message);
    put_entry_string(w, "defaultText", default_text);
}

void ucfl_put_event_authenticate(struct ucfl_writer *w, int64_t view_id, int64_t request_id, const char *origin,
                                 const char *realm, int scheme, unsigned flags)
{
    event_start(w, "authenticate", view_id, 6);
    put_entry_int(w, "requestId", request_id);
    put_entry_string(w, "origin", origin);
    put_entry_string(w, "realm", realm);
    put_entry_string(w, "scheme", ucfl_auth_scheme_name(scheme));
    put_entry_bool(w, "retry", (flags & UC_WEBVIEW_AUTH_RETRY) != 0);
    put_entry_bool(w, "proxy", (flags & UC_WEBVIEW_AUTH_PROXY) != 0);
}

void ucfl_put_event_request_cancelled(struct ucfl_writer *w, int64_t view_id, int64_t request_id, const char *reason)
{
    event_start(w, "requestCancelled", view_id, 2);
    put_entry_int(w, "requestId", request_id);
    put_entry_string(w, "reason", reason);
}

void ucfl_put_event_close_requested(struct ucfl_writer *w, int64_t view_id)
{
    event_start(w, "closeRequested", view_id, 0);
}

void ucfl_put_event_new_window(struct ucfl_writer *w, int64_t view_id, const char *uri, int result)
{
    event_start(w, "newWindow", view_id, 2);
    put_entry_string(w, "uri", uri);
    put_entry_bool(w, "loaded", result == UC_WEBVIEW_NEW_WINDOW_LOADED);
}

void ucfl_put_event_response_refused(struct ucfl_writer *w, int64_t view_id, const char *uri, const char *mime_type,
                                     int reason)
{
    event_start(w, "responseRefused", view_id, 3);
    put_entry_string(w, "uri", uri);
    put_entry_string(w, "mimeType", mime_type);
    put_entry_string(w, "reason", ucfl_response_refused_name(reason));
}

/* ---- names ------------------------------------------------------------------------------------------------------ */

static const char *name_of(int value, const char *const *names, size_t n)
{
    return value >= 0 && (size_t) value < n && names[value] ? names[value] : "unknown";
}

#define NAME_OF(value, names) name_of((value), (names), sizeof(names) / sizeof((names)[0]))

const char *ucfl_load_event_name(int load_event)
{
    static const char *const names[] = {
        [UC_WEBVIEW_LOAD_STARTED] = "started",     [UC_WEBVIEW_LOAD_REDIRECTED] = "redirected",
        [UC_WEBVIEW_LOAD_COMMITTED] = "committed", [UC_WEBVIEW_LOAD_FINISHED] = "finished",
        [UC_WEBVIEW_LOAD_FAILED] = "failed",
    };
    return NAME_OF(load_event, names);
}

const char *ucfl_load_failure_name(int reason)
{
    static const char *const names[] = {
        [UC_WEBVIEW_FAIL_OTHER] = "other",
        [UC_WEBVIEW_FAIL_HOST_NOT_FOUND] = "hostNotFound",
        [UC_WEBVIEW_FAIL_NO_NETWORK] = "noNetwork",
        [UC_WEBVIEW_FAIL_CONNECTION_REFUSED] = "connectionRefused",
        [UC_WEBVIEW_FAIL_TIMEOUT] = "timeout",
        [UC_WEBVIEW_FAIL_CERTIFICATE] = "certificate",
        [UC_WEBVIEW_FAIL_SCHEME_REFUSED] = "schemeRefused",
    };
    return NAME_OF(reason, names);
}

const char *ucfl_termination_name(int reason)
{
    static const char *const names[] = {
        [UC_WEBVIEW_TERMINATED_CRASHED] = "crashed",
        [UC_WEBVIEW_TERMINATED_EXCEEDED_MEMORY_LIMIT] = "exceededMemoryLimit",
        [UC_WEBVIEW_TERMINATED_BY_API] = "byApi",
    };
    return NAME_OF(reason, names);
}

const char *ucfl_ime_purpose_name(unsigned purpose)
{
    static const char *const names[] = {
        [UC_WEBVIEW_IME_PURPOSE_FREE_FORM] = "freeForm", [UC_WEBVIEW_IME_PURPOSE_ALPHA] = "alpha",
        [UC_WEBVIEW_IME_PURPOSE_DIGITS] = "digits",      [UC_WEBVIEW_IME_PURPOSE_NUMBER] = "number",
        [UC_WEBVIEW_IME_PURPOSE_PHONE] = "phone",        [UC_WEBVIEW_IME_PURPOSE_URL] = "url",
        [UC_WEBVIEW_IME_PURPOSE_EMAIL] = "email",        [UC_WEBVIEW_IME_PURPOSE_NAME] = "name",
        [UC_WEBVIEW_IME_PURPOSE_PASSWORD] = "password",  [UC_WEBVIEW_IME_PURPOSE_PIN] = "pin",
        [UC_WEBVIEW_IME_PURPOSE_TERMINAL] = "terminal",  [UC_WEBVIEW_IME_PURPOSE_SEARCH] = "search",
    };
    return purpose <= INT32_MAX ? NAME_OF((int) purpose, names) : "unknown";
}

const char *ucfl_pinch_phase_name(int phase)
{
    static const char *const names[] = {
        [UC_WEBVIEW_PINCH_BEGIN] = "begin",
        [UC_WEBVIEW_PINCH_UPDATE] = "update",
        [UC_WEBVIEW_PINCH_END] = "end",
        [UC_WEBVIEW_PINCH_CANCEL] = "cancel",
    };
    return NAME_OF(phase, names);
}

const char *ucfl_dialog_type_name(int type)
{
    static const char *const names[] = {
        [UC_WEBVIEW_DIALOG_ALERT] = "alert",
        [UC_WEBVIEW_DIALOG_CONFIRM] = "confirm",
        [UC_WEBVIEW_DIALOG_PROMPT] = "prompt",
        [UC_WEBVIEW_DIALOG_BEFORE_UNLOAD] = "beforeUnload",
    };
    const char *name = NAME_OF(type, names);
    return strcmp(name, "unknown") == 0 ? NULL : name;
}

const char *ucfl_auth_scheme_name(int scheme)
{
    switch (scheme) {
    case UC_WEBVIEW_AUTH_SCHEME_DEFAULT:
        return "default";
    case UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC:
        return "basic";
    case UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST:
        return "digest";
    case UC_WEBVIEW_AUTH_SCHEME_HTML_FORM:
        return "htmlForm";
    case UC_WEBVIEW_AUTH_SCHEME_NTLM:
        return "ntlm";
    case UC_WEBVIEW_AUTH_SCHEME_NEGOTIATE:
        return "negotiate";
    case UC_WEBVIEW_AUTH_SCHEME_SERVER_TRUST:
        return "serverTrust";
    default:
        return "unknown"; /* UC_WEBVIEW_AUTH_SCHEME_UNKNOWN and values of a newer library */
    }
}

const char *ucfl_response_refused_name(int reason)
{
    static const char *const names[] = {
        [UC_WEBVIEW_RESPONSE_DOWNLOAD] = "download",
        [UC_WEBVIEW_RESPONSE_UNSUPPORTED] = "unsupported",
    };
    return NAME_OF(reason, names);
}

const char *ucfl_rendered_name(int status)
{
    static const char *const names[] = {
        [UC_WEBVIEW_RENDERED_DONE] = "done",
        [UC_WEBVIEW_RENDERED_TIMEOUT] = "timeout",
        [UC_WEBVIEW_RENDERED_CANCELLED] = "cancelled",
    };
    return NAME_OF(status, names);
}

/* ---- log helper ------------------------------------------------------------------------------------------------- */

void ucfl_log_origin(const char *uri, char *out, size_t out_size)
{
    const char *p, *authority, *end, *at;
    size_t scheme_len;

    if (!out_size)
        return;
    if (!uri) {
        snprintf(out, out_size, "(none)");
        return;
    }
    /* scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" (RFC 3986) */
    for (p = uri; (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (p > uri && ((*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.'));
         p++) {
    }
    if (p == uri || *p != ':') {
        snprintf(out, out_size, "(invalid)");
        return;
    }
    scheme_len = (size_t) (p - uri);
    if (p[1] != '/' || p[2] != '/') {
        snprintf(out, out_size, "%.*s:", (int) scheme_len, uri);
        return;
    }
    authority = p + 3;
    end = authority + strcspn(authority, "/?#");
    /* user info goes: the last '@' before the end of the authority */
    for (at = end; at > authority && at[-1] != '@'; at--) {
    }
    if (at > authority)
        authority = at;
    snprintf(out, out_size, "%.*s://%.*s", (int) scheme_len, uri, (int) (end - authority), authority);
}
