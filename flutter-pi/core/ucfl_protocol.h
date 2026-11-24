/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The uc.webview channel protocol, version 1 (spike subset), shared by every host adapter.
 *
 * Two plain SDK channels:
 *  - UCFL_CHANNEL_CONTROL, a MethodChannel with StandardMethodCodec: calls down, one reply each.
 *  - UCFL_CHANNEL_EVENTS, an EventChannel (StandardMethodCodec): "listen"/"cancel" calls down, events up as success
 *    envelopes; every event is a map with "type" and "viewId".
 * Frames never cross the channel: a view's pixels reach the UI as a texture whose id create returns.
 *
 * Methods (arguments are one map; numbers may be ints or doubles where a double is named):
 *   hello                                   -> {protocol: int, available: bool, reason: String?, library: String?,
 *                                               abi: int?, features: List<String>}
 *   create {width: int, height: int,        -> {viewId: int, textureId: int}
 *           url: String?, pixelRatio: double?, refreshRate: double?}
 *           width/height in device pixels; pixelRatio CSS px to device px; refreshRate of the display in Hz (taken
 *           before the first page load of the process only, see uc_webview_global_set_refresh_rate())
 *   touch {viewId, slot: int, type: int,    -> null      type: 0 down, 1 move, 2 up, 3 cancel (UC_WEBVIEW_TOUCH_*);
 *          x: double, y: double, timeMs: int?}           x, y in view (device) pixels from the top left
 *   key {viewId, keysym: int, keycode: int?, pressed: bool} -> null   XKB keysym, keycode = evdev code + 8 or 0
 *   setMapped {viewId, mapped: bool}        -> null
 *   dispose {viewId}                        -> null
 *   stats                                   -> {name: int, ...} counters of the host adapter
 * Errors: code "badMessage" (not a method call), "badArguments" (message names the argument), "unavailable" (no
 * library), "noView" (unknown viewId), "failed" (the library refused). An unknown method gets the empty reply
 * (MissingPluginException in Dart).
 *
 * Events:
 *   {type: "loadChanged", viewId, event: "started"|"redirected"|"committed"|"finished"|"failed"|"unknown",
 *    uri: String?}
 *   {type: "progress", viewId, progress: double}            0.0 .. 1.0
 *   {type: "frame", viewId, width: int, height: int, zeroCopy: bool}
 *                                                            the first frame, and a frame whose size or kind changed
 *   {type: "terminated", viewId, reason: "crashed"|"exceededMemoryLimit"|"byApi"|"unknown"}
 *
 * Thread: any, no global state.
 */
#ifndef UCFL_PROTOCOL_H
#define UCFL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ucfl_codec.h"

#define UCFL_PROTOCOL_VERSION 1
#define UCFL_CHANNEL_CONTROL "uc.webview/control"
#define UCFL_CHANNEL_EVENTS "uc.webview/events"

enum ucfl_method {
    UCFL_METHOD_UNKNOWN = 0,
    UCFL_METHOD_HELLO,
    UCFL_METHOD_CREATE,
    UCFL_METHOD_TOUCH,
    UCFL_METHOD_KEY,
    UCFL_METHOD_SET_MAPPED,
    UCFL_METHOD_DISPOSE,
    UCFL_METHOD_STATS,
    /* the EventChannel's calls */
    UCFL_METHOD_LISTEN,
    UCFL_METHOD_CANCEL,
};

/* A parsed call. Only the part of the union of method is set. */
struct ucfl_call {
    enum ucfl_method method;
    int64_t view_id; /* touch, key, setMapped, dispose */
    union {
        struct {
            int width, height;
            char *url;           /* malloc()ed, NULL when not given; ucfl_call_clear() frees it */
            double pixel_ratio;  /* 0 when not given */
            double refresh_rate; /* 0 when not given */
        } create;
        struct {
            int slot;
            int type;
            double x, y;
            uint32_t time_ms;
        } touch;
        struct {
            uint32_t keysym, keycode;
            bool pressed;
        } key;
        struct {
            bool mapped;
        } set_mapped;
    };
};

/* Decodes and checks a method call of channel UCFL_CHANNEL_CONTROL (events = false) or UCFL_CHANNEL_EVENTS (true).
 * Returns 0; EBADMSG for a message that is no method call; ENOTSUP for an unknown method (call->method UNKNOWN);
 * EINVAL for invalid arguments, *error then names the problem (a static string); ENOMEM. */
int ucfl_parse_call(const uint8_t *msg, size_t size, bool events, struct ucfl_call *call, const char **error);
void ucfl_call_clear(struct ucfl_call *call);

/* the hello reply */
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

/* complete replies (envelope included) */
void ucfl_put_hello_reply(struct ucfl_writer *w, const struct ucfl_hello *hello);
void ucfl_put_create_reply(struct ucfl_writer *w, int64_t view_id, int64_t texture_id);
void ucfl_put_null_reply(struct ucfl_writer *w);
void ucfl_put_stats_reply(struct ucfl_writer *w, const struct ucfl_stat *stats, size_t count);

/* complete events (envelope included) */
void ucfl_put_event_load_changed(struct ucfl_writer *w, int64_t view_id, int load_event, const char *uri);
void ucfl_put_event_progress(struct ucfl_writer *w, int64_t view_id, double progress);
void ucfl_put_event_frame(struct ucfl_writer *w, int64_t view_id, int width, int height, bool zero_copy);
void ucfl_put_event_terminated(struct ucfl_writer *w, int64_t view_id, int reason);

/* the names of the library's constants in the events */
const char *ucfl_load_event_name(int load_event);
const char *ucfl_termination_name(int reason);

#endif /* UCFL_PROTOCOL_H */
