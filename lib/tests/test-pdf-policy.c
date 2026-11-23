/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_pdf_policy.c (GLib only): run with `make check`. When the host has WebKitGTK 2.54
 * (libwebkit2gtk-4.1.so.0 or libwebkitgtk-6.0.so.4, loaded with GModule, no build dependency), the identifier
 * uc_webview.c looks up is also checked against WebKit's own feature list: GTK and WPE share WebKitSettings.cpp,
 * WebKitFeature.cpp and the preference (UnifiedWebPreferences.yaml:6491-6506). Switching the feature in a
 * WebKitSettings needs a GTK display on WebKitGTK; that part and its effect on loads are the broadway check in the
 * package README ("Host checks").
 */
#include "uc_pdf_policy.h"

#include <gmodule.h>
#include <string.h>

static void
test_mime_types (void)
{
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("application/pdf"));
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("text/pdf"));
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("application/x-pdf"));
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("Application/PDF"));     /* a server's spelling, WebKit keeps it */
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("TEXT/PDF"));
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("application/pdf; charset=binary"));
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("application/pdf;"));
    g_assert_true (uc_pdf_policy_is_pdf_mime_type (" application/pdf "));
    g_assert_true (uc_pdf_policy_is_pdf_mime_type ("\tapplication/x-pdf\t;q=1"));

    g_assert_false (uc_pdf_policy_is_pdf_mime_type (NULL));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type (""));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type (" "));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type (";application/pdf"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("application/pd"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("application/pdfx"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("application/x-pdf2"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("application/vnd.pdf"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("image/pdf"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("text/html"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("text/plain"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("application/octet-stream"));
    g_assert_false (uc_pdf_policy_is_pdf_mime_type ("application/xhtml+xml"));
}

static void
test_check_response (void)
{
    static const char *const shown[] = { "text/html", "text/plain", "image/png", "application/json",
                                         "application/octet-stream", "video/mp4", NULL, "" };

    /* viewer off (production): every PDF type is ignored, everything else is WebKit's decision */
    g_assert_cmpint (uc_pdf_policy_check_response ("application/pdf", FALSE), ==, UC_PDF_POLICY_IGNORE);
    g_assert_cmpint (uc_pdf_policy_check_response ("text/pdf", FALSE), ==, UC_PDF_POLICY_IGNORE);
    g_assert_cmpint (uc_pdf_policy_check_response ("application/x-pdf", FALSE), ==, UC_PDF_POLICY_IGNORE);
    g_assert_cmpint (uc_pdf_policy_check_response ("Application/PDF", FALSE), ==, UC_PDF_POLICY_IGNORE);
    for (gsize i = 0; i < G_N_ELEMENTS (shown); i++)
        g_assert_cmpint (uc_pdf_policy_check_response (shown[i], FALSE), ==, UC_PDF_POLICY_DEFAULT);

    /* UC_WEBVIEW_PDF_VIEWER=1: the library stays out of every decision */
    g_assert_cmpint (uc_pdf_policy_check_response ("application/pdf", TRUE), ==, UC_PDF_POLICY_DEFAULT);
    g_assert_cmpint (uc_pdf_policy_check_response ("text/pdf", TRUE), ==, UC_PDF_POLICY_DEFAULT);
    g_assert_cmpint (uc_pdf_policy_check_response ("application/x-pdf", TRUE), ==, UC_PDF_POLICY_DEFAULT);
    for (gsize i = 0; i < G_N_ELEMENTS (shown); i++)
        g_assert_cmpint (uc_pdf_policy_check_response (shown[i], TRUE), ==, UC_PDF_POLICY_DEFAULT);
}

static void
test_env (void)
{
    static const char *const on[] = { "1", "true", "TRUE", "yes", "on", "On" };
    static const char *const off[] = { "", "0", "false", "no", "off", "2", " 1", "enabled" };
    char *saved = g_strdup (g_getenv (UC_PDF_VIEWER_ENV));

    g_assert_cmpstr (UC_PDF_VIEWER_ENV, ==, "UC_WEBVIEW_PDF_VIEWER");
    g_unsetenv (UC_PDF_VIEWER_ENV);
    g_assert_false (uc_pdf_policy_viewer_requested ());   /* production default: off */
    for (gsize i = 0; i < G_N_ELEMENTS (on); i++) {
        g_setenv (UC_PDF_VIEWER_ENV, on[i], TRUE);
        g_assert_true (uc_pdf_policy_viewer_requested ());
    }
    for (gsize i = 0; i < G_N_ELEMENTS (off); i++) {
        g_setenv (UC_PDF_VIEWER_ENV, off[i], TRUE);
        g_assert_false (uc_pdf_policy_viewer_requested ());
    }
    if (saved != NULL)
        g_setenv (UC_PDF_VIEWER_ENV, saved, TRUE);
    else
        g_unsetenv (UC_PDF_VIEWER_ENV);
    g_free (saved);
}

/* WebKit's identifier is the preference name without "Enabled" (WebKitFeature.cpp:131-136) */
static void
test_feature_identifier (void)
{
    const char *preference = "PDFJSViewerEnabled";
    g_assert_true (g_str_has_suffix (preference, "Enabled"));
    g_assert_cmpint (strlen (UC_PDF_VIEWER_FEATURE), ==, strlen (preference) - strlen ("Enabled"));
    g_assert_cmpint (strncmp (UC_PDF_VIEWER_FEATURE, preference, strlen (UC_PDF_VIEWER_FEATURE)), ==, 0);
}

/* ---- the feature switch against WebKitGTK, when the host has it ---------------------------------------------- */

typedef struct _WebKitFeature WebKitFeature;
typedef struct _WebKitFeatureList WebKitFeatureList;

static struct {
    GModule *module;
    const char *name;
    WebKitFeatureList *(*get_all_features) (void);
    WebKitFeature *(*list_find) (WebKitFeatureList *, const char *);
    void (*list_unref) (WebKitFeatureList *);
    gboolean (*get_default_value) (WebKitFeature *);
    const char *(*get_identifier) (WebKitFeature *);
} wk;

static gboolean
load_webkit (void)
{
    static const char *const names[] = { "libwebkit2gtk-4.1.so.0", "libwebkitgtk-6.0.so.4", NULL };

    if (wk.module != NULL)
        return TRUE;
    for (int i = 0; names[i] != NULL && wk.module == NULL; i++) {
        wk.module = g_module_open (names[i], G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
        wk.name = names[i];
    }
    if (wk.module == NULL)
        return FALSE;
    /* webkit_feature_list_find() is new in 2.54 */
    if (!g_module_symbol (wk.module, "webkit_settings_get_all_features", (gpointer *) &wk.get_all_features)
        || !g_module_symbol (wk.module, "webkit_feature_list_find", (gpointer *) &wk.list_find)
        || !g_module_symbol (wk.module, "webkit_feature_list_unref", (gpointer *) &wk.list_unref)
        || !g_module_symbol (wk.module, "webkit_feature_get_default_value", (gpointer *) &wk.get_default_value)
        || !g_module_symbol (wk.module, "webkit_feature_get_identifier", (gpointer *) &wk.get_identifier)) {
        g_module_close (wk.module);
        wk.module = NULL;
        return FALSE;
    }
    return TRUE;
}

static void
test_webkit_feature (void)
{
    WebKitFeatureList *features;
    WebKitFeature *feature;

    if (!load_webkit ()) {
        g_test_skip ("no WebKitGTK 2.54 on this host");
        return;
    }
    g_test_message ("feature list of %s", wk.name);
    features = wk.get_all_features ();
    /* the preference's own name is not an identifier: a switch by that name would miss */
    g_assert_null (wk.list_find (features, "PDFJSViewerEnabled"));
    feature = wk.list_find (features, UC_PDF_VIEWER_FEATURE);
    if (feature == NULL) {
        wk.list_unref (features);
        g_test_skip ("this WebKitGTK is built without PDF.js");
        return;
    }
    g_assert_cmpstr (wk.get_identifier (feature), ==, UC_PDF_VIEWER_FEATURE);
    g_assert_true (wk.get_default_value (feature));   /* on by default for GTK and WPE: the library must switch it off */
    wk.list_unref (features);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/pdf-policy/mime-types", test_mime_types);
    g_test_add_func ("/pdf-policy/check-response", test_check_response);
    g_test_add_func ("/pdf-policy/env", test_env);
    g_test_add_func ("/pdf-policy/feature-identifier", test_feature_identifier);
    g_test_add_func ("/pdf-policy/webkit-feature", test_webkit_feature);
    return g_test_run ();
}
