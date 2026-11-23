/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_formats.c (GLib only): run with `make check`.
 */
#include "uc_formats.h"

#define AFBC_16x16 0x0800000000000001ull /* DRM_FORMAT_MOD_ARM_AFBC(BLOCK_SIZE_16x16) */

static const UcFormatEntry *
entry (GArray *formats, guint i)
{
    return &g_array_index (formats, UcFormatEntry, i);
}

static void
test_fallback_linear (void)
{
    GArray *out = uc_formats_select (NULL, 0);
    g_assert_cmpuint (out->len, ==, 2);
    g_assert_cmpuint (entry (out, 0)->fourcc, ==, UC_FOURCC_ARGB8888);
    g_assert_cmpuint (entry (out, 0)->n_modifiers, ==, 1);
    g_assert_cmpuint (entry (out, 0)->modifiers[0], ==, UC_DRM_MOD_LINEAR);
    g_assert_cmpuint (entry (out, 1)->fourcc, ==, UC_FOURCC_XRGB8888);
    g_assert_cmpuint (entry (out, 1)->modifiers[0], ==, UC_DRM_MOD_LINEAR);
    uc_formats_free (out);

    /* EGL reports only formats we do not want (e.g. NV12 'NV12') */
    guint64 mods[] = { UC_DRM_MOD_LINEAR };
    UcFormatEntry in[] = { { 0x3231564eu, mods, 1 } };
    out = uc_formats_select (in, 1);
    g_assert_cmpuint (out->len, ==, 2);
    g_assert_cmpuint (entry (out, 0)->modifiers[0], ==, UC_DRM_MOD_LINEAR);
    uc_formats_free (out);
}

static void
test_egl_modifiers (void)
{
    /* Panfrost-like answer: XRGB first, AFBC + LINEAR + INVALID; ARGB with INVALID only */
    guint64 xr_mods[] = { AFBC_16x16, UC_DRM_MOD_INVALID, UC_DRM_MOD_LINEAR };
    guint64 ar_mods[] = { UC_DRM_MOD_INVALID };
    UcFormatEntry in[] = {
        { UC_FOURCC_XRGB8888, xr_mods, 3 },
        { 0x3231564eu, NULL, 0 },
        { UC_FOURCC_ARGB8888, ar_mods, 1 },
    };
    GArray *out = uc_formats_select (in, 3);

    g_assert_cmpuint (out->len, ==, 2);
    /* ARGB is advertised first, INVALID kept alone -> implicit modifiers */
    g_assert_cmpuint (entry (out, 0)->fourcc, ==, UC_FOURCC_ARGB8888);
    g_assert_cmpuint (entry (out, 0)->n_modifiers, ==, 1);
    g_assert_cmpuint (entry (out, 0)->modifiers[0], ==, UC_DRM_MOD_INVALID);
    /* XRGB: INVALID dropped because explicit modifiers exist, order kept */
    g_assert_cmpuint (entry (out, 1)->fourcc, ==, UC_FOURCC_XRGB8888);
    g_assert_cmpuint (entry (out, 1)->n_modifiers, ==, 2);
    g_assert_cmpuint (entry (out, 1)->modifiers[0], ==, AFBC_16x16);
    g_assert_cmpuint (entry (out, 1)->modifiers[1], ==, UC_DRM_MOD_LINEAR);
    uc_formats_free (out);
}

static void
test_format_without_modifiers (void)
{
    /* no EGL_EXT_image_dma_buf_import_modifiers: formats come without modifiers -> LINEAR */
    UcFormatEntry in[] = { { UC_FOURCC_ARGB8888, NULL, 0 } };
    GArray *out = uc_formats_select (in, 1);
    g_assert_cmpuint (out->len, ==, 1);
    g_assert_cmpuint (entry (out, 0)->fourcc, ==, UC_FOURCC_ARGB8888);
    g_assert_cmpuint (entry (out, 0)->n_modifiers, ==, 1);
    g_assert_cmpuint (entry (out, 0)->modifiers[0], ==, UC_DRM_MOD_LINEAR);
    uc_formats_free (out);
}

static void
test_fourcc_string (void)
{
    char buf[5];
    g_assert_cmpstr (uc_fourcc_to_string (UC_FOURCC_ARGB8888, buf), ==, "AR24");
    g_assert_cmpstr (uc_fourcc_to_string (UC_FOURCC_XRGB8888, buf), ==, "XR24");
    g_assert_cmpstr (uc_fourcc_to_string (0x00000001u, buf), ==, "????");
    g_assert_cmpuint (UC_FOURCC_ARGB8888, ==, ((guint32) 'A' | ((guint32) 'R' << 8) | ((guint32) '2' << 16) | ((guint32) '4' << 24)));
}

/* library 0.10.3: the stride of wpe_buffer_import_to_pixels() data from its size (uc_formats.h) */
static void
test_pixels_stride (void)
{
    /* WPE WebKit 2.54.3: rows without padding, also for a width whose buffer rows are padded (481 px: 1924 bytes,
     * a 64-byte aligned plane has 1984) */
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 480 * 4 * 800, 480, 800), ==, 1920);
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 481 * 4 * 800, 481, 800), ==, 1924);
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 1 * 4 * 1, 1, 1), ==, 4);
    /* 2.54.2: the gbm_bo_map() mapping, height rows of the map's stride */
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 1984 * 800, 481, 800), ==, 1984);
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 2048 * 800, 480, 800), ==, 2048);
    /* a linear copy of a tiled buffer may have another stride than the plane; the size is what counts */
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 1936 * 3, 481, 3), ==, 1936);
    /* not height whole rows */
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 1920 * 800 + 1, 480, 800), ==, -1);
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 1920 * 799, 480, 800), ==, -1);
    /* rows shorter than width * 4 */
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) 1916 * 800, 480, 800), ==, -1);
    g_assert_cmpint (uc_formats_pixels_stride (0, 480, 800), ==, -1);
    /* no size */
    g_assert_cmpint (uc_formats_pixels_stride (1920, 0, 1), ==, -1);
    g_assert_cmpint (uc_formats_pixels_stride (1920, 480, 0), ==, -1);
    g_assert_cmpint (uc_formats_pixels_stride (1920, -480, 1), ==, -1);
    g_assert_cmpint (uc_formats_pixels_stride (1920, 480, -1), ==, -1);
    /* a stride that does not fit the frame's int */
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) G_MAXINT + 1, 1, 1), ==, -1);
    g_assert_cmpint (uc_formats_pixels_stride ((gsize) G_MAXINT, 1, 1), ==, G_MAXINT);
    g_assert_cmpint (uc_formats_pixels_stride (G_MAXSIZE, G_MAXINT, 1), ==, -1);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/formats/fallback-linear", test_fallback_linear);
    g_test_add_func ("/formats/egl-modifiers", test_egl_modifiers);
    g_test_add_func ("/formats/without-modifiers", test_format_without_modifiers);
    g_test_add_func ("/formats/fourcc-string", test_fourcc_string);
    g_test_add_func ("/formats/pixels-stride", test_pixels_stride);
    return g_test_run ();
}
