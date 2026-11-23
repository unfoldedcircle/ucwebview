/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * dlopen() loader of libucwebview.so.1: the host links nothing of the library, so a firmware without it (or with an
 * incompatible one) gives "web view unavailable" instead of a binary that does not start. Required symbols are those
 * of the first release of API version 1; later additions within the version are optional and resolved one by one
 * (uc_webview.h, "Compatibility"), as remote-ui's webviewlibrary.cpp does.
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
    UCFL_FN(create, new);
    UCFL_FN(destroy, free);
    UCFL_FN(resize, resize);
    UCFL_FN(set_mapped, set_mapped);
    UCFL_FN(set_focused, set_focused);
    UCFL_FN(set_scale, set_scale);
    UCFL_FN(load_uri, load_uri);
    UCFL_FN(frame_rendered, frame_rendered);
    UCFL_FN(frame_release, frame_release);
    UCFL_FN(touch, touch);
    UCFL_FN(key, key);

    /* optional, NULL when the library is older */
    UCFL_FN(global_set_refresh_rate, global_set_refresh_rate); /* 0.2.0 */
    UCFL_FN(create_sized, new_sized);               /* 0.4.0 */
    /* Not in any release up to 0.10.3: a version string for the hello reply, if a later release adds it. */
    const char *(*library_version)(void);

    unsigned features; /* UCFL_FEATURE_* bits found */
};

#undef UCFL_FN

/* Optional parts of the library, by the function that marks them (names in ucfl_feature_names()). */
enum {
    UCFL_FEATURE_REFRESH_RATE = 1u << 0,   /* uc_webview_global_set_refresh_rate, 0.2.0 */
    UCFL_FEATURE_DRAG_THRESHOLD = 1u << 1, /* uc_webview_global_get_drag_threshold, 0.3.0 */
    UCFL_FEATURE_NEW_SIZED = 1u << 2,      /* uc_webview_new_sized, 0.4.0 */
    UCFL_FEATURE_WHEN_RENDERED = 1u << 3,  /* uc_webview_when_rendered, 0.5.0 */
    UCFL_FEATURE_PINCH_ZOOM = 1u << 4,     /* uc_webview_set_magnification, 0.6.0 */
    UCFL_FEATURE_REQUESTS = 1u << 5,       /* uc_webview_request_reply, 0.7.0 */
    UCFL_FEATURE_SITE_SETTINGS = 1u << 6,  /* uc_webview_set_origin_headers, 0.8.0 */
    UCFL_FEATURE_DEVICE_API = 1u << 7,     /* uc_webview_set_device_api, 0.9.0 */
};
#define UCFL_FEATURE_COUNT 8

/*
 * Loads the library from path (a soname such as UC_WEBVIEW_SONAME is searched like dlopen() does) and resolves its
 * symbols. Returns true; false with a reason in reason[reason_size] when the library is missing, lacks a required
 * symbol or has another API version than UC_WEBVIEW_API_VERSION (the library is closed again then).
 * The library is opened with RTLD_NOW | RTLD_LOCAL and never closed after a successful load: WebKit and GLib keep
 * process-wide state that does not survive an unload.
 */
bool ucfl_lib_load(struct ucfl_lib *lib, const char *path, char *reason, size_t reason_size);

/* The names of the features in lib->features, in a fixed order; returns the count (<= UCFL_FEATURE_COUNT). */
size_t ucfl_feature_names(unsigned features, const char **names, size_t max);

#endif /* UCFL_LOADER_H */
