/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for the pure parts of uc_egl.c plus a smoke test of the EGL
 * queries against the host's libEGL (no display needed for the negative paths).
 */
#include "uc_egl.h"

static void
test_extension_in_string (void)
{
    const char *ext = "EGL_EXT_device_base EGL_EXT_device_drm EGL_KHR_image_base";
    g_assert_true (uc_egl_extension_in_string (ext, "EGL_EXT_device_base"));
    g_assert_true (uc_egl_extension_in_string (ext, "EGL_EXT_device_drm"));
    g_assert_true (uc_egl_extension_in_string (ext, "EGL_KHR_image_base"));
    /* prefixes and substrings are not matches */
    g_assert_false (uc_egl_extension_in_string (ext, "EGL_EXT_device_drm_render_node"));
    g_assert_false (uc_egl_extension_in_string (ext, "EGL_EXT_device"));
    g_assert_false (uc_egl_extension_in_string (ext, "device_drm"));
    g_assert_false (uc_egl_extension_in_string (NULL, "EGL_EXT_device_drm"));
    g_assert_false (uc_egl_extension_in_string (ext, NULL));
    g_assert_false (uc_egl_extension_in_string (ext, ""));
    g_assert_true (uc_egl_extension_in_string ("EGL_EXT_device_drm", "EGL_EXT_device_drm"));
}

static gboolean
fake_exists (const char *path, gpointer user_data)
{
    GHashTable *fs = user_data;
    return g_hash_table_contains (fs, path);
}

static void
test_choose_drm_nodes (void)
{
    GHashTable *fs = g_hash_table_new (g_str_hash, g_str_equal);
    char *primary, *render;

    /* EGL knows both (Remote 3: card0 + renderD128, panfrost) */
    g_assert_true (uc_egl_choose_drm_nodes ("/dev/dri/card0", "/dev/dri/renderD128", fake_exists, fs, &primary, &render));
    g_assert_cmpstr (primary, ==, "/dev/dri/card0");
    g_assert_cmpstr (render, ==, "/dev/dri/renderD128");
    g_free (primary); g_free (render);

    /* EGL knows the render node only */
    g_assert_true (uc_egl_choose_drm_nodes (NULL, "/dev/dri/renderD129", fake_exists, fs, &primary, &render));
    g_assert_null (primary);
    g_assert_cmpstr (render, ==, "/dev/dri/renderD129");
    g_free (render);

    /* EGL knows nothing (software device), fixed nodes exist */
    g_hash_table_add (fs, (gpointer) "/dev/dri/renderD128");
    g_hash_table_add (fs, (gpointer) "/dev/dri/card0");
    g_assert_true (uc_egl_choose_drm_nodes ("", NULL, fake_exists, fs, &primary, &render));
    g_assert_cmpstr (primary, ==, "/dev/dri/card0");
    g_assert_cmpstr (render, ==, "/dev/dri/renderD128");
    g_free (primary); g_free (render);

    /* render node only on the file system */
    g_hash_table_remove (fs, "/dev/dri/card0");
    g_assert_true (uc_egl_choose_drm_nodes (NULL, NULL, fake_exists, fs, &primary, &render));
    g_assert_null (primary);
    g_assert_cmpstr (render, ==, "/dev/dri/renderD128");
    g_free (render);

    /* nothing at all: software path */
    g_hash_table_remove_all (fs);
    g_assert_false (uc_egl_choose_drm_nodes (NULL, NULL, fake_exists, fs, &primary, &render));
    g_assert_null (primary);
    g_assert_null (render);
    g_assert_false (uc_egl_choose_drm_nodes (NULL, NULL, NULL, NULL, &primary, &render));
    g_hash_table_unref (fs);
}

static void
test_queries_without_display (void)
{
    char *primary = (char *) "x", *render = (char *) "y";
    guint n = 7;
    g_assert_cmpstr (uc_egl_display_extensions (NULL), ==, "");
    g_assert_false (uc_egl_query_drm_nodes (NULL, &primary, &render));
    g_assert_null (primary);
    g_assert_null (render);
    g_assert_null (uc_egl_query_dmabuf_formats (NULL, &n));
    g_assert_cmpuint (n, ==, 0);
    uc_egl_formats_free (NULL, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/egl/extension-in-string", test_extension_in_string);
    g_test_add_func ("/egl/choose-drm-nodes", test_choose_drm_nodes);
    g_test_add_func ("/egl/queries-without-display", test_queries_without_display);
    return g_test_run ();
}
