/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * dlopen() loader of libucwebview.so.1: the host links nothing of the library, so a firmware without it (or with an
 * incompatible one) gives "web view unavailable" instead of a binary that does not start. Required symbols are those
 * of the first release of API version 1 (every function remote-ui's webviewlibrary.cpp resolves as required); later
 * additions within the version are optional and resolved one by one (uc_webview.h, "Compatibility"), and the
 * features they make up are decided as remote-ui decides them (pinch zoom, requests and the device API need several
 * functions each, the site settings one each).
 *
 * The member types come from the declarations in uc_webview.h (__typeof__), so a changed signature in the header is
 * a compile error here instead of a wrong call.
 *
 * Thread: the thread that will run uc_webview_global_init() (dlopen itself is thread-safe).
 */
#ifndef UCFL_LOADER_H
#define UCFL_LOADER_H

#include <stdbool.h>
#include <stddef.h>

#include "uc_webview.h"

/* a member named field with the type of a pointer to uc_webview_<name> */
#define UCFL_FN(field, name) __typeof__(&uc_webview_##name) field

struct ucfl_lib {
    void *handle;

    /* required: API version 1 as first released */
    UCFL_FN(api_version, api_version);
    UCFL_FN(global_init, global_init);
    UCFL_FN(global_shutdown, global_shutdown);
    UCFL_FN(create, new);
    UCFL_FN(destroy, free);
    UCFL_FN(resize, resize);
    UCFL_FN(set_mapped, set_mapped);
    UCFL_FN(set_focused, set_focused);
    UCFL_FN(set_scale, set_scale);
    UCFL_FN(load_uri, load_uri);
    UCFL_FN(load_html, load_html);
    UCFL_FN(reload, reload);
    UCFL_FN(stop, stop);
    UCFL_FN(go_back, go_back);
    UCFL_FN(go_forward, go_forward);
    UCFL_FN(can_go_back, can_go_back);
    UCFL_FN(can_go_forward, can_go_forward);
    UCFL_FN(get_uri, get_uri);
    UCFL_FN(get_title, get_title);
    UCFL_FN(run_javascript, run_javascript);
    UCFL_FN(set_zoom, set_zoom);
    UCFL_FN(set_background_rgba, set_background_rgba);
    UCFL_FN(frame_rendered, frame_rendered);
    UCFL_FN(frame_release, frame_release);
    UCFL_FN(touch, touch);
    UCFL_FN(pointer_button, pointer_button);
    UCFL_FN(pointer_move, pointer_move);
    UCFL_FN(scroll, scroll);
    UCFL_FN(key, key);
    UCFL_FN(ime_commit, ime_commit);
    UCFL_FN(ime_preedit, ime_preedit);
    UCFL_FN(ime_delete_surrounding, ime_delete_surrounding);

    /* optional, NULL when the library is older (the release that added each) */
    UCFL_FN(global_set_refresh_rate, global_set_refresh_rate);           /* 0.2.0 */
    UCFL_FN(global_get_refresh_rate, global_get_refresh_rate);           /* 0.2.0 */
    UCFL_FN(global_get_drag_threshold, global_get_drag_threshold);       /* 0.3.0 */
    UCFL_FN(create_sized, new_sized);                                    /* 0.4.0 */
    UCFL_FN(when_rendered, when_rendered);                               /* 0.5.0 */
    UCFL_FN(set_magnification, set_magnification);                       /* 0.6.0 */
    UCFL_FN(get_magnification, get_magnification);                       /* 0.6.0 */
    UCFL_FN(request_reply, request_reply);                               /* 0.7.0 */
    UCFL_FN(request_auth, request_auth);                                 /* 0.7.0 */
    UCFL_FN(set_origin_headers, set_origin_headers);                     /* 0.8.0 */
    UCFL_FN(set_user_agent, set_user_agent);                             /* 0.8.0 */
    UCFL_FN(global_set_cookies, global_set_cookies);                     /* 0.8.0 */
    UCFL_FN(global_allow_certificate, global_allow_certificate);         /* 0.8.0 */
    UCFL_FN(global_set_tls_errors_policy, global_set_tls_errors_policy); /* 0.8.0 */
    UCFL_FN(global_set_color_scheme, global_set_color_scheme);           /* 0.8.0 */
    UCFL_FN(global_clear_data, global_clear_data);                       /* 0.8.0 */
    UCFL_FN(set_device_api, set_device_api);                             /* 0.9.0 */
    UCFL_FN(set_device_state, set_device_state);                         /* 0.9.0 */
    UCFL_FN(device_event, device_event);                                 /* 0.9.0 */
    const char *(*library_version)(void);                                /* 0.11.0, hello's library */

    unsigned features; /* UCFL_FEATURE_* bits found */
};

#undef UCFL_FN

/*
 * Optional parts of the library, as the protocol's hello reply names them (ucfl_feature_names()) and as remote-ui's
 * WebViewLibrary decides them. A method of the protocol that needs a feature the library lacks is answered with the
 * error "unsupported".
 */
enum {
    UCFL_FEATURE_REFRESH_RATE = 1u << 0,       /* "refreshRate": global_set_refresh_rate, 0.2.0 */
    UCFL_FEATURE_DRAG_THRESHOLD = 1u << 1,     /* "dragThreshold": global_get_drag_threshold, 0.3.0 */
    UCFL_FEATURE_NEW_SIZED = 1u << 2,          /* "newSized": new_sized, 0.4.0 (callbacks after API 1 reach it) */
    UCFL_FEATURE_WHEN_RENDERED = 1u << 3,      /* "whenRendered": when_rendered, 0.5.0 */
    UCFL_FEATURE_PINCH_ZOOM = 1u << 4,         /* "pinchZoom": new_sized, set/get_magnification, when_rendered */
    UCFL_FEATURE_REQUESTS = 1u << 5,           /* "requests": new_sized, request_reply, request_auth (0.7.0) */
    UCFL_FEATURE_ORIGIN_HEADERS = 1u << 6,     /* "originHeaders": set_origin_headers, 0.8.0 */
    UCFL_FEATURE_USER_AGENT = 1u << 7,         /* "userAgent": set_user_agent, 0.8.0 */
    UCFL_FEATURE_COOKIES = 1u << 8,            /* "cookies": global_set_cookies, 0.8.0 */
    UCFL_FEATURE_ALLOW_CERTIFICATE = 1u << 9,  /* "allowCertificate": global_allow_certificate, 0.8.0 */
    UCFL_FEATURE_TLS_ERRORS_POLICY = 1u << 10, /* "tlsErrorsPolicy": global_set_tls_errors_policy, 0.8.0 */
    UCFL_FEATURE_COLOR_SCHEME = 1u << 11,      /* "colorScheme": global_set_color_scheme, 0.8.0 */
    UCFL_FEATURE_CLEAR_DATA = 1u << 12,        /* "clearData": global_clear_data, 0.8.0 */
    UCFL_FEATURE_DEVICE_API = 1u << 13,        /* "deviceApi": set_device_api, set_device_state, device_event */
};
#define UCFL_FEATURE_COUNT 14

/*
 * Loads the library from path (a soname such as UC_WEBVIEW_SONAME is searched like dlopen() does) and resolves its
 * symbols. Returns true; false with a reason in reason[reason_size] when the library is missing, lacks a required
 * symbol or has another API version than UC_WEBVIEW_API_VERSION (the library is closed again then).
 * The library is opened with RTLD_NOW | RTLD_LOCAL and never closed after a successful load: WebKit and GLib keep
 * process-wide state that does not survive an unload.
 */
bool ucfl_lib_load(struct ucfl_lib *lib, const char *path, char *reason, size_t reason_size);

/* The UCFL_FEATURE_* bits of the resolved functions (ucfl_lib_load() sets lib->features with it). */
unsigned ucfl_lib_features(const struct ucfl_lib *lib);

/* The names of the features in bits, in bit order; returns the count (<= max). */
size_t ucfl_feature_names(unsigned bits, const char **names, size_t max);

/* The name of one feature bit, NULL for a value that is not exactly one feature bit. */
const char *ucfl_feature_name(unsigned bit);

#endif /* UCFL_LOADER_H */
