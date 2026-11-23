/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_magnify.c (GLib only): the scale range, the focal point clamp, the focal point arithmetic
 * and the two scripts. The scripts also run in JavaScriptCore when the host has its GLib API library
 * (libjavascriptcoregtk-4.1.so.0 or -6.0.so.1, loaded with GModule, no build dependency): with a fake visualViewport
 * and scrollTo() the after script must scroll to what uc_magnify_scroll_target() computes. Without the library those
 * cases are skipped. Run with `make check`.
 */
#include "uc_magnify.h"
#include "uc_webview.h"

#include <glib-object.h>
#include <gmodule.h>
#include <locale.h>
#include <string.h>

static void
test_range (void)
{
    double zero = 0.0, nan_value = zero / zero, inf = 1.0 / zero;

    g_assert_cmpfloat (UC_MAGNIFY_MIN, ==, UC_WEBVIEW_MAGNIFICATION_MIN);
    g_assert_cmpfloat (UC_MAGNIFY_MAX, ==, UC_WEBVIEW_MAGNIFICATION_MAX);
    g_assert_true (uc_magnify_scale_valid (1.0));
    g_assert_true (uc_magnify_scale_valid (0.25));
    g_assert_true (uc_magnify_scale_valid (10.0));
    g_assert_true (uc_magnify_scale_valid (3.7));
    g_assert_false (uc_magnify_scale_valid (0.249));
    g_assert_false (uc_magnify_scale_valid (10.01));
    g_assert_false (uc_magnify_scale_valid (0.0));
    g_assert_false (uc_magnify_scale_valid (-2.0));
    g_assert_false (uc_magnify_scale_valid (nan_value));
    g_assert_false (uc_magnify_scale_valid (inf));
}

static void
test_clamp_focal (void)
{
    double zero = 0.0, x, y;

    x = 100; y = 700;
    uc_magnify_clamp_focal (&x, &y, 480, 800);
    g_assert_cmpfloat (x, ==, 100);
    g_assert_cmpfloat (y, ==, 700);
    x = -5; y = 900;
    uc_magnify_clamp_focal (&x, &y, 480, 800);
    g_assert_cmpfloat (x, ==, 0);
    g_assert_cmpfloat (y, ==, 800);
    x = zero / zero; y = zero / zero;
    uc_magnify_clamp_focal (&x, &y, 480, 800);
    g_assert_cmpfloat (x, ==, 240);
    g_assert_cmpfloat (y, ==, 400);
}

/* the content point under the focal point, in CSS pixels */
static double
content (double scroll, double zoom, double scale, double focal)
{
    return scroll + focal / (zoom * scale);
}

static void
test_scroll_target (void)
{
    double left, top;

    /* the GTK 2.54.0 probe's cases (uc_magnify.h): 1 -> 2 at (100, 700) from (500, 1000) */
    uc_magnify_scroll_target (1, 500, 1000, 2, 1, 100, 700, &left, &top);
    g_assert_cmpfloat_with_epsilon (left, 550, 1e-9);
    g_assert_cmpfloat_with_epsilon (top, 1350, 1e-9);
    /* page zoom 1.5: 1 -> 2.5 at (300, 200) from (400, 900) */
    uc_magnify_scroll_target (1, 400, 900, 2.5, 1.5, 300, 200, &left, &top);
    g_assert_cmpfloat_with_epsilon (left, 520, 1e-9);
    g_assert_cmpfloat_with_epsilon (top, 980, 1e-9);
    /* at the top-left corner a focal point at the corner keeps the scroll position */
    uc_magnify_scroll_target (1, 0, 0, 3, 1, 0, 0, &left, &top);
    g_assert_cmpfloat (left, ==, 0);
    g_assert_cmpfloat (top, ==, 0);
    /* no change, no scroll */
    uc_magnify_scroll_target (2, 123.5, 456.25, 2, 1.25, 77, 99, &left, &top);
    g_assert_cmpfloat_with_epsilon (left, 123.5, 1e-9);
    g_assert_cmpfloat_with_epsilon (top, 456.25, 1e-9);
    /* zooming out at the same point returns to the start */
    {
        double l2, t2;
        uc_magnify_scroll_target (1, 500, 1000, 4, 1, 333, 111, &left, &top);
        uc_magnify_scroll_target (4, left, top, 1, 1, 333, 111, &l2, &t2);
        g_assert_cmpfloat_with_epsilon (l2, 500, 1e-9);
        g_assert_cmpfloat_with_epsilon (t2, 1000, 1e-9);
    }
    /* the invariant over a grid: the content under the focal point is the same before and after */
    for (double s0 = 0.5; s0 <= 4; s0 += 0.5)
        for (double s1 = 1; s1 <= 5; s1 += 0.75)
            for (double zoom = 0.5; zoom <= 3; zoom += 1.25)
                for (double f = 0; f <= 800; f += 160) {
                    uc_magnify_scroll_target (s0, 37, 2000, s1, zoom, f, 800 - f, &left, &top);
                    g_assert_cmpfloat_with_epsilon (content (left, zoom, s1, f), content (37, zoom, s0, f), 1e-9);
                    g_assert_cmpfloat_with_epsilon (content (top, zoom, s1, 800 - f), content (2000, zoom, s0, 800 - f),
                                                    1e-9);
                }
}

static void
test_scripts (void)
{
    char *a = uc_magnify_script_before (7);
    char *b = uc_magnify_script_after (7, 240.5, 400, 1.25);

    g_assert_nonnull (strstr (a, "}) (7)"));
    g_assert_nonnull (strstr (a, "visualViewport"));
    g_assert_nonnull (strstr (b, "}) (7, 240.500, 400.000, 1.250000)"));
    g_assert_nonnull (strstr (b, "behavior: 'instant'"));
    g_free (a);
    g_free (b);
    /* an invalid zoom is not passed on (WebKit's zoom is > 0) */
    b = uc_magnify_script_after (1, 0, 0, 0);
    g_assert_nonnull (strstr (b, "(1, 0.000, 0.000, 1.000000)"));
    g_free (b);
}

/* Qt hosts run with setlocale(LC_ALL, "") (QCoreApplication), a German locale prints 1,5: the scripts must not */
static void
test_scripts_locale (void)
{
    static const char *const comma_locales[] = { "de_DE.UTF-8", "de_CH.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", NULL };
    const char *found = NULL;
    char *saved = g_strdup (setlocale (LC_NUMERIC, NULL));
    char *b;

    for (guint i = 0; comma_locales[i] != NULL && found == NULL; i++)
        if (setlocale (LC_NUMERIC, comma_locales[i]) != NULL)
            found = comma_locales[i];
    b = uc_magnify_script_after (3, 12.5, 7.25, 1.5);
    setlocale (LC_NUMERIC, saved);
    g_free (saved);
    g_assert_nonnull (strstr (b, "(3, 12.500, 7.250, 1.500000)"));
    g_free (b);
    if (found == NULL)
        g_test_skip ("no locale with a decimal comma on this host; checked in the C locale only");
}

/* ---- the scripts in JavaScriptCore (optional) -------------------------------------------------------------- */

typedef struct _JSCContext JSCContext;
typedef struct _JSCValue JSCValue;
typedef struct _JSCException JSCException;

static struct {
    GModule *module;
    JSCContext *(*context_new) (void);
    JSCValue *(*evaluate) (JSCContext *, const char *, gssize);
    JSCException *(*get_exception) (JSCContext *);
    const char *(*exception_message) (JSCException *);
    double (*to_double) (JSCValue *);
    gboolean (*is_number) (JSCValue *);
} jsc;

static gboolean
jsc_load (void)
{
    static const char *const names[] = { "libjavascriptcoregtk-4.1.so.0", "libjavascriptcoregtk-6.0.so.1", NULL };

    if (jsc.module != NULL)
        return TRUE;
    for (guint i = 0; names[i] != NULL && jsc.module == NULL; i++)
        jsc.module = g_module_open (names[i], G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (jsc.module == NULL)
        return FALSE;
    if (!g_module_symbol (jsc.module, "jsc_context_new", (gpointer *) &jsc.context_new)
        || !g_module_symbol (jsc.module, "jsc_context_evaluate", (gpointer *) &jsc.evaluate)
        || !g_module_symbol (jsc.module, "jsc_context_get_exception", (gpointer *) &jsc.get_exception)
        || !g_module_symbol (jsc.module, "jsc_exception_get_message", (gpointer *) &jsc.exception_message)
        || !g_module_symbol (jsc.module, "jsc_value_to_double", (gpointer *) &jsc.to_double)
        || !g_module_symbol (jsc.module, "jsc_value_is_number", (gpointer *) &jsc.is_number)) {
        g_module_close (jsc.module);
        jsc.module = NULL;
        return FALSE;
    }
    return TRUE;
}

static double
js_number (JSCContext *context, const char *script)
{
    JSCValue *value = jsc.evaluate (context, script, -1);
    JSCException *exception = jsc.get_exception (context);
    double result;

    if (exception != NULL)
        g_error ("script failed: %s\n%s", jsc.exception_message (exception), script);
    g_assert_true (jsc.is_number (value));
    result = jsc.to_double (value);
    g_object_unref (value);
    return result;
}

/* Math.round(): floor (v + 0.5), without libm */
static double
js_round (double v)
{
    double f;

    v += 0.5;
    f = (double) (gint64) v;
    return f > v ? f - 1 : f;
}

/* A, WebKit's scale change, B against a fake window: B scrolls to uc_magnify_scroll_target() rounded */
static void
check_js_case (JSCContext *context, guint id, double scale0, double left0, double top0, double scale1, double zoom,
               double fx, double fy)
{
    char *setup, *a, *b;
    double left, top;

    {
        char s0[G_ASCII_DTOSTR_BUF_SIZE], l0[G_ASCII_DTOSTR_BUF_SIZE], t0[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (s0, sizeof s0, scale0);
        g_ascii_dtostr (l0, sizeof l0, left0);
        g_ascii_dtostr (t0, sizeof t0, top0);
        setup = g_strdup_printf ("visualViewport = { scale: %s, pageLeft: %s, pageTop: %s }; scrolled = null; 0", s0,
                                 l0, t0);
    }
    g_assert_cmpfloat (js_number (context, setup), ==, 0);
    g_free (setup);
    a = uc_magnify_script_before (id);
    g_assert_cmpfloat (js_number (context, a), ==, scale0);
    g_free (a);
    {
        char s1[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (s1, sizeof s1, scale1);
        setup = g_strdup_printf ("visualViewport.scale = %s; 0", s1);
        js_number (context, setup);
        g_free (setup);
    }
    b = uc_magnify_script_after (id, fx, fy, zoom);
    g_assert_cmpfloat (js_number (context, b), ==, scale1);
    g_free (b);
    uc_magnify_scroll_target (scale0, left0, top0, scale1, zoom, fx, fy, &left, &top);
    g_assert_cmpfloat (js_number (context, "scrolled.left"), ==, js_round (left));
    g_assert_cmpfloat (js_number (context, "scrolled.top"), ==, js_round (top));
    g_assert_cmpfloat (js_number (context, "scrolled.instant"), ==, 1);
    /* the record is gone: B again does nothing */
    b = uc_magnify_script_after (id, fx, fy, zoom);
    g_assert_cmpfloat (js_number (context, b), ==, -1);
    g_free (b);
}

static void
test_scripts_in_jsc (void)
{
    JSCContext *context;

    if (!jsc_load ()) {
        g_test_skip ("no libjavascriptcoregtk on this host");
        return;
    }
    context = jsc.context_new ();
    /* the fake window: the global object, with scrollTo() recording its argument */
    js_number (context, "var window = this; scrollTo = function (o) {"
                        " scrolled = { left: o.left, top: o.top, instant: o.behavior === 'instant' ? 1 : 0 }; }; 0");
    check_js_case (context, 1, 1, 500, 1000, 2, 1, 100, 700);
    check_js_case (context, 2, 2, 550, 1350, 3.5, 1, 400, 100);
    check_js_case (context, 3, 1, 400, 900, 2.5, 1.5, 300, 200);
    check_js_case (context, 4, 3.5, 636, 1370.84375, 1, 1, 240, 400);
    check_js_case (context, 5, 1, 0, 0, 2, 0.5, 479.5, 799.25);
    check_js_case (context, 6, 1.75, 12.25, 3.5, 1.2, 2.0, 17, 23);
    /* a page without visualViewport: A and B report -1, nothing scrolls */
    js_number (context, "delete this.visualViewport; scrolled = null; 0");
    {
        char *a = uc_magnify_script_before (9), *b = uc_magnify_script_after (9, 1, 1, 1);
        g_assert_cmpfloat (js_number (context, a), ==, -1);
        g_assert_cmpfloat (js_number (context, b), ==, -1);
        g_assert_cmpfloat (js_number (context, "scrolled === null ? 1 : 0"), ==, 1);
        g_free (a);
        g_free (b);
    }
    g_object_unref (context);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/magnify/range", test_range);
    g_test_add_func ("/magnify/clamp-focal", test_clamp_focal);
    g_test_add_func ("/magnify/scroll-target", test_scroll_target);
    g_test_add_func ("/magnify/scripts", test_scripts);
    g_test_add_func ("/magnify/scripts-locale", test_scripts_locale);
    g_test_add_func ("/magnify/scripts-in-jsc", test_scripts_in_jsc);
    return g_test_run ();
}
