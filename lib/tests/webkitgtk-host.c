/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The uc_webview.h API on WebKitGTK 2.54 (libwebkit2gtk-4.1), for the request modes of ucwebview-test on a build host
 * (make ucwebview-test-gtk, package README "Host checks"). Not part of the library.
 *
 * The request handling is the library's own: uc_requests_wk.c, uc_requests.c, uc_trace.c, uc_pdf_policy.c and
 * uc_abi.c are compiled in unchanged, against the generated WPE 2.54.1 headers (tests/mk-host-headers.py): WebKitGTK
 * and WPE share WebKitWebView.cpp, WebKitScriptDialog.cpp, WebKitAuthenticationRequest.cpp, WebKitDownload.cpp and
 * the policy decisions, with the same function signatures and enum values for everything those files call. This
 * file stands in for uc_webview.c: it creates the WebKitWebView in a GTK window (a broadway display is enough),
 * calls the same hooks in the same order, and leaves out what WebKitGTK cannot share (WPEPlatform: frames, input
 * events, whose presses only end the dialog loop guard, IME, pinch). Two differences of the GTK port are bridged here:
 *  - WebKitGTK shows its own dialogs where WPE closes them at once (WebKitWebViewGtk.cpp:43-80): a script-dialog
 *    handler after the library's returns TRUE without a reference, which closes the dialog with its default answer
 *    as on WPE (WebKitWebViewWPE.cpp:39-47); authenticate is always taken by the library.
 *  - The 4.1 API emits download-started on the WebKitWebContext, not on a WebKitNetworkSession
 *    (WebKitNavigationClient.cpp:156-163).
 * No WebKitGTK or GTK headers are installed on the build host, so the few GTK and 4.1-only functions are declared
 * here.
 *
 * Site settings (library 0.8.0): uc_site_wk.c, uc_site.c and uc_headers.c are compiled in unchanged; where the 4.1 API
 * keeps what WPE 2.0 has in WebKitNetworkSession (the cookie manager, the TLS policy and exceptions, the data
 * directories) this file uses the WebKitWebContext and its WebKitWebsiteDataManager. The origin headers come from the
 * library's web process extension built for WebKitGTK 4.1 (make ucwebview-test-gtk builds
 * tests/gtk-ext/libucwebview-extension.so from uc_extension.c). The color scheme is GTK's
 * gtk-application-prefer-dark-theme instead of WPE's dark-mode setting.
 *
 * Device state for web pages (library 0.9.0): uc_device_wk.c, uc_page_wk.c and uc_device.c are compiled in unchanged,
 * with the same hooks (the view's user content manager, the load and termination events, the state after FINISHED),
 * and the web process extension built for WebKitGTK 4.1 runs the shim (uc_ext_device.c). uc_webview_set_mapped() hides
 * and shows the GTK window, so that the page is hidden as on the remote (--device-test's throttling step; WebKitGTK
 * throttles hidden pages by default, so there the step checks the tool, not the library's switch;
 * UC_HARNESS_NO_THROTTLE=1 switches it off, the step's negative control).
 *
 * uc_webview_when_rendered() (since library 0.9.0 here): uc_rendered.c and the probe (uc_rendered_probe_script()) are
 * the library's, sent through uc_page_wk.c to the extension like in uc_webview.c, with the report handler in the
 * library's world. WebKitGTK hands the harness no frames, so a request ends at its rendering update's report plus the
 * library's grace for a frame (DONE without a frame). The 4.1 API registers a handler for a world with
 * ..._register_script_message_handler_in_world() and passes the message as a WebKitJavascriptResult (WPE 2.0: the
 * world is an argument of ..._register_script_message_handler(), the message a JSCValue).
 *
 * Process swaps (library 0.8.1): WPE swaps the web process at every cross-site main-frame navigation, also at a
 * server redirect (WebKitWebContext.cpp:442-446); WebKitGTK 4.1 only with the context property
 * process-swap-on-cross-site-navigation-enabled, FALSE by default. The harness sets it, so that redirects behave as on
 * the remote (the 0.8.0 header leak at a cross-site redirect showed on the device only). UC_HARNESS_PSON=0 runs
 * without it.
 */
#include "uc_abi.h"
#include "uc_device_wk.h"
#include "uc_ext_protocol.h"
#include "uc_load_error.h"
#include "uc_page_wk.h"
#include "uc_pdf_policy.h"
#include "uc_rendered.h"
#include "uc_requests_wk.h"
#include "uc_site_wk.h"
#include "uc_trace.h"
#include "uc_uri_policy.h"
#include "uc_webview.h"

#include <string.h>

/* GTK 3 and the WebKitGTK 4.1 API (WebKitWebContext.h.in, gtk/gtkwindow.h, gtk/gtkcontainer.h, gtk/gtkwidget.h) */
typedef struct _GtkWidget GtkWidget;
extern void gtk_init (int *argc, char ***argv);
extern GtkWidget *gtk_window_new (int type);
extern void gtk_window_set_default_size (GtkWidget *window, int width, int height);
extern void gtk_container_add (GtkWidget *container, GtkWidget *widget);
extern void gtk_widget_show_all (GtkWidget *widget);
extern void gtk_widget_show (GtkWidget *widget);
extern void gtk_widget_hide (GtkWidget *widget);
extern void gtk_widget_destroy (GtkWidget *widget);
/* 0.8.0: the 4.1 API's data manager and context functions (WebKitWebsiteDataManager.h.in, WebKitWebContext.h.in
 * without ENABLE(2022_GLIB_API)) and GTK's dark theme switch (gtk/gtksettings.h) */
extern WebKitWebsiteDataManager *webkit_website_data_manager_new (const char *first_option_name, ...);
extern WebKitWebsiteDataManager *webkit_website_data_manager_new_ephemeral (void);
extern WebKitWebContext *webkit_web_context_new_with_website_data_manager (WebKitWebsiteDataManager *manager);
extern WebKitWebsiteDataManager *webkit_web_context_get_website_data_manager (WebKitWebContext *context);
extern WebKitCookieManager *webkit_website_data_manager_get_cookie_manager (WebKitWebsiteDataManager *manager);
extern void webkit_website_data_manager_set_tls_errors_policy (WebKitWebsiteDataManager *manager,
                                                               WebKitTLSErrorsPolicy policy);
extern void webkit_web_context_allow_tls_certificate_for_host (WebKitWebContext *context, GTlsCertificate *certificate,
                                                               const char *host);
extern void webkit_web_context_set_web_extensions_directory (WebKitWebContext *context, const char *directory);
extern void webkit_web_context_set_web_extensions_initialization_user_data (WebKitWebContext *context,
                                                                            GVariant *user_data);
extern gpointer gtk_settings_get_default (void);
/* 0.9.0: the rendered probe's handler with the 4.1 API (WebKitUserContentManager.h.in without ENABLE(2022_GLIB_API),
 * WebKitJavascriptResult.h) */
typedef struct _WebKitJavascriptResult WebKitJavascriptResult;
extern gboolean webkit_user_content_manager_register_script_message_handler_in_world (WebKitUserContentManager *manager,
                                                                                      const char *name,
                                                                                      const char *world_name);
extern void webkit_user_content_manager_unregister_script_message_handler_in_world (WebKitUserContentManager *manager,
                                                                                    const char *name,
                                                                                    const char *world_name);
extern JSCValue *webkit_javascript_result_get_js_value (WebKitJavascriptResult *js_result);

/* as in uc_webview.c */
#define UC_RENDERED_TIMEOUT_MS 60000
#define UC_RENDERED_GRACE_MS   100

#ifndef UC_HARNESS_EXTENSION_DIR
#define UC_HARNESS_EXTENSION_DIR "tests/gtk-ext"
#endif

struct uc_webview {
    uc_webview_callbacks cb;
    void *user;
    GtkWidget *window;
    WebKitWebView *web_view;
    UcWkRequests *requests;
    UcHeadersTable *headers;
    WebKitSettings *own_settings;
    UcPageWk *page;         /* library 0.9.0 */
    UcDeviceWk *device;
    UcRendered *rendered;   /* uc_webview_when_rendered() */
    WebKitUserContentManager *content_manager;
    gboolean freeing;
    unsigned tls_failure_errors;    /* library 0.10.0, as in uc_webview.c */
    GPtrArray *rejected;            /* URIs the scheme allowlist refused (since 0.10.0 here, as in uc_webview.c) */
    guint rejected_source;
};

static struct {
    gboolean initialised;
    WebKitWebContext *context;
    WebKitSettings *settings;
    gboolean log_full_urls;
    gboolean pdf_viewer;
    int n_views;
    double refresh_rate;
    UcTlsExceptions *tls;
    gboolean tls_ignore;
    gboolean dark;
    UcSiteWkHeaders *headers;
    GPtrArray *views;
    char *default_ua;   /* as in uc_webview.c (library 0.8.2) */
} global = { .refresh_rate = 60.0 };

static int callback_depth;

#define CALLBACK(wv, name, ...) \
    do { \
        if ((wv)->cb.name != NULL) { \
            callback_depth++; \
            (wv)->cb.name ((wv), __VA_ARGS__, (wv)->user); \
            callback_depth--; \
        } \
    } while (0)

#define CHECK_VIEW(wv) do { if ((wv) == NULL || (wv)->web_view == NULL) return; } while (0)
#define CHECK_VIEW_VAL(wv, v) do { if ((wv) == NULL || (wv)->web_view == NULL) return (v); } while (0)

static void
unsupported (const char *what)
{
    static GHashTable *said;
    if (said == NULL)
        said = g_hash_table_new (g_str_hash, g_str_equal);
    if (!g_hash_table_contains (said, what)) {
        g_hash_table_add (said, (gpointer) what);
        g_message ("ucwebview (WebKitGTK host harness): %s is not available here", what);
    }
}

int uc_webview_api_version (void) { return UC_WEBVIEW_API_VERSION; }
const char *uc_webview_library_version (void) { return UC_WEBVIEW_VERSION; }

/* ---- the same hooks as uc_webview.c ------------------------------------------------------------------------- */

static void
on_download_started (WebKitWebContext *context, WebKitDownload *download, gpointer user_data)
{
    WebKitWebView *web_view = webkit_download_get_web_view (download);
    uc_webview *wv = web_view != NULL ? g_object_get_data (G_OBJECT (web_view), "ucwebview") : NULL;

    (void) context; (void) user_data;
    uc_wk_requests_download_started (wv != NULL ? wv->requests : NULL, download, global.log_full_urls);
}

static void
on_load_changed (WebKitWebView *web_view, WebKitLoadEvent event, gpointer user_data)
{
    uc_webview *wv = user_data;
    if (event == WEBKIT_LOAD_STARTED) {
        wv->tls_failure_errors = 0;
        uc_site_wk_navigation (web_view, TRUE);
    }
    uc_page_wk_load_changed (wv->page, event);
    if (event == WEBKIT_LOAD_COMMITTED && wv->rendered != NULL)
        uc_rendered_cancel_all (wv->rendered, "navigation");
    uc_wk_requests_load_changed (wv->requests, event);
    if (event == WEBKIT_LOAD_FINISHED)
        uc_device_wk_load_finished (wv->device);
    CALLBACK (wv, load_changed, (int) event, webkit_web_view_get_uri (web_view));
}

static gboolean
on_load_failed (WebKitWebView *web_view, WebKitLoadEvent event, const char *failing_uri, GError *error, gpointer user_data)
{
    uc_webview *wv = user_data;
    gboolean refused = uc_wk_requests_load_failed (wv->requests, error);
    unsigned tls_errors = wv->tls_failure_errors;
    UcLoadFailure failure;
    char *loggable;
    char *message;

    wv->tls_failure_errors = 0;
    if (g_error_matches (error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) || refused)
        return FALSE;
    /* a load on the connection of a preconnect that failed on the certificate: loaded again (0.10.3) */
    if (uc_site_wk_load_cancelled (web_view, event, failing_uri, error, global.log_full_urls))
        return TRUE;
    uc_load_failure_from_error (&failure, error, tls_errors);
    loggable = uc_uri_policy_loggable (failing_uri, global.log_full_urls);
    message = uc_uri_policy_scrub_text (failure.message, global.log_full_urls);
    g_message ("ucwebview: load failed: %s: %s (%s %d)", loggable, message,
               *failure.domain != '\0' ? failure.domain : "no domain", failure.code);
    g_free (message);
    g_free (loggable);
    CALLBACK (wv, load_failed_detail, failing_uri, failure.reason, failure.domain, failure.code, failure.tls_errors,
              failure.message);
    CALLBACK (wv, load_changed, UC_WEBVIEW_LOAD_FAILED, failing_uri);
    return FALSE;
}

static void
on_title_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    uc_webview *wv = user_data;
    (void) pspec;
    CALLBACK (wv, title_changed, webkit_web_view_get_title (WEBKIT_WEB_VIEW (object)));
}

static void
on_progress_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    uc_webview *wv = user_data;
    (void) pspec;
    CALLBACK (wv, progress_changed, webkit_web_view_get_estimated_load_progress (WEBKIT_WEB_VIEW (object)));
}

static void
on_web_process_terminated (WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, gpointer user_data)
{
    uc_webview *wv = user_data;
    (void) web_view;
    g_warning ("ucwebview: web process terminated (reason %d)", (int) reason);
    uc_page_wk_web_process_terminated (wv->page);
    if (wv->rendered != NULL)
        uc_rendered_cancel_all (wv->rendered, "web process terminated");
    uc_wk_requests_web_process_terminated (wv->requests);
    CALLBACK (wv, web_process_terminated, (int) reason);
}

/* the page moved to the web process of a navigation (a process swap, WebKitWebView.cpp:578-581): logged, so that a run
 * shows where --device-test's other-site and home steps swapped */
static void
on_page_id_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) pspec; (void) user_data;
    g_message ("ucwebview (WebKitGTK host harness): page id %" G_GUINT64_FORMAT ": the page moved to another web "
               "process", webkit_web_view_get_page_id (WEBKIT_WEB_VIEW (object)));
}

/* ---- uc_webview_when_rendered(), as in uc_webview.c ---------------------------------------------------------- */

static void
on_rendered_finished (UcRenderedFunc func, gpointer data, const UcRenderedResult *result, gpointer user_data)
{
    uc_webview *wv = user_data;
    uc_webview_rendered_cb cb = (uc_webview_rendered_cb) func;

    uc_trace_rendered (wv, result);
    callback_depth++;
    cb (wv, (int) result->status, data);
    callback_depth--;
}

static void
on_rendered_message (WebKitUserContentManager *manager, WebKitJavascriptResult *js_result, gpointer user_data)
{
    uc_webview *wv = user_data;
    JSCValue *value = webkit_javascript_result_get_js_value (js_result);
    double id;

    (void) manager;
    if (wv->rendered == NULL || !jsc_value_is_number (value))
        return;
    id = jsc_value_to_double (value);
    if (id >= 1.0 && id <= (double) G_MAXUINT && !uc_rendered_update_done (wv->rendered, (guint) id))
        g_debug ("ucwebview: rendered report %u for no pending request (timed out or cancelled)", (guint) id);
}

static void
on_rendered_probe_done (GObject *object, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = uc_page_wk_send_finish (object, result, &error);
    uc_webview *wv = g_object_get_data (object, "ucwebview");
    guint id = GPOINTER_TO_UINT (user_data);
    gboolean ran = FALSE;
    double value = 0;

    if (reply != NULL && g_variant_is_of_type (reply, G_VARIANT_TYPE (UC_EXT_SCRIPT_REPLY_TYPE)))
        g_variant_get (reply, UC_EXT_SCRIPT_REPLY_TYPE, &ran, &value);
    g_clear_pointer (&reply, g_variant_unref);
    if (ran)
        return;
    g_message ("ucwebview: rendered probe %u did not run (%s %d)", id,
               error != NULL ? g_quark_to_string (error->domain) : "exception", error != NULL ? error->code : 0);
    g_clear_error (&error);
    if (wv != NULL && wv->rendered != NULL)
        uc_rendered_cancel (wv->rendered, id, "script refused");
}

int
uc_webview_when_rendered (uc_webview *wv, uc_webview_rendered_cb cb, void *user)
{
    guint id;
    char *script;

    if (wv == NULL || wv->web_view == NULL || wv->rendered == NULL || cb == NULL || wv->freeing)
        return -1;
    id = uc_rendered_add (wv->rendered, (UcRenderedFunc) cb, user);
    if (id == 0)
        return -1;
    script = uc_rendered_probe_script (id);
    uc_page_wk_send_with_reply (wv->page, UC_EXT_SCRIPT_MESSAGE, g_variant_new (UC_EXT_SCRIPT_VARIANT_TYPE, script),
                                on_rendered_probe_done, GUINT_TO_POINTER (id));
    g_free (script);
    return 0;
}

/* WPE's default instead of WebKitGTK's dialog: closed at once with the values set so far */
static gboolean
on_script_dialog_wpe_default (WebKitWebView *web_view, WebKitScriptDialog *dialog, gpointer user_data)
{
    (void) web_view; (void) dialog; (void) user_data;
    return TRUE;
}

/* ---- site settings, as in uc_webview.c with the 4.1 API --------------------------------------------------------- */

static WebKitSettings *
settings_new (void)
{
    WebKitSettings *settings = webkit_settings_new ();
    /* the PDF viewer as in uc_webview_global_init() (uc_pdf_policy.h) */
    WebKitFeatureList *features = webkit_settings_get_all_features ();
    WebKitFeature *feature = webkit_feature_list_find (features, UC_PDF_VIEWER_FEATURE);
    if (feature != NULL)
        webkit_settings_set_feature_enabled (settings, feature, global.pdf_viewer);
    /* the hidden page throttling of 0.9.0 is WebKitGTK's default; UC_HARNESS_NO_THROTTLE=1 switches it off, the
     * negative control of --device-test's throttling step */
    if (g_strcmp0 (g_getenv ("UC_HARNESS_NO_THROTTLE"), "1") == 0) {
        static const char *const ids[] = { UC_DEVICE_FEATURE_TIMER_THROTTLING, UC_DEVICE_FEATURE_ANIMATION_SUSPEND };
        for (gsize i = 0; i < G_N_ELEMENTS (ids); i++) {
            WebKitFeature *throttle = webkit_feature_list_find (features, ids[i]);
            if (throttle != NULL)
                webkit_settings_set_feature_enabled (settings, throttle, FALSE);
        }
    }
    webkit_feature_list_unref (features);
    if (global.default_ua != NULL)
        webkit_settings_set_user_agent (settings, global.default_ua);
    return settings;
}

static WebKitWebsiteDataManager *
data_manager (void)
{
    return webkit_web_context_get_website_data_manager (global.context);
}

static void
apply_tls_policy (void)
{
    webkit_website_data_manager_set_tls_errors_policy (data_manager (), global.tls_ignore ? WEBKIT_TLS_ERRORS_POLICY_IGNORE
                                                                                           : WEBKIT_TLS_ERRORS_POLICY_FAIL);
}

/* GTK's equivalent of WPE's dark-mode setting */
static void
apply_color_scheme (void)
{
    g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", global.dark, NULL);
}

static void
allow_certificate (GTlsCertificate *certificate, const char *host, gpointer user_data)
{
    (void) user_data;
    webkit_web_context_allow_tls_certificate_for_host (global.context, certificate, host);
}

static gboolean
on_load_failed_with_tls_errors (WebKitWebView *web_view, const char *failing_uri, GTlsCertificate *certificate,
                                GTlsCertificateFlags errors, gpointer user_data)
{
    uc_webview *wv = user_data;
    gboolean accepted = uc_site_wk_tls_error (web_view, failing_uri, certificate, errors, global.tls,
                                              allow_certificate, NULL, global.log_full_urls);

    wv->tls_failure_errors = accepted ? 0 : (unsigned) errors;
    return accepted;
}

/* WebKitWebContext::initialize-web-extensions of the 4.1 API */
static void
on_initialize_web_extensions (WebKitWebContext *context, gpointer user_data)
{
    (void) user_data;
    if (global.headers != NULL)
        webkit_web_context_set_web_extensions_initialization_user_data (context,
                                                                        uc_site_wk_headers_user_data (global.headers));
}

static void
setup_extension (void)
{
    char *path = g_build_filename (UC_HARNESS_EXTENSION_DIR, UC_SITE_WK_EXTENSION_NAME, NULL);
    gboolean found = g_file_test (path, G_FILE_TEST_IS_REGULAR);

    g_free (path);
    if (!found)
        return;
    webkit_web_context_set_web_extensions_directory (global.context, UC_HARNESS_EXTENSION_DIR);
    g_signal_connect (global.context, "initialize-web-extensions", G_CALLBACK (on_initialize_web_extensions), NULL);
    global.headers = uc_site_wk_headers_new (global.context);
}

static void
publish_headers (void)
{
    GPtrArray *tables;

    if (global.headers == NULL)
        return;
    tables = g_ptr_array_new ();
    for (guint i = 0; i < global.views->len; i++)
        g_ptr_array_add (tables, ((uc_webview *) g_ptr_array_index (global.views, i))->headers);
    uc_site_wk_headers_publish (global.headers, tables);
    g_ptr_array_unref (tables);
}

/* The context with WPE's process swaps unless UC_HARNESS_PSON=0 (WebKitWebContext.cpp:442-446) */
static WebKitWebContext *
context_new (WebKitWebsiteDataManager *manager)
{
    const char *env = g_getenv ("UC_HARNESS_PSON");
    gboolean pson = env == NULL || g_strcmp0 (env, "0") != 0;

    g_message ("ucwebview (WebKitGTK host harness): process swap on cross-site navigation %s%s", pson ? "on" : "off",
               pson ? " (as WPE)" : " (UC_HARNESS_PSON=0)");
    return g_object_new (WEBKIT_TYPE_WEB_CONTEXT, "website-data-manager", manager,
                         "process-swap-on-cross-site-navigation-enabled", pson, NULL);
}

/* ---- process ------------------------------------------------------------------------------------------------ */

int
uc_webview_global_init (void *egl_display, const char *data_dir, const char *cache_dir)
{
    WebKitFeatureList *features;
    WebKitFeature *feature;

    (void) egl_display; (void) features; (void) feature;
    if (global.initialised)
        return 0;
    gtk_init (NULL, NULL);
    global.log_full_urls = uc_env_value_true (g_getenv ("UC_WEBVIEW_LOG_FULL_URLS"));
    global.pdf_viewer = uc_pdf_policy_viewer_requested ();
    uc_trace_set_enabled (uc_env_value_true (g_getenv ("UC_WEBVIEW_TRACE")));
    if (data_dir != NULL && *data_dir != '\0' && g_mkdir_with_parents (data_dir, 0700) == 0) {
        /* a persistent session like create_session() of uc_webview.c, cookies in data_dir/cookies.sqlite */
        char *cache = cache_dir != NULL && *cache_dir != '\0' ? g_strdup (cache_dir) : g_build_filename (data_dir, "cache", NULL);
        char *cookies = g_build_filename (data_dir, "cookies.sqlite", NULL);
        uc_site_private_file (cookies);     /* as create_session() of uc_webview.c (0.8.3) */
        WebKitWebsiteDataManager *manager = webkit_website_data_manager_new ("base-data-directory", data_dir,
                                                                             "base-cache-directory", cache, NULL);
        global.context = context_new (manager);
        g_object_unref (manager);
        webkit_cookie_manager_set_persistent_storage (webkit_website_data_manager_get_cookie_manager (
                                                          webkit_web_context_get_website_data_manager (global.context)),
                                                      cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        g_free (cache);
        g_free (cookies);
    } else {
        WebKitWebsiteDataManager *manager = webkit_website_data_manager_new_ephemeral ();
        global.context = context_new (manager);
        g_object_unref (manager);
    }
    g_signal_connect (global.context, "download-started", G_CALLBACK (on_download_started), NULL);
    global.settings = settings_new ();
    {
        /* the DEFAULT user agent as setup_default_user_agent() of uc_webview.c makes it */
        const char *source = NULL;
        char *version = uc_site_firmware_version (UC_SITE_RELEASE_FILE, g_getenv ("UC_OS_VERSION"), &source);
        webkit_settings_set_user_agent (global.settings, NULL);
        g_free (global.default_ua);
        global.default_ua = uc_site_default_user_agent (webkit_settings_get_user_agent (global.settings), version);
        webkit_settings_set_user_agent (global.settings, global.default_ua);
        g_message ("ucwebview: default user agent \"%s\" (%s)", global.default_ua, source);
        g_free (version);
    }
    if (global.tls == NULL)
        global.tls = uc_tls_exceptions_new ();
    if (global.views == NULL)
        global.views = g_ptr_array_new ();
    apply_tls_policy ();
    apply_color_scheme ();
    setup_extension ();
    global.initialised = TRUE;
    g_message ("ucwebview %s (API %d, WebKitGTK host harness): WebKitGTK %u.%u.%u, %s context, %s%s, %s",
               uc_webview_library_version (), UC_WEBVIEW_API_VERSION,
               webkit_get_major_version (), webkit_get_minor_version (), webkit_get_micro_version (),
               data_dir != NULL ? "persistent" : "ephemeral", global.pdf_viewer ? "PDF viewer on" : "PDF viewer off",
               uc_trace_enabled () ? ", input trace on" : "",
               global.headers != NULL ? "web process extension " UC_HARNESS_EXTENSION_DIR : "no web process extension");
    return 0;
}

void
uc_webview_global_shutdown (void)
{
    if (!global.initialised || global.n_views > 0)
        return;
    g_clear_object (&global.settings);
    g_clear_pointer (&global.headers, uc_site_wk_headers_free);
    g_signal_handlers_disconnect_by_func (global.context, G_CALLBACK (on_download_started), NULL);
    g_signal_handlers_disconnect_by_func (global.context, G_CALLBACK (on_initialize_web_extensions), NULL);
    g_clear_object (&global.context);
    global.initialised = FALSE;
}

int uc_webview_global_set_refresh_rate (double hz) { if (!(hz >= 20 && hz <= 240)) return -1; global.refresh_rate = hz; return 0; }
double uc_webview_global_get_refresh_rate (void) { return global.refresh_rate; }
int uc_webview_global_get_drag_threshold (void) { return 16; }

/* ---- views -------------------------------------------------------------------------------------------------- */

static uc_webview *
view_new (int width, int height, const uc_webview_callbacks *callbacks, void *user)
{
    uc_webview *wv;

    if (!global.initialised)
        return NULL;
    wv = g_new0 (uc_webview, 1);
    wv->cb = *callbacks;
    wv->user = user;
    wv->window = gtk_window_new (0 /* GTK_WINDOW_TOPLEVEL */);
    gtk_window_set_default_size (wv->window, width, height);
    wv->web_view = g_object_ref_sink (g_object_new (WEBKIT_TYPE_WEB_VIEW, "web-context", global.context,
                                                    "settings", global.settings, NULL));
    gtk_container_add (wv->window, (GtkWidget *) wv->web_view);
    g_signal_connect (wv->web_view, "load-changed", G_CALLBACK (on_load_changed), wv);
    g_signal_connect (wv->web_view, "load-failed", G_CALLBACK (on_load_failed), wv);
    g_signal_connect (wv->web_view, "notify::title", G_CALLBACK (on_title_notify), wv);
    g_signal_connect (wv->web_view, "notify::estimated-load-progress", G_CALLBACK (on_progress_notify), wv);
    g_signal_connect (wv->web_view, "web-process-terminated", G_CALLBACK (on_web_process_terminated), wv);
    g_signal_connect (wv->web_view, "notify::page-id", G_CALLBACK (on_page_id_notify), wv);
    g_signal_connect (wv->web_view, "load-failed-with-tls-errors", G_CALLBACK (on_load_failed_with_tls_errors), wv);
    g_object_set_data (G_OBJECT (wv->web_view), "ucwebview", wv);
    {
        UcWkRequestsHost requests_host = { wv, &wv->cb, user, &callback_depth, global.log_full_urls, global.pdf_viewer };
        wv->requests = uc_wk_requests_new (wv->web_view, &requests_host);
    }
    wv->page = uc_page_wk_new (wv->web_view);
    wv->device = uc_device_wk_new (wv->web_view, wv->page);
    wv->rendered = uc_rendered_new (UC_RENDERED_TIMEOUT_MS, UC_RENDERED_GRACE_MS, on_rendered_finished, wv);
    wv->content_manager = webkit_web_view_get_user_content_manager (wv->web_view);
    if (!webkit_user_content_manager_register_script_message_handler_in_world (wv->content_manager, UC_RENDERED_HANDLER,
                                                                               UC_EXT_HOST_WORLD))
        g_warning ("ucwebview: cannot register the rendered probe's message handler");
    g_signal_connect (wv->content_manager, "script-message-received::" UC_RENDERED_HANDLER,
                      G_CALLBACK (on_rendered_message), wv);
    /* after the library's handler: WPE's default for the dialogs the library leaves to WebKit */
    g_signal_connect (wv->web_view, "script-dialog", G_CALLBACK (on_script_dialog_wpe_default), NULL);
    gtk_widget_show_all (wv->window);
    global.n_views++;
    g_ptr_array_add (global.views, wv);
    return wv;
}

uc_webview *
uc_webview_new (int width, int height, const uc_webview_callbacks *cb, void *user)
{
    uc_webview_callbacks callbacks;
    uc_abi_callbacks_copy_v1 (&callbacks, cb);
    return view_new (width, height, &callbacks, user);
}

uc_webview *
uc_webview_new_sized (int width, int height, const uc_webview_callbacks *cb, size_t cb_size, void *user)
{
    uc_webview_callbacks callbacks;
    if (uc_abi_callbacks_copy (&callbacks, cb, cb_size) == UC_ABI_COPY_TOO_SMALL)
        return NULL;
    return view_new (width, height, &callbacks, user);
}

void
uc_webview_free (uc_webview *wv)
{
    if (wv == NULL)
        return;
    if (callback_depth > 0) {
        g_critical ("ucwebview: uc_webview_free() called from inside a callback; leaking the view");
        return;
    }
    wv->freeing = TRUE;
    if (wv->rejected_source != 0)
        g_source_remove (wv->rejected_source);
    g_clear_pointer (&wv->rejected, g_ptr_array_unref);
    uc_rendered_free (wv->rendered, "view freed");
    wv->rendered = NULL;
    g_signal_handlers_disconnect_by_data (wv->content_manager, wv);
    webkit_user_content_manager_unregister_script_message_handler_in_world (wv->content_manager, UC_RENDERED_HANDLER,
                                                                            UC_EXT_HOST_WORLD);
    uc_wk_requests_free (wv->requests);
    g_clear_pointer (&wv->device, uc_device_wk_free);
    g_clear_pointer (&wv->page, uc_page_wk_free);
    g_object_set_data (G_OBJECT (wv->web_view), "ucwebview", NULL);
    g_signal_handlers_disconnect_by_data (wv->web_view, wv);
    webkit_web_view_stop_loading (wv->web_view);
    gtk_widget_destroy (wv->window);
    g_clear_object (&wv->web_view);
    g_clear_object (&wv->own_settings);
    global.n_views--;
    g_ptr_array_remove (global.views, wv);
    if (wv->headers != NULL) {
        g_clear_pointer (&wv->headers, uc_headers_table_free);
        publish_headers ();
    }
    g_free (wv);
}

/* ---- navigation, as in uc_webview.c ------------------------------------------------------------------------- */

static gboolean
report_rejected_loads (gpointer user_data)
{
    uc_webview *wv = user_data;
    GPtrArray *uris = wv->rejected;

    wv->rejected = NULL;
    wv->rejected_source = 0;
    for (guint i = 0; i < uris->len; i++) {
        const char *uri = g_ptr_array_index (uris, i);
        UcLoadFailure failure;

        uc_load_failure_scheme_refused (&failure);
        CALLBACK (wv, load_failed_detail, uri, failure.reason, failure.domain, failure.code, failure.tls_errors,
                  failure.message);
        CALLBACK (wv, load_changed, UC_WEBVIEW_LOAD_FAILED, uri);
    }
    g_ptr_array_unref (uris);
    return G_SOURCE_REMOVE;
}

void
uc_webview_load_uri (uc_webview *wv, const char *uri)
{
    CHECK_VIEW (wv);
    if (uri == NULL || *uri == '\0')
        return;
    if (uc_uri_policy_check (uri, FALSE) != UC_URI_ALLOWED) {
        /* reported like uc_webview.c's load_allowed(): FAILED from an idle, since 0.10.0 after the detail */
        g_warning ("ucwebview (WebKitGTK host harness): load_uri refused");
        if (wv->rejected == NULL)
            wv->rejected = g_ptr_array_new_with_free_func (g_free);
        g_ptr_array_add (wv->rejected, g_strdup (uri));
        if (wv->rejected_source == 0)
            wv->rejected_source = g_idle_add_full (G_PRIORITY_DEFAULT, report_rejected_loads, wv, NULL);
        return;
    }
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    uc_site_wk_navigation (wv->web_view, FALSE);
    webkit_web_view_load_uri (wv->web_view, uri);
}

void
uc_webview_load_html (uc_webview *wv, const char *html, const char *base_uri)
{
    CHECK_VIEW (wv);
    if (html == NULL)
        return;
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    uc_site_wk_navigation (wv->web_view, FALSE);
    webkit_web_view_load_html (wv->web_view, html, base_uri);
}

void
uc_webview_reload (uc_webview *wv)
{
    CHECK_VIEW (wv);
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    uc_site_wk_navigation (wv->web_view, FALSE);
    webkit_web_view_reload (wv->web_view);
}

void
uc_webview_stop (uc_webview *wv)
{
    CHECK_VIEW (wv);
    uc_wk_requests_host_navigation (wv->requests, FALSE);
    uc_site_wk_navigation (wv->web_view, FALSE);
    webkit_web_view_stop_loading (wv->web_view);
}

void
uc_webview_go_back (uc_webview *wv)
{
    CHECK_VIEW (wv);
    if (!webkit_web_view_can_go_back (wv->web_view))
        return;
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    uc_site_wk_navigation (wv->web_view, FALSE);
    webkit_web_view_go_back (wv->web_view);
}

void
uc_webview_go_forward (uc_webview *wv)
{
    CHECK_VIEW (wv);
    if (!webkit_web_view_can_go_forward (wv->web_view))
        return;
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    uc_site_wk_navigation (wv->web_view, FALSE);
    webkit_web_view_go_forward (wv->web_view);
}

int uc_webview_can_go_back (uc_webview *wv) { CHECK_VIEW_VAL (wv, 0); return webkit_web_view_can_go_back (wv->web_view); }
int uc_webview_can_go_forward (uc_webview *wv) { CHECK_VIEW_VAL (wv, 0); return webkit_web_view_can_go_forward (wv->web_view); }
const char *uc_webview_get_uri (uc_webview *wv) { CHECK_VIEW_VAL (wv, NULL); return webkit_web_view_get_uri (wv->web_view); }
const char *uc_webview_get_title (uc_webview *wv) { CHECK_VIEW_VAL (wv, NULL); return webkit_web_view_get_title (wv->web_view); }

static void
on_javascript_done (GObject *object, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (object), result, &error);
    (void) user_data;
    if (value == NULL) {
        g_warning ("ucwebview: JavaScript failed: %s", error != NULL ? error->message : "unknown error");
        g_clear_error (&error);
        return;
    }
    g_object_unref (value);
}

void
uc_webview_run_javascript (uc_webview *wv, const char *script)
{
    CHECK_VIEW (wv);
    if (script != NULL)
        webkit_web_view_evaluate_javascript (wv->web_view, script, -1, NULL, NULL, NULL, on_javascript_done, NULL);
}

void uc_webview_set_zoom (uc_webview *wv, double zoom) { CHECK_VIEW (wv); webkit_web_view_set_zoom_level (wv->web_view, zoom); }

/* ---- requests: exactly the library's ------------------------------------------------------------------------ */

int
uc_webview_request_reply (uc_webview *wv, uint64_t request, int accept, const char *text)
{
    CHECK_VIEW_VAL (wv, -1);
    return uc_wk_requests_reply (wv->requests, request, accept, text);
}

int
uc_webview_request_auth (uc_webview *wv, uint64_t request, const char *user, const char *password)
{
    CHECK_VIEW_VAL (wv, -1);
    return uc_wk_requests_auth (wv->requests, request, user, password);
}

/* ---- site settings (library 0.8.0): the library's checks and WebKit code, the 4.1 API's objects ---------------- */

int
uc_webview_set_origin_headers (uc_webview *wv, const char *origin, const char *const *names, const char *const *values,
                               size_t count)
{
    char *normalized;
    const char *reason = NULL;

    CHECK_VIEW_VAL (wv, -1);
    if (global.headers == NULL) {
        g_warning ("ucwebview (WebKitGTK host harness): no web process extension in %s", UC_HARNESS_EXTENSION_DIR);
        return -1;
    }
    normalized = uc_headers_origin (origin);
    if (normalized == NULL || !uc_headers_list_valid (names, values, count, &reason)) {
        g_warning ("ucwebview: set_origin_headers() refused: %s", reason != NULL ? reason : "not an http(s) origin");
        g_free (normalized);
        return -1;
    }
    if (wv->headers == NULL)
        wv->headers = uc_headers_table_new ();
    uc_headers_table_set (wv->headers, normalized, names, values, (guint) count,
                          uc_site_wk_headers_next_seq (global.headers));
    g_free (normalized);
    publish_headers ();
    return 0;
}

int
uc_webview_set_user_agent (uc_webview *wv, int preset, const char *custom)
{
    const char *ua = NULL, *reason = NULL;

    CHECK_VIEW_VAL (wv, -1);
    if (!uc_site_user_agent (preset, custom, &ua, &reason)) {
        g_warning ("ucwebview: set_user_agent(%d) refused: %s", preset, reason);
        return -1;
    }
    if (ua == NULL)
        ua = global.default_ua;
    if (wv->own_settings == NULL) {
        if (g_strcmp0 (ua, global.default_ua) == 0)
            return 0;
        wv->own_settings = settings_new ();
        webkit_web_view_set_settings (wv->web_view, wv->own_settings);
    }
    webkit_settings_set_user_agent (wv->own_settings, ua);
    return 0;
}

int
uc_webview_global_set_cookies (const char *url, const uc_webview_cookie *cookies, size_t count, uc_webview_done_cb cb,
                               void *user)
{
    if (!global.initialised)
        return -1;
    return uc_site_wk_set_cookies (webkit_website_data_manager_get_cookie_manager (data_manager ()), url, cookies,
                                   count, cb, user, global.log_full_urls);
}

int
uc_webview_global_allow_certificate (const char *host, const char *sha256)
{
    const char *reason = NULL;

    if (global.tls == NULL)
        global.tls = uc_tls_exceptions_new ();
    if (uc_tls_exceptions_add (global.tls, host, sha256, &reason) != 0) {
        g_warning ("ucwebview: allow_certificate() refused: %s", reason);
        return -1;
    }
    return 0;
}

int
uc_webview_global_set_tls_errors_policy (int ignore)
{
    global.tls_ignore = ignore != 0;
    if (global.initialised)
        apply_tls_policy ();
    return 0;
}

int
uc_webview_global_set_color_scheme (int scheme)
{
    if (scheme != UC_WEBVIEW_COLOR_SCHEME_LIGHT && scheme != UC_WEBVIEW_COLOR_SCHEME_DARK)
        return -1;
    global.dark = scheme == UC_WEBVIEW_COLOR_SCHEME_DARK;
    if (global.initialised)
        apply_color_scheme ();
    return 0;
}

int
uc_webview_global_clear_data (unsigned types, uc_webview_done_cb cb, void *user)
{
    if (!global.initialised)
        return -1;
    return uc_site_wk_clear_data (data_manager (), types, cb, user);
}

/* ---- WPEPlatform only: not here ----------------------------------------------------------------------------- */

void uc_webview_resize (uc_webview *wv, int w, int h) { (void) wv; (void) w; (void) h; unsupported ("resize"); }
/* the page is hidden with its window (WebKitWebViewBase follows the toplevel's visibility) */
void
uc_webview_set_mapped (uc_webview *wv, int mapped)
{
    CHECK_VIEW (wv);
    if (mapped)
        gtk_widget_show (wv->window);
    else
        gtk_widget_hide (wv->window);
}

/* ---- device state for web pages (library 0.9.0), as in uc_webview.c, without its log lines ---------------- */

int
uc_webview_set_device_api (uc_webview *wv, int enabled)
{
    CHECK_VIEW_VAL (wv, -1);
    uc_device_wk_set_enabled (wv->device, enabled != 0);
    return 0;
}

int
uc_webview_set_device_state (uc_webview *wv, const char *json)
{
    const char *error = NULL;

    CHECK_VIEW_VAL (wv, -1);
    if (!uc_device_wk_set_state (wv->device, json, NULL, &error)) {
        g_warning ("ucwebview: set_device_state() refused: %s", error);
        return -1;
    }
    return 0;
}

int
uc_webview_device_event (uc_webview *wv, const char *type, int64_t slept_ms)
{
    UcDeviceEvent event;

    CHECK_VIEW_VAL (wv, -1);
    if (!uc_device_event_parse (type, &event) || (event == UC_DEVICE_EVENT_RESUME && slept_ms < 0))
        return -1;
    if (uc_device_wk_enabled (wv->device))
        uc_device_wk_event (wv->device, event, slept_ms);
    return 0;
}
void uc_webview_set_focused (uc_webview *wv, int focused) { (void) wv; (void) focused; }
void uc_webview_set_scale (uc_webview *wv, double scale) { (void) wv; (void) scale; }
void uc_webview_set_background_rgba (uc_webview *wv, float r, float g, float b, float a) { (void) wv; (void) r; (void) g; (void) b; (void) a; }
int uc_webview_set_magnification (uc_webview *wv, double s, double x, double y) { (void) wv; (void) s; (void) x; (void) y; unsupported ("magnification"); return -1; }
double uc_webview_get_magnification (uc_webview *wv) { CHECK_VIEW_VAL (wv, 0.0); return 1.0; }
void uc_webview_frame_rendered (uc_webview *wv, void *frame) { (void) wv; (void) frame; }
void uc_webview_frame_release (uc_webview *wv, void *frame) { (void) wv; (void) frame; }
void uc_webview_pointer_move (uc_webview *wv, double x, double y, unsigned m, uint32_t ms) { (void) wv; (void) x; (void) y; (void) m; (void) ms; unsupported ("pointer"); }
void uc_webview_scroll (uc_webview *wv, double dx, double dy, double x, double y, int p, uint32_t ms) { (void) wv; (void) dx; (void) dy; (void) x; (void) y; (void) p; (void) ms; unsupported ("scroll"); }

/* Input: the events do not reach WebKitGTK, but the presses end the dialog loop guard as in uc_webview.c (library
 * 0.7.2), which the --dialog-test's tap step checks. The text input calls never count here: no element takes them. */
void
uc_webview_touch (uc_webview *wv, int id, int phase, double x, double y, uint32_t ms)
{
    (void) id; (void) x; (void) y; (void) ms;
    CHECK_VIEW (wv);
    if (phase == UC_WEBVIEW_TOUCH_DOWN)
        uc_wk_requests_user_input (wv->requests);
    unsupported ("touch");
}

void
uc_webview_pointer_button (uc_webview *wv, int b, int p, double x, double y, unsigned m, uint32_t ms)
{
    (void) b; (void) x; (void) y; (void) m; (void) ms;
    CHECK_VIEW (wv);
    if (p)
        uc_wk_requests_user_input (wv->requests);
    unsupported ("pointer");
}

void
uc_webview_key (uc_webview *wv, uint32_t k, uint32_t c, int p, unsigned m, uint32_t ms)
{
    (void) k; (void) c; (void) m; (void) ms;
    CHECK_VIEW (wv);
    if (p)
        uc_wk_requests_user_input (wv->requests);
    unsupported ("keys");
}

void uc_webview_ime_commit (uc_webview *wv, const char *t) { (void) wv; (void) t; unsupported ("text input"); }
void uc_webview_ime_preedit (uc_webview *wv, const char *t, int c) { (void) wv; (void) t; (void) c; unsupported ("text input"); }
void uc_webview_ime_delete_surrounding (uc_webview *wv, int o, int n) { (void) wv; (void) o; (void) n; unsupported ("text input"); }
