/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * WebKit cache model of libucwebview, see uc_cache_model.h.
 */
#include "uc_cache_model.h"

#include <string.h>

static const char *const names[] = {
    [UC_CACHE_MODEL_VIEWER] = "viewer",
    [UC_CACHE_MODEL_BROWSER] = "browser",
    [UC_CACHE_MODEL_WEB_BROWSER] = "web-browser",
};

UcCacheModel
uc_cache_model_parse (const char *value, UcCacheModelSource *source)
{
    const char *start, *end;
    gsize len;

    if (source != NULL)
        *source = UC_CACHE_MODEL_SOURCE_DEFAULT;
    if (value == NULL)
        return UC_CACHE_MODEL_DEFAULT;
    start = value;
    while (g_ascii_isspace (*start))
        start++;
    end = start + strlen (start);
    while (end > start && g_ascii_isspace (end[-1]))
        end--;
    len = (gsize) (end - start);
    if (len == 0)
        return UC_CACHE_MODEL_DEFAULT;
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        if (strlen (names[i]) == len && g_ascii_strncasecmp (start, names[i], len) == 0) {
            if (source != NULL)
                *source = UC_CACHE_MODEL_SOURCE_ENV;
            return (UcCacheModel) i;
        }
    }
    if (source != NULL)
        *source = UC_CACHE_MODEL_SOURCE_INVALID;
    return UC_CACHE_MODEL_DEFAULT;
}

const char *
uc_cache_model_name (UcCacheModel model)
{
    if ((guint) model < G_N_ELEMENTS (names))
        return names[model];
    return names[UC_CACHE_MODEL_DEFAULT];
}
