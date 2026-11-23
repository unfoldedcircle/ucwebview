/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * uc_webview_set_magnification(): arithmetic and scripts, see uc_magnify.h.
 */
#include "uc_magnify.h"

gboolean
uc_magnify_scale_valid (double scale)
{
    /* NaN fails both comparisons */
    return scale >= UC_MAGNIFY_MIN && scale <= UC_MAGNIFY_MAX;
}

void
uc_magnify_clamp_focal (double *x, double *y, int width, int height)
{
    double w = width > 0 ? width : 1, h = height > 0 ? height : 1;

    if (!(*x == *x))
        *x = w / 2;
    if (!(*y == *y))
        *y = h / 2;
    *x = CLAMP (*x, 0.0, w);
    *y = CLAMP (*y, 0.0, h);
}

void
uc_magnify_scroll_target (double scale0, double left0, double top0, double scale1, double zoom, double fx, double fy,
                          double *left1, double *top1)
{
    double k0 = zoom * scale0, k1 = zoom * scale1;

    *left1 = left0 + fx / k0 - fx / k1;
    *top1 = top0 + fy / k0 - fy / k1;
}

/* A, run in the library's script world before the scale message (uc_magnify.h). The record lives in the world's
 * own window wrapper and dies with the document. */
static const char script_before[] =
    "(function (id) {"
    " var w = window, v = w.visualViewport, all = w.__ucwebviewMagnify || (w.__ucwebviewMagnify = {});"
    " if (!v) return -1;"
    " all[id] = [v.scale, v.pageLeft, v.pageTop];"
    " return v.scale;"
    "}) (%u)";

/* B, after the scale message: uc_magnify_scroll_target() with the scales the page reports. 'instant' because a page
 * with scroll-behavior: smooth would animate a programmatic scroll (LocalDOMWindow.cpp:1860). */
static const char script_after[] =
    "(function (id, fx, fy, zoom) {"
    " var w = window, v = w.visualViewport, all = w.__ucwebviewMagnify || {}, s = all[id], k0, k1;"
    " delete all[id];"
    " if (!v || !s) return -1;"
    " k0 = zoom * s[0]; k1 = zoom * v.scale;"
    " w.scrollTo ({ left: Math.round (s[1] + fx / k0 - fx / k1), top: Math.round (s[2] + fy / k0 - fy / k1),"
    " behavior: 'instant' });"
    " return v.scale;"
    "}) (%u, %s, %s, %s)";

char *
uc_magnify_script_before (guint id)
{
    return g_strdup_printf (script_before, id);
}

char *
uc_magnify_script_after (guint id, double fx, double fy, double zoom)
{
    char x[G_ASCII_DTOSTR_BUF_SIZE], y[G_ASCII_DTOSTR_BUF_SIZE], z[G_ASCII_DTOSTR_BUF_SIZE];

    /* g_ascii_formatd: '.' whatever LC_NUMERIC says */
    g_ascii_formatd (x, sizeof x, "%.3f", fx);
    g_ascii_formatd (y, sizeof y, "%.3f", fy);
    g_ascii_formatd (z, sizeof z, "%.6f", zoom > 0 ? zoom : 1.0);
    return g_strdup_printf (script_after, id, x, y, z);
}
