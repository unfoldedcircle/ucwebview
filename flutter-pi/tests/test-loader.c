/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test of core/ucfl_loader.c (GLib's test framework only) against the fakes of tests/fake-ucwebview.c,
 * whose paths the Makefile passes: a missing library, one of another API version, one without a required function
 * and one without the optional functions give "unavailable" or fewer features, never a crash.
 */
#include "ucfl_loader.h"

#include <string.h>

#include <glib.h>

static void test_missing(void)
{
    struct ucfl_lib lib;
    char reason[256];

    g_assert_false(ucfl_lib_load(&lib, FAKE_DIR "/no-such-libucwebview.so.1", reason, sizeof(reason)));
    g_assert_nonnull(strstr(reason, "no-such-libucwebview.so.1"));
    g_assert_null(lib.handle);
}

static void test_complete(void)
{
    struct ucfl_lib lib;
    char reason[256];
    const char *names[UCFL_FEATURE_COUNT];
    size_t n;

    g_assert_true(ucfl_lib_load(&lib, FAKE_DIR "/fake-ok.so", reason, sizeof(reason)));
    g_assert_cmpstr(reason, ==, "");
    g_assert_cmpint(lib.api_version(), ==, UC_WEBVIEW_API_VERSION);
    g_assert_nonnull(lib.create_sized);
    g_assert_nonnull(lib.global_set_refresh_rate);
    g_assert_cmpint(lib.global_set_refresh_rate(48.61), ==, 0);
    g_assert_cmpstr(lib.library_version(), ==, "9.9.9-fake");
    g_assert_nonnull(lib.create(1, 1, NULL, NULL));
    g_assert_cmpuint(lib.features, ==, UCFL_FEATURE_REFRESH_RATE | UCFL_FEATURE_NEW_SIZED | UCFL_FEATURE_DEVICE_API);
    n = ucfl_feature_names(lib.features, names, G_N_ELEMENTS(names));
    g_assert_cmpuint(n, ==, 3);
    g_assert_cmpstr(names[0], ==, "refreshRate");
    g_assert_cmpstr(names[1], ==, "newSized");
    g_assert_cmpstr(names[2], ==, "deviceApi");
    g_assert_cmpuint(ucfl_feature_names(lib.features, names, 1), ==, 1);
}

static void test_old(void)
{
    struct ucfl_lib lib;
    char reason[256];

    g_assert_true(ucfl_lib_load(&lib, FAKE_DIR "/fake-old.so", reason, sizeof(reason)));
    g_assert_null(lib.create_sized);
    g_assert_null(lib.global_set_refresh_rate);
    g_assert_null(lib.library_version);
    g_assert_cmpuint(lib.features, ==, 0);
}

static void test_other_api_version(void)
{
    struct ucfl_lib lib;
    char reason[256];

    g_assert_false(ucfl_lib_load(&lib, FAKE_DIR "/fake-abi2.so", reason, sizeof(reason)));
    g_assert_nonnull(strstr(reason, "API version 2"));
    g_assert_null(lib.handle);
}

static void test_incomplete(void)
{
    struct ucfl_lib lib;
    char reason[256];

    g_assert_false(ucfl_lib_load(&lib, FAKE_DIR "/fake-incomplete.so", reason, sizeof(reason)));
    g_assert_nonnull(strstr(reason, "lacks uc_webview_key"));
    g_assert_null(lib.handle);
    g_assert_null(lib.touch);

    /* a reason buffer too small for the list stays terminated */
    g_assert_false(ucfl_lib_load(&lib, FAKE_DIR "/fake-incomplete.so", reason, 8));
    g_assert_cmpuint(strlen(reason), ==, 7);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/loader/missing", test_missing);
    g_test_add_func("/loader/complete", test_complete);
    g_test_add_func("/loader/old", test_old);
    g_test_add_func("/loader/other-api-version", test_other_api_version);
    g_test_add_func("/loader/incomplete", test_incomplete);
    return g_test_run();
}
