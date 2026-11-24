/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The uc.webview channel protocol, see ucfl_protocol.h.
 */
#include "ucfl_protocol.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "uc_webview.h"

static const struct {
    const char *name;
    enum ucfl_method method;
    bool events; /* a call of the event channel */
} methods[] = {
    { "hello", UCFL_METHOD_HELLO, false },
    { "create", UCFL_METHOD_CREATE, false },
    { "touch", UCFL_METHOD_TOUCH, false },
    { "key", UCFL_METHOD_KEY, false },
    { "setMapped", UCFL_METHOD_SET_MAPPED, false },
    { "dispose", UCFL_METHOD_DISPOSE, false },
    { "stats", UCFL_METHOD_STATS, false },
    { "listen", UCFL_METHOD_LISTEN, true },
    { "cancel", UCFL_METHOD_CANCEL, true },
};

static bool int_arg(const struct ucfl_value *args, const char *key, int64_t min, int64_t max, int64_t *out)
{
    int64_t v;
    if (!ucfl_value_get_int(ucfl_map_get(args, key), &v) || v < min || v > max)
        return false;
    *out = v;
    return true;
}

/* an optional argument: absent or null keeps *out */
static bool optional(const struct ucfl_value *args, const char *key)
{
    const struct ucfl_value *v = ucfl_map_get(args, key);
    return v && v->type != UCFL_NULL;
}

static bool number_arg(const struct ucfl_value *args, const char *key, double *out)
{
    double v;
    if (!ucfl_value_get_number(ucfl_map_get(args, key), &v) || !isfinite(v))
        return false;
    *out = v;
    return true;
}

static int parse_args(const struct ucfl_value *args, struct ucfl_call *call, const char **error)
{
    int64_t v;

    switch (call->method) {
    case UCFL_METHOD_TOUCH:
    case UCFL_METHOD_KEY:
    case UCFL_METHOD_SET_MAPPED:
    case UCFL_METHOD_DISPOSE:
        if (!int_arg(args, "viewId", 1, INT64_MAX, &call->view_id)) {
            *error = "viewId: a positive int is required";
            return EINVAL;
        }
        break;
    default:
        break;
    }

    switch (call->method) {
    case UCFL_METHOD_CREATE:
        if (!int_arg(args, "width", 1, 16384, &v)) {
            *error = "width: an int 1..16384 is required";
            return EINVAL;
        }
        call->create.width = (int) v;
        if (!int_arg(args, "height", 1, 16384, &v)) {
            *error = "height: an int 1..16384 is required";
            return EINVAL;
        }
        call->create.height = (int) v;
        if (optional(args, "url") && !(call->create.url = ucfl_value_dup_string(ucfl_map_get(args, "url")))) {
            *error = "url: a string without NUL is expected";
            return EINVAL;
        }
        if (optional(args, "pixelRatio") &&
            (!number_arg(args, "pixelRatio", &call->create.pixel_ratio) || call->create.pixel_ratio <= 0 ||
             call->create.pixel_ratio > 16)) {
            *error = "pixelRatio: a number in (0, 16] is expected";
            return EINVAL;
        }
        if (optional(args, "refreshRate") && !number_arg(args, "refreshRate", &call->create.refresh_rate)) {
            *error = "refreshRate: a number is expected";
            return EINVAL;
        }
        return 0;
    case UCFL_METHOD_TOUCH:
        /* Dart's pointer ids count up from 1 for the whole process; the library takes an int */
        if (!int_arg(args, "slot", 0, INT64_MAX, &v)) {
            *error = "slot: an int >= 0 is required";
            return EINVAL;
        }
        call->touch.slot = (int) (v & 0x7fffffff);
        if (!int_arg(args, "type", UC_WEBVIEW_TOUCH_DOWN, UC_WEBVIEW_TOUCH_CANCEL, &v)) {
            *error = "type: 0 (down), 1 (move), 2 (up) or 3 (cancel) is required";
            return EINVAL;
        }
        call->touch.type = (int) v;
        if (!number_arg(args, "x", &call->touch.x) || !number_arg(args, "y", &call->touch.y)) {
            *error = "x, y: finite numbers are required";
            return EINVAL;
        }
        if (optional(args, "timeMs") && !ucfl_value_get_int(ucfl_map_get(args, "timeMs"), &v)) {
            *error = "timeMs: an int is expected";
            return EINVAL;
        }
        call->touch.time_ms = optional(args, "timeMs") ? (uint32_t) v : 0;
        return 0;
    case UCFL_METHOD_KEY:
        if (!int_arg(args, "keysym", 0, UINT32_MAX, &v)) {
            *error = "keysym: an XKB keysym is required";
            return EINVAL;
        }
        call->key.keysym = (uint32_t) v;
        if (optional(args, "keycode")) {
            if (!int_arg(args, "keycode", 0, UINT32_MAX, &v)) {
                *error = "keycode: an XKB key code is expected";
                return EINVAL;
            }
            call->key.keycode = (uint32_t) v;
        }
        if (!ucfl_value_get_bool(ucfl_map_get(args, "pressed"), &call->key.pressed)) {
            *error = "pressed: a bool is required";
            return EINVAL;
        }
        return 0;
    case UCFL_METHOD_SET_MAPPED:
        if (!ucfl_value_get_bool(ucfl_map_get(args, "mapped"), &call->set_mapped.mapped)) {
            *error = "mapped: a bool is required";
            return EINVAL;
        }
        return 0;
    default:
        return 0; /* hello, dispose, stats, listen, cancel: nothing more */
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
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
            if (methods[i].events == events && ucfl_value_is_string(&method, methods[i].name)) {
                call->method = methods[i].method;
                err = parse_args(&args, call, error);
                break;
            }
        }
    }
    ucfl_value_clear(&method);
    ucfl_value_clear(&args);
    if (err && err != ENOTSUP) {
        ucfl_call_clear(call);
    }
    return err;
}

void ucfl_call_clear(struct ucfl_call *call)
{
    if (call->method == UCFL_METHOD_CREATE)
        free(call->create.url);
    memset(call, 0, sizeof(*call));
}

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

void ucfl_put_create_reply(struct ucfl_writer *w, int64_t view_id, int64_t texture_id)
{
    ucfl_put_success_envelope(w);
    ucfl_put_map(w, 2);
    ucfl_put_string(w, "viewId");
    ucfl_put_int(w, view_id);
    ucfl_put_string(w, "textureId");
    ucfl_put_int(w, texture_id);
}

void ucfl_put_null_reply(struct ucfl_writer *w)
{
    ucfl_put_success_envelope(w);
    ucfl_put_null(w);
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

void ucfl_put_event_load_changed(struct ucfl_writer *w, int64_t view_id, int load_event, const char *uri)
{
    event_start(w, "loadChanged", view_id, 2);
    ucfl_put_string(w, "event");
    ucfl_put_string(w, ucfl_load_event_name(load_event));
    ucfl_put_string(w, "uri");
    ucfl_put_string(w, uri);
}

void ucfl_put_event_progress(struct ucfl_writer *w, int64_t view_id, double progress)
{
    event_start(w, "progress", view_id, 1);
    ucfl_put_string(w, "progress");
    ucfl_put_double(w, progress);
}

void ucfl_put_event_frame(struct ucfl_writer *w, int64_t view_id, int width, int height, bool zero_copy)
{
    event_start(w, "frame", view_id, 3);
    ucfl_put_string(w, "width");
    ucfl_put_int(w, width);
    ucfl_put_string(w, "height");
    ucfl_put_int(w, height);
    ucfl_put_string(w, "zeroCopy");
    ucfl_put_bool(w, zero_copy);
}

void ucfl_put_event_terminated(struct ucfl_writer *w, int64_t view_id, int reason)
{
    event_start(w, "terminated", view_id, 1);
    ucfl_put_string(w, "reason");
    ucfl_put_string(w, ucfl_termination_name(reason));
}

const char *ucfl_load_event_name(int load_event)
{
    switch (load_event) {
    case UC_WEBVIEW_LOAD_STARTED:
        return "started";
    case UC_WEBVIEW_LOAD_REDIRECTED:
        return "redirected";
    case UC_WEBVIEW_LOAD_COMMITTED:
        return "committed";
    case UC_WEBVIEW_LOAD_FINISHED:
        return "finished";
    case UC_WEBVIEW_LOAD_FAILED:
        return "failed";
    default:
        return "unknown"; /* a value of a newer library (uc_webview.h, "Compatibility") */
    }
}

const char *ucfl_termination_name(int reason)
{
    switch (reason) {
    case UC_WEBVIEW_TERMINATED_CRASHED:
        return "crashed";
    case UC_WEBVIEW_TERMINATED_EXCEEDED_MEMORY_LIMIT:
        return "exceededMemoryLimit";
    case UC_WEBVIEW_TERMINATED_BY_API:
        return "byApi";
    default:
        return "unknown";
    }
}
