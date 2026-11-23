/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * uc_webview_set_magnification() (library 0.6.0): the focal point arithmetic and the two scripts that keep the
 * content under the focal point in place. GLib only, so that the rules are unit-tested on a host without WPE
 * (tests/test-magnify.c); uc_webview.c sends the scripts and WebKit's scale change.
 *
 * Why scripts: WebKit 2.54's webkit_web_view_set_magnification() keeps no point of the view fixed. It sends
 * DidScalePageInViewCoordinates with the view centre c (WebKitWebView.cpp:4250-4253), and the web process scrolls to
 * r * (s - c) with the scroll position s and the ratio r of the new to the old scale (WebPage.cpp:2960-2970,
 * rootViewToContents(-c) = s - c, ScrollView.cpp:957-962, 1069-1072); keeping the centre would be r * (s + c) - c.
 * Checked on WebKitGTK 2.54.0, which shares that code: magnification 2 at scroll (500, 1000) in a 480x800 view
 * lands at (260, 600), not at (620, 1200). The public API has no other scale call (WebPageProxy::scalePage() with an
 * origin is internal), so the library sets the scroll position itself, in its own script world:
 *   A  before the scale message: records visualViewport.scale, .pageLeft, .pageTop (CSS pixels, sub-pixel exact,
 *      VisualViewport.cpp:143-185) under the request id, in the world's own window object (page scripts do not see
 *      it, every world has its own JS wrappers);
 *   M  webkit_web_view_set_magnification(scale);
 *   B  after it: reads the new scale and scrolls with window.scrollTo({behavior: 'instant'}) so that the content
 *      point under the focal point is where it was (uc_magnify_scroll_target()).
 * The three go to the same WebPage over one connection and run in this order (WebPageProxy.cpp:6792-6808, 7533-7560;
 * WebPage::runJavaScript runs the script inside its message, WebPage.cpp:4905-4990). The page sees one programmatic
 * scroll. scrollTo() takes whole CSS pixels (LocalDOMWindow.cpp:1833-1863, mapFromCSSToLayoutUnits(int)), so the
 * point stays within half a CSS pixel, magnification * page zoom / 2 view pixels.
 */
#ifndef UC_MAGNIFY_H
#define UC_MAGNIFY_H

#include <glib.h>

G_BEGIN_DECLS

/* the range of uc_webview_set_magnification() (UC_WEBVIEW_MAGNIFICATION_MIN/MAX, asserted in uc_webview.c) */
#define UC_MAGNIFY_MIN 0.25
#define UC_MAGNIFY_MAX 10.0

/* Two magnifications closer than this are the same (no change is sent). */
#define UC_MAGNIFY_EPSILON 1e-4

/* A number in UC_MAGNIFY_MIN..UC_MAGNIFY_MAX (NaN and infinities are not). */
gboolean uc_magnify_scale_valid  (double scale);
/* The focal point clamped into a view of width x height pixels; NaN becomes the view centre. */
void     uc_magnify_clamp_focal  (double *x, double *y, int width, int height);
/* The scroll position in CSS pixels after a change from scale0 at (left0, top0) to scale1, with page zoom zoom, that
 * keeps the content at the focal point (fx, fy) (view pixels) in place: a view pixel is 1 / (zoom * scale) CSS
 * pixels, so left1 = left0 + fx / (zoom * scale0) - fx / (zoom * scale1), the same for top. The page clamps the result
 * to its scroll range. The after script computes the same with the values the page reports. */
void     uc_magnify_scroll_target (double scale0, double left0, double top0, double scale1, double zoom, double fx,
                                   double fy, double *left1, double *top1);
/* Script A and B for request id, numbers in the C locale (the host may have set a locale with a decimal comma,
 * QCoreApplication calls setlocale(LC_ALL, "")). Free with g_free(). A returns the scale before, B the scale after,
 * -1 when the page has no visualViewport or A's record is gone (another document). */
char    *uc_magnify_script_before (guint id);
char    *uc_magnify_script_after  (guint id, double fx, double fy, double zoom);

G_END_DECLS

#endif /* UC_MAGNIFY_H */
