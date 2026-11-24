/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * dlopen() loader of libucwebview, see ucfl_loader.h.
 */
#include "ucfl_loader.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* POSIX dlsym() returns function addresses as void *; they are stored through memcpy() into the typed members */
_Static_assert(sizeof(void *) == sizeof(void (*)(void)), "function and object pointers differ in size");

#define REQUIRED(field, name) { "uc_webview_" #name, offsetof(struct ucfl_lib, field), true }
#define OPTIONAL(field, name) { "uc_webview_" #name, offsetof(struct ucfl_lib, field), false }

static const struct symbol {
    const char *name;
    size_t offset;
    bool required;
} symbols[] = {
    REQUIRED(api_version, api_version),
    REQUIRED(global_init, global_init),
    REQUIRED(global_shutdown, global_shutdown),
    REQUIRED(create, new),
    REQUIRED(destroy, free),
    REQUIRED(resize, resize),
    REQUIRED(set_mapped, set_mapped),
    REQUIRED(set_focused, set_focused),
    REQUIRED(set_scale, set_scale),
    REQUIRED(load_uri, load_uri),
    REQUIRED(load_html, load_html),
    REQUIRED(reload, reload),
    REQUIRED(stop, stop),
    REQUIRED(go_back, go_back),
    REQUIRED(go_forward, go_forward),
    REQUIRED(can_go_back, can_go_back),
    REQUIRED(can_go_forward, can_go_forward),
    REQUIRED(get_uri, get_uri),
    REQUIRED(get_title, get_title),
    REQUIRED(run_javascript, run_javascript),
    REQUIRED(set_zoom, set_zoom),
    REQUIRED(set_background_rgba, set_background_rgba),
    REQUIRED(frame_rendered, frame_rendered),
    REQUIRED(frame_release, frame_release),
    REQUIRED(touch, touch),
    REQUIRED(pointer_button, pointer_button),
    REQUIRED(pointer_move, pointer_move),
    REQUIRED(scroll, scroll),
    REQUIRED(key, key),
    REQUIRED(ime_commit, ime_commit),
    REQUIRED(ime_preedit, ime_preedit),
    REQUIRED(ime_delete_surrounding, ime_delete_surrounding),
    OPTIONAL(global_set_refresh_rate, global_set_refresh_rate),
    OPTIONAL(global_get_refresh_rate, global_get_refresh_rate),
    OPTIONAL(global_get_drag_threshold, global_get_drag_threshold),
    OPTIONAL(create_sized, new_sized),
    OPTIONAL(when_rendered, when_rendered),
    OPTIONAL(set_magnification, set_magnification),
    OPTIONAL(get_magnification, get_magnification),
    OPTIONAL(request_reply, request_reply),
    OPTIONAL(request_auth, request_auth),
    OPTIONAL(set_origin_headers, set_origin_headers),
    OPTIONAL(set_user_agent, set_user_agent),
    OPTIONAL(global_set_cookies, global_set_cookies),
    OPTIONAL(global_allow_certificate, global_allow_certificate),
    OPTIONAL(global_set_tls_errors_policy, global_set_tls_errors_policy),
    OPTIONAL(global_set_color_scheme, global_set_color_scheme),
    OPTIONAL(global_clear_data, global_clear_data),
    OPTIONAL(set_device_api, set_device_api),
    OPTIONAL(set_device_state, set_device_state),
    OPTIONAL(device_event, device_event),
    OPTIONAL(library_version, library_version),
};

/* the names of the feature bits, in bit order */
static const char *const feature_names[UCFL_FEATURE_COUNT] = {
    "refreshRate",      "dragThreshold",   "newSized",    "whenRendered", "pinchZoom",
    "requests",         "originHeaders",   "userAgent",   "cookies",      "allowCertificate",
    "tlsErrorsPolicy",  "colorScheme",     "clearData",   "deviceApi",
};

unsigned ucfl_lib_features(const struct ucfl_lib *lib)
{
    unsigned f = 0;

    if (lib->global_set_refresh_rate)
        f |= UCFL_FEATURE_REFRESH_RATE;
    if (lib->global_get_drag_threshold)
        f |= UCFL_FEATURE_DRAG_THRESHOLD;
    if (lib->create_sized)
        f |= UCFL_FEATURE_NEW_SIZED;
    if (lib->when_rendered)
        f |= UCFL_FEATURE_WHEN_RENDERED;
    /* remote-ui WebViewLibrary::pinchZoomAvailable(): the pinch callback reaches the library only through new_sized,
     * and the scaled frame is held until when_rendered reports */
    if (lib->create_sized && lib->set_magnification && lib->get_magnification && lib->when_rendered)
        f |= UCFL_FEATURE_PINCH_ZOOM;
    /* WebViewLibrary::requestsAvailable(): the request callbacks reach the library only through new_sized */
    if (lib->create_sized && lib->request_reply && lib->request_auth)
        f |= UCFL_FEATURE_REQUESTS;
    if (lib->set_origin_headers)
        f |= UCFL_FEATURE_ORIGIN_HEADERS;
    if (lib->set_user_agent)
        f |= UCFL_FEATURE_USER_AGENT;
    if (lib->global_set_cookies)
        f |= UCFL_FEATURE_COOKIES;
    if (lib->global_allow_certificate)
        f |= UCFL_FEATURE_ALLOW_CERTIFICATE;
    if (lib->global_set_tls_errors_policy)
        f |= UCFL_FEATURE_TLS_ERRORS_POLICY;
    if (lib->global_set_color_scheme)
        f |= UCFL_FEATURE_COLOR_SCHEME;
    if (lib->global_clear_data)
        f |= UCFL_FEATURE_CLEAR_DATA;
    /* WebViewLibrary::deviceApiAvailable(): all three */
    if (lib->set_device_api && lib->set_device_state && lib->device_event)
        f |= UCFL_FEATURE_DEVICE_API;
    return f;
}

bool ucfl_lib_load(struct ucfl_lib *lib, const char *path, char *reason, size_t reason_size)
{
    const char *error;
    bool missing = false;
    size_t used;

    memset(lib, 0, sizeof(*lib));
    lib->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib->handle) {
        error = dlerror();
        snprintf(reason, reason_size, "%s", error ? error : "dlopen failed");
        return false;
    }

    snprintf(reason, reason_size, "%s lacks", path);
    used = strlen(reason);
    for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
        void *address = dlsym(lib->handle, symbols[i].name);
        memcpy((char *) lib + symbols[i].offset, &address, sizeof(address));
        if (!address && symbols[i].required) {
            int n = snprintf(reason + used, reason_size - used, " %s", symbols[i].name);
            if (n > 0 && (size_t) n < reason_size - used)
                used += (size_t) n;
            missing = true;
        }
    }
    if (missing) {
        dlclose(lib->handle);
        memset(lib, 0, sizeof(*lib));
        return false;
    }

    int version = lib->api_version();
    if (version != UC_WEBVIEW_API_VERSION) {
        snprintf(reason, reason_size, "%s has API version %d, this build needs %d", path, version, UC_WEBVIEW_API_VERSION);
        dlclose(lib->handle);
        memset(lib, 0, sizeof(*lib));
        return false;
    }

    lib->features = ucfl_lib_features(lib);
    if (reason_size)
        reason[0] = '\0';
    return true;
}

const char *ucfl_feature_name(unsigned bit)
{
    for (unsigned i = 0; i < UCFL_FEATURE_COUNT; i++) {
        if (bit == 1u << i)
            return feature_names[i];
    }
    return NULL;
}

size_t ucfl_feature_names(unsigned bits, const char **names, size_t max)
{
    size_t n = 0;
    for (unsigned i = 0; i < UCFL_FEATURE_COUNT && n < max; i++) {
        if (bits & (1u << i))
            names[n++] = feature_names[i];
    }
    return n;
}
