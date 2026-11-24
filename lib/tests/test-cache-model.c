/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_cache_model.c (GLib only): run with `make check`.
 */
#include "uc_cache_model.h"

static void
check (const char *value, UcCacheModel expected, UcCacheModelSource expected_source)
{
    UcCacheModelSource source = (UcCacheModelSource) 42;

    g_assert_cmpint (uc_cache_model_parse (value, &source), ==, expected);
    g_assert_cmpint (source, ==, expected_source);
    /* source may be NULL */
    g_assert_cmpint (uc_cache_model_parse (value, NULL), ==, expected);
}

static void
test_default (void)
{
    g_assert_cmpint (UC_CACHE_MODEL_DEFAULT, ==, UC_CACHE_MODEL_BROWSER);
    check (NULL, UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_DEFAULT);
    check ("", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_DEFAULT);
    check ("  \t\n", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_DEFAULT);
}

static void
test_values (void)
{
    check ("viewer", UC_CACHE_MODEL_VIEWER, UC_CACHE_MODEL_SOURCE_ENV);
    check ("browser", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_ENV);
    check ("web-browser", UC_CACHE_MODEL_WEB_BROWSER, UC_CACHE_MODEL_SOURCE_ENV);
    check ("VIEWER", UC_CACHE_MODEL_VIEWER, UC_CACHE_MODEL_SOURCE_ENV);
    check ("Web-Browser", UC_CACHE_MODEL_WEB_BROWSER, UC_CACHE_MODEL_SOURCE_ENV);
    check (" browser\n", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_ENV);
    check ("\tweb-browser ", UC_CACHE_MODEL_WEB_BROWSER, UC_CACHE_MODEL_SOURCE_ENV);
}

static void
test_invalid (void)
{
    check ("web_browser", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
    check ("webbrowser", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
    check ("document-viewer", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
    check ("view", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
    check ("viewers", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
    check ("browser x", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
    check ("1", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
    check ("0", UC_CACHE_MODEL_BROWSER, UC_CACHE_MODEL_SOURCE_INVALID);
}

static void
test_names (void)
{
    g_assert_cmpstr (uc_cache_model_name (UC_CACHE_MODEL_VIEWER), ==, "viewer");
    g_assert_cmpstr (uc_cache_model_name (UC_CACHE_MODEL_BROWSER), ==, "browser");
    g_assert_cmpstr (uc_cache_model_name (UC_CACHE_MODEL_WEB_BROWSER), ==, "web-browser");
    g_assert_cmpstr (uc_cache_model_name ((UcCacheModel) 7), ==, "browser");
    /* the names parse back to their model */
    for (int m = UC_CACHE_MODEL_VIEWER; m <= UC_CACHE_MODEL_WEB_BROWSER; m++)
        check (uc_cache_model_name ((UcCacheModel) m), (UcCacheModel) m, UC_CACHE_MODEL_SOURCE_ENV);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/cache-model/default", test_default);
    g_test_add_func ("/cache-model/values", test_values);
    g_test_add_func ("/cache-model/invalid", test_invalid);
    g_test_add_func ("/cache-model/names", test_names);
    return g_test_run ();
}
