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

#define MEMBER(field) offsetof(struct ucfl_lib, field)

static const struct symbol {
    const char *name;
    size_t offset;
    bool required;
} symbols[] = {
    { "uc_webview_api_version", MEMBER(api_version), true },
    { "uc_webview_global_init", MEMBER(global_init), true },
    { "uc_webview_new", MEMBER(create), true },
    { "uc_webview_free", MEMBER(destroy), true },
    { "uc_webview_resize", MEMBER(resize), true },
    { "uc_webview_set_mapped", MEMBER(set_mapped), true },
    { "uc_webview_set_focused", MEMBER(set_focused), true },
    { "uc_webview_set_scale", MEMBER(set_scale), true },
    { "uc_webview_load_uri", MEMBER(load_uri), true },
    { "uc_webview_frame_rendered", MEMBER(frame_rendered), true },
    { "uc_webview_frame_release", MEMBER(frame_release), true },
    { "uc_webview_touch", MEMBER(touch), true },
    { "uc_webview_key", MEMBER(key), true },
    { "uc_webview_global_set_refresh_rate", MEMBER(global_set_refresh_rate), false },
    { "uc_webview_new_sized", MEMBER(create_sized), false },
    { "uc_webview_library_version", MEMBER(library_version), false },
};

/* the function that marks each feature bit, in bit order */
static const struct feature {
    const char *symbol;
    const char *name;
} features[UCFL_FEATURE_COUNT] = {
    { "uc_webview_global_set_refresh_rate", "refreshRate" },
    { "uc_webview_global_get_drag_threshold", "dragThreshold" },
    { "uc_webview_new_sized", "newSized" },
    { "uc_webview_when_rendered", "whenRendered" },
    { "uc_webview_set_magnification", "pinchZoom" },
    { "uc_webview_request_reply", "requests" },
    { "uc_webview_set_origin_headers", "siteSettings" },
    { "uc_webview_set_device_api", "deviceApi" },
};

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

    for (unsigned i = 0; i < UCFL_FEATURE_COUNT; i++) {
        if (dlsym(lib->handle, features[i].symbol))
            lib->features |= 1u << i;
    }
    if (reason_size)
        reason[0] = '\0';
    return true;
}

size_t ucfl_feature_names(unsigned bits, const char **names, size_t max)
{
    size_t n = 0;
    for (unsigned i = 0; i < UCFL_FEATURE_COUNT && n < max; i++) {
        if (bits & (1u << i))
            names[n++] = features[i].name;
    }
    return n;
}
