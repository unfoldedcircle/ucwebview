/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * libucwebview public C API (uc_webview.h): WebKitWebView setup, frame
 * bookkeeping, event translation and the IME bridge. Everything runs on the UI
 * thread (the thread of uc_webview_global_init(), which iterates the default
 * GLib main context); see the header for the contract.
 *
 * WebKit setup mirrors launcher/main.c: the WebKitWebView is
 * created with g_object_new() and the construct-only "display" property
 * (WebKitWebView.cpp:1368-1370), a WebKitNetworkSession holds website data and
 * cache (or is ephemeral), WebKitSettings come from the environment.
 */
#include "uc_abi.h"
#include "uc_device_wk.h"
#include "uc_ext_protocol.h"
#include "uc_formats.h"
#include "uc_internal.h"
#include "uc_load_error.h"
#include "uc_magnify.h"
#include "uc_page_wk.h"
#include "uc_pdf_policy.h"
#include "uc_refresh.h"
#include "uc_rendered.h"
#include "uc_requests_wk.h"
#include "uc_site.h"
#include "uc_site_wk.h"
#include "uc_trace.h"
#include "uc_uri_policy.h"

#include <errno.h>
#include <string.h>

#define UC_MIN_SIZE 1
#define UC_MAX_SIZE 16384

/* uc_webview_when_rendered() (library 0.5.0): the library's own script world, which page scripts cannot see
 * (WebKit keeps a world's script objects and message handlers apart, UserMessageHandlersNamespace.cpp:81-101,
 * LocalDOMWindow.cpp:862-888), the message handler the probe reports through, how long a request may take, and how
 * long it waits after the rendering update for a frame (uc_rendered.h). The magnification scripts of 0.6.0
 * (uc_magnify.h) run in the same world, and since 0.9.0 the device API's start script (uc_ext_protocol.h: the web
 * process extension creates the world, so that it and the library's user content share it).
 * The timeout was 15 s up to 0.5.x. A zoom step of a large page took 11 to 15 s on a Remote 3 in its low power mode
 * (408 MHz, which the remote enters 10 s after the last input) and hit it twice, the Wikipedia article on Linux timed
 * out at every step there (device measurements). 60 s lets those renders finish; a host treats a timeout as "show the
 * next frame as it comes". */
#define UC_RENDERED_WORLD      UC_EXT_HOST_WORLD
#define UC_RENDERED_TIMEOUT_MS 60000
#define UC_RENDERED_GRACE_MS   100

G_STATIC_ASSERT (UC_RENDERED_DONE == UC_WEBVIEW_RENDERED_DONE);
G_STATIC_ASSERT (UC_RENDERED_TIMEOUT == UC_WEBVIEW_RENDERED_TIMEOUT);
G_STATIC_ASSERT (UC_RENDERED_CANCELLED == UC_WEBVIEW_RENDERED_CANCELLED);
G_STATIC_ASSERT (UC_PINCH_BEGIN == UC_WEBVIEW_PINCH_BEGIN);
G_STATIC_ASSERT (UC_PINCH_UPDATE == UC_WEBVIEW_PINCH_UPDATE);
G_STATIC_ASSERT (UC_PINCH_END == UC_WEBVIEW_PINCH_END);
G_STATIC_ASSERT (UC_PINCH_CANCEL == UC_WEBVIEW_PINCH_CANCEL);
G_STATIC_ASSERT (UC_MAGNIFY_MIN == UC_WEBVIEW_MAGNIFICATION_MIN);
G_STATIC_ASSERT (UC_MAGNIFY_MAX == UC_WEBVIEW_MAGNIFICATION_MAX);
G_STATIC_ASSERT (UC_SITE_USER_AGENT_DEFAULT == UC_WEBVIEW_USER_AGENT_DEFAULT);
G_STATIC_ASSERT (UC_SITE_USER_AGENT_MOBILE == UC_WEBVIEW_USER_AGENT_MOBILE);
G_STATIC_ASSERT (UC_SITE_USER_AGENT_DESKTOP == UC_WEBVIEW_USER_AGENT_DESKTOP);
G_STATIC_ASSERT (UC_SITE_USER_AGENT_CUSTOM == UC_WEBVIEW_USER_AGENT_CUSTOM);
G_STATIC_ASSERT (UC_SITE_DATA_CACHE == UC_WEBVIEW_DATA_CACHE);
G_STATIC_ASSERT (UC_SITE_DATA_ALL == UC_WEBVIEW_DATA_ALL);

/* The directory of the library's web process extension (origin headers, library 0.8.0), $(libdir)/ucwebview/... */
#ifndef UC_WEBVIEW_EXTENSION_DIR
#define UC_WEBVIEW_EXTENSION_DIR "/usr/lib/ucwebview/web-process-extensions"
#endif

/* ---- process state ----------------------------------------------------------- */

static void on_download_started (WebKitNetworkSession *session, WebKitDownload *download, gpointer user_data);

static struct {
    UcWpeDisplay *display;
    WebKitNetworkSession *session;
    WebKitSettings *settings;
    GThread *ui_thread;
    int n_views;
    gboolean allow_file_urls;   /* UC_WEBVIEW_ALLOW_FILE_URLS, read once by uc_webview_global_init() */
    gboolean log_full_urls;     /* UC_WEBVIEW_LOG_FULL_URLS: log lines keep query, fragment and payload of URIs */
    gboolean pdf_viewer;        /* UC_WEBVIEW_PDF_VIEWER: WebKit's PDF.js viewer stays on (uc_pdf_policy.h) */
    uint32_t drag_threshold;    /* px, set on the display by uc_webview_global_init(); 0 before */
    /* display refresh rate in mHz, 0 = not given (uc_refresh.h has the precedence) */
    int env_refresh_mhz;        /* UC_WEBVIEW_REFRESH_RATE, read once by uc_webview_global_init() */
    int host_refresh_mhz;       /* uc_webview_global_set_refresh_rate(), also before uc_webview_global_init() */
    /* site settings (library 0.8.0); the first three may be set before uc_webview_global_init() */
    UcTlsExceptions *tls;       /* uc_webview_global_allow_certificate(); kept for the life of the process */
    gboolean tls_ignore;        /* uc_webview_global_set_tls_errors_policy() */
    gboolean dark;              /* uc_webview_global_set_color_scheme() */
    UcSiteWkHeaders *headers;   /* the views' origin headers for the web process extension; NULL without it */
    gulong extensions_handler;  /* initialize-web-process-extensions of the default WebKitWebContext */
    char *default_ua;           /* WebKit's user agent + " UCRemote/<firmware version>" (library 0.8.2) */
    GPtrArray *views;           /* uc_webview*, in creation order */
} global;

typedef struct {
    uc_webview *owner;
    WPEBuffer *buffer;      /* ref held until uc_webview_frame_release() */
    GBytes *pixels;         /* SHM frames: keeps the mapping alive */
    gboolean rendered;          /* wpe_view_buffer_rendered() done */
    gboolean rendered_pending;  /* rendered requested from inside frame_ready(): finish from an idle */
    gboolean released_pending;  /* release requested from inside frame_ready(): finish from an idle */
    uc_webview_frame info;
} UcFrame;

struct uc_webview {
    uc_webview_callbacks cb;
    void *user;
    WebKitWebView *web_view;
    UcWpeView *view;            /* borrowed from web_view */
    WPEToplevel *toplevel;      /* borrowed from view */
    GList *frames;              /* UcFrame*, oldest first */
    UcPointerState pointer;
    UcKeyState keys;
    int width, height;
    double scale;
    gboolean in_render_buffer;  /* frame_ready() callback running: defer rendered/released to an idle */
    guint deferred_source;
    gboolean last_load_failed;
    /* load_failed_detail() (library 0.10.0): the GTlsCertificateFlags of a load-failed-with-tls-errors the library
     * did not accept, for the load-failed WebKit emits right after it with the same error (WebKitWebView.cpp:2793-2798) */
    unsigned tls_failure_errors;
    GPtrArray *rejected;        /* URIs the scheme allowlist refused, reported as FAILED from an idle */
    guint rejected_source;
    UcRendered *rendered;       /* uc_webview_when_rendered() requests */
    WebKitUserContentManager *content_manager;  /* borrowed from web_view: the rendered probe's message handler */
    gboolean freeing;           /* inside uc_webview_free(): new rendered requests are refused */
    /* pinch zoom (library 0.6.0) */
    UcGestureController *gesture;       /* a reference; NULL without the host's pinch callback */
    guint magnify_id;                   /* the last uc_magnify_script_before/after() request id */
    gboolean setting_magnification;     /* inside webkit_web_view_set_magnification() of the host's call */
    double magnification;               /* what the host knows: set by it or reported by magnification_changed() */
    guint magnification_source;         /* idle that reports WebKit's own change */
    /* requests of the page (library 0.7.0): dialogs, logins, new windows, refused responses (uc_requests_wk.h) */
    UcWkRequests *requests;
    /* site settings (library 0.8.0) */
    UcHeadersTable *headers;            /* uc_webview_set_origin_headers(); NULL until the first call */
    WebKitSettings *own_settings;       /* the view's own settings since uc_webview_set_user_agent(); NULL: shared */
    /* device state for web pages (library 0.9.0): the start script and the changes (uc_device_wk.h), sent through
     * the messages to the page's web process extension (uc_page_wk.h) */
    UcPageWk *page;
    UcDeviceWk *device;
};

/* uc_webview_free() while a callback is running would free the object under the caller; the contract forbids it,
 * and the check keeps the failure mode a warning instead of a crash. */
static int callback_depth;

static gboolean
on_ui_thread (const char *func)
{
    if (global.ui_thread != NULL && g_thread_self () != global.ui_thread) {
        g_critical ("ucwebview: %s() called from a thread other than the one that ran uc_webview_global_init()", func);
        return FALSE;
    }
    return TRUE;
}

#define CHECK_VIEW(wv) \
    do { \
        if ((wv) == NULL || (wv)->web_view == NULL) { \
            g_warning ("ucwebview: %s(): invalid web view", G_STRFUNC); \
            return; \
        } \
        if (!on_ui_thread (G_STRFUNC)) \
            return; \
    } while (0)

#define CHECK_VIEW_VAL(wv, val) \
    do { \
        if ((wv) == NULL || (wv)->web_view == NULL) { \
            g_warning ("ucwebview: %s(): invalid web view", G_STRFUNC); \
            return (val); \
        } \
        if (!on_ui_thread (G_STRFUNC)) \
            return (val); \
    } while (0)

#define CALLBACK(wv, name, ...) \
    do { \
        if ((wv)->cb.name != NULL) { \
            callback_depth++; \
            (wv)->cb.name ((wv), __VA_ARGS__, (wv)->user); \
            callback_depth--; \
        } \
    } while (0)

static gboolean
env_true (const char *name)
{
    return uc_env_value_true (g_getenv (name));
}

UC_EXPORT int
uc_webview_api_version (void)
{
    return UC_WEBVIEW_API_VERSION;
}

/* The rate after the precedence of uc_refresh.h, applied to the display's screen and vsync source. */
static int
apply_refresh_rate (UcRefreshSource *source)
{
    int mhz = uc_refresh_effective (global.env_refresh_mhz, global.host_refresh_mhz, source);
    if (global.display != NULL)
        uc_wpe_display_set_refresh_rate (global.display, mhz);
    return mhz;
}

UC_EXPORT int
uc_webview_global_set_refresh_rate (double hz)
{
    int mhz = uc_refresh_mhz_from_hz (hz);

    if (!on_ui_thread (G_STRFUNC))
        return -1;
    if (mhz == 0) {
        g_warning ("ucwebview: refresh rate %g Hz ignored, expected 20..240 Hz", hz);
        return -1;
    }
    if (mhz == global.host_refresh_mhz)
        return 0;
    global.host_refresh_mhz = mhz;
    if (global.display == NULL)
        return 0;   /* applied and logged by uc_webview_global_init() */
    if (global.env_refresh_mhz > 0) {
        g_message ("ucwebview: the host reports %.3f Hz, UC_WEBVIEW_REFRESH_RATE keeps %.3f Hz", mhz / 1000.0,
                   global.env_refresh_mhz / 1000.0);
        return 0;
    }
    apply_refresh_rate (NULL);
    g_message ("ucwebview: display refresh rate %.3f Hz (host)", mhz / 1000.0);
    return 0;
}

UC_EXPORT double
uc_webview_global_get_refresh_rate (void)
{
    return uc_refresh_effective (global.env_refresh_mhz, global.host_refresh_mhz, NULL) / 1000.0;
}

static const char *apply_pdf_viewer (WebKitSettings *settings, gboolean viewer);
static const char *apply_hidden_page_throttling (WebKitSettings *settings);

static WebKitNetworkSession *
create_session (const char *data_dir, const char *cache_dir)
{
    char *cache = NULL;
    WebKitNetworkSession *session;

    if (data_dir == NULL || *data_dir == '\0') {
        g_message ("ucwebview: ephemeral network session (no data directory)");
        return webkit_network_session_new_ephemeral ();
    }
    if (g_mkdir_with_parents (data_dir, 0700) != 0) {
        g_warning ("ucwebview: cannot create %s (%s), using an ephemeral session", data_dir, g_strerror (errno));
        return webkit_network_session_new_ephemeral ();
    }
    if (cache_dir == NULL || *cache_dir == '\0')
        cache = g_build_filename (data_dir, "cache", NULL);
    else
        cache = g_strdup (cache_dir);
    if (g_mkdir_with_parents (cache, 0700) != 0)
        g_warning ("ucwebview: cannot create cache directory %s (%s)", cache, g_strerror (errno));
    session = webkit_network_session_new (data_dir, cache);
    {
        /* Library 0.8.0: cookies persist too. A persistent session keeps local storage, IndexedDB and the HTTP cache
         * in its directories, but its cookie jar lives in memory until the application names a file
         * (WebKitCookieManager.cpp:179-207, "By default, cookie_manager doesn't store the cookies persistently";
         * WebsiteDataStore.h:649-650 starts with an empty path). SQLite like Epiphany; libsoup writes every change of
         * a cookie with an expiry time (SoupCookieJarDB) and reads the file when WebKit's network process creates
         * the session (NetworkSessionSoup.cpp:60-61, 76-93). Session cookies stay in memory. */
        char *cookies = g_build_filename (data_dir, "cookies.sqlite", NULL);
        /* 0600 before the network process opens it (library 0.8.3): the cookie values are session tokens in clear;
         * the 0700 directory protects them already, the file mode keeps them private if it is copied or the
         * directory's mode changes */
        if (!uc_site_private_file (cookies))
            g_warning ("ucwebview: cannot make %s private (%s)", cookies, g_strerror (errno));
        webkit_cookie_manager_set_persistent_storage (webkit_network_session_get_cookie_manager (session), cookies,
                                                      WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        g_message ("ucwebview: website data in %s, cache in %s, cookies in %s", data_dir, cache, cookies);
        g_free (cookies);
    }
    g_free (cache);
    return session;
}

/* The WebKitSettings every view starts with (one shared object) and a view gets as its own copy when the host gives
 * it a user agent (library 0.8.0): the same switches, from the environment read by uc_webview_global_init(). */
static WebKitSettings *
settings_new (const char **pdf_viewer_state, const char **throttling_state)
{
    WebKitSettings *settings = webkit_settings_new_with_settings (
        "enable-developer-extras", env_true ("UC_WEBVIEW_DEVELOPER_EXTRAS"),
        "enable-write-console-messages-to-stdout", env_true ("UC_WEBVIEW_CONSOLE_LOG"),
        NULL);
    const char *state = apply_pdf_viewer (settings, global.pdf_viewer);
    const char *throttling = apply_hidden_page_throttling (settings);

    if (pdf_viewer_state != NULL)
        *pdf_viewer_state = state;
    if (throttling_state != NULL)
        *throttling_state = throttling;
    if (global.default_ua != NULL)
        webkit_settings_set_user_agent (settings, global.default_ua);
    return settings;
}

/* The DEFAULT user agent (library 0.8.2): WebKit's own, which keeps its platform part (UserAgentGLib.cpp:79-122), and
 * the Remote with its firmware version, "UCRemote/<UC_OS_VERSION>" (uc_site.h). The release file is hidden in the
 * remote-ui sandbox (TemporaryFileSystem=/:ro), whose units load it as the environment (EnvironmentFile=/etc/uc-release),
 * so UC_OS_VERSION comes from there; a host outside the sandbox reads the file. Once per process. Not
 * webkit_settings_set_user_agent_with_application_details(): without a version it appends WebKit's 605.1.15
 * (UserAgentGLib.cpp:146-149). Returns the words for the log line. */
static const char *
setup_default_user_agent (WebKitSettings *settings)
{
    static char *version;
    static const char *source;
    static gboolean read;

    if (!read) {
        read = TRUE;
        version = uc_site_firmware_version (UC_SITE_RELEASE_FILE, g_getenv ("UC_OS_VERSION"), &source);
    }
    g_free (global.default_ua);
    /* NULL: WebKit's standard user agent (WebKitSettings.cpp:3313-3318), the base also after a second
     * uc_webview_global_init() */
    webkit_settings_set_user_agent (settings, NULL);
    global.default_ua = uc_site_default_user_agent (webkit_settings_get_user_agent (settings), version);
    webkit_settings_set_user_agent (settings, global.default_ua);
    return source;
}

/* WebKit's PDF.js viewer (uc_pdf_policy.h): off in the settings every view shares, unless UC_WEBVIEW_PDF_VIEWER asks
 * for it. Returns the words for the start line. */
static const char *
apply_pdf_viewer (WebKitSettings *settings, gboolean viewer)
{
    WebKitFeatureList *features = webkit_settings_get_all_features ();
    WebKitFeature *feature = webkit_feature_list_find (features, UC_PDF_VIEWER_FEATURE);
    const char *state;

    if (feature == NULL) {
        state = "no PDF viewer in this WebKit";     /* built with ENABLE_PDFJS=OFF */
    } else {
        webkit_settings_set_feature_enabled (settings, feature, viewer);
        state = viewer ? "PDF viewer on (UC_WEBVIEW_PDF_VIEWER)" : "PDF viewer off";
    }
    webkit_feature_list_unref (features);
    return state;
}

/* Hidden page throttling (library 0.9.0, uc_device.h): an unmapped view's page (uc_webview_set_mapped(0): the display
 * is off, the item invisible) fires its timers at most once a second, aligned (DOMTimer.cpp, Page::
 * updateDOMTimerAlignmentInterval; a timer only from its sixth run on, Document::domTimerAlignmentInterval()), and its
 * CSS animations stop (Page::hiddenPageCSSAnimationSuspensionStateChanged).
 * WebKit switches both on by default for GTK and Cocoa only (UnifiedWebPreferences.yaml), so up to 0.8.x a hidden page
 * on the Remote kept its timers at full rate. Every view, no host switch. Returns the words for the start line. */
static const char *
apply_hidden_page_throttling (WebKitSettings *settings)
{
    static const char *const ids[] = { UC_DEVICE_FEATURE_TIMER_THROTTLING, UC_DEVICE_FEATURE_ANIMATION_SUSPEND };
    WebKitFeatureList *features = webkit_settings_get_all_features ();
    guint found = 0;

    for (guint i = 0; i < G_N_ELEMENTS (ids); i++) {
        WebKitFeature *feature = webkit_feature_list_find (features, ids[i]);
        if (feature != NULL) {
            webkit_settings_set_feature_enabled (settings, feature, TRUE);
            found++;
        }
    }
    webkit_feature_list_unref (features);
    if (found == G_N_ELEMENTS (ids))
        return "hidden pages throttled";
    return found > 0 ? "hidden pages partly throttled (a feature is missing in this WebKit)"
                     : "no hidden page throttling in this WebKit";
}

/* ---- site settings of the process (library 0.8.0) ------------------------------------------------------------ */

/* WPE's dark-mode setting of the library's display. WebKit reads it from the primary display for the whole process
 * (SystemSettingsManagerProxyWPE.cpp:103-106, 206-216; the proxy is created with the first WebKitWebView,
 * WebKitWebView.cpp:984) and tells every page when it changes (WPEWebViewPlatform.cpp:142-145,
 * WebPageProxy.cpp:17431-17461): prefers-color-scheme follows. */
static void
apply_color_scheme (void)
{
    GError *error = NULL;

    if (!wpe_settings_set_boolean (wpe_display_get_settings (WPE_DISPLAY (global.display)), WPE_SETTING_DARK_MODE,
                                   global.dark, WPE_SETTINGS_SOURCE_APPLICATION, &error)) {
        g_warning ("ucwebview: cannot set the color scheme: %s", error != NULL ? error->message : "unknown error");
        g_clear_error (&error);
    }
}

/* The session's policy (WebKitNetworkSession.cpp:430-448): IGNORE accepts every certificate, FAIL lets WebKit emit
 * load-failed-with-tls-errors, where the exceptions apply (uc_site_wk_tls_error()). */
static void
apply_tls_policy (void)
{
    webkit_network_session_set_tls_errors_policy (global.session, global.tls_ignore ? WEBKIT_TLS_ERRORS_POLICY_IGNORE
                                                                                     : WEBKIT_TLS_ERRORS_POLICY_FAIL);
}

static void
allow_certificate (GTlsCertificate *certificate, const char *host, gpointer user_data)
{
    (void) user_data;
    webkit_network_session_allow_tls_certificate_for_host (global.session, certificate, host);
}

/* WebKitWebContext::initialize-web-process-extensions: before every web process starts
 * (WebKitWebContext.cpp:2017-2023), the origin headers as they are now go with it as its initialization data. */
static void
on_initialize_web_process_extensions (WebKitWebContext *context, gpointer user_data)
{
    (void) user_data;
    if (global.headers != NULL)
        webkit_web_context_set_web_process_extensions_initialization_user_data (
            context, uc_site_wk_headers_user_data (global.headers));
}

/* The library's web process extension (uc_extension.c): every web process loads the .so files of the directory
 * (WebProcessExtensionManager.cpp:38-49, 101-123). The views use the default WebKitWebContext
 * (WebKitWebView.cpp:957-958); the directory must be set before its first web process starts. Without the file
 * (a library installed without it) there are no origin headers and no device state for pages (0.9.0). Returns the
 * words for the start line. */
static const char *
setup_extension (void)
{
    WebKitWebContext *context = webkit_web_context_get_default ();
    char *path = g_build_filename (UC_WEBVIEW_EXTENSION_DIR, UC_SITE_WK_EXTENSION_NAME, NULL);
    gboolean found = g_file_test (path, G_FILE_TEST_IS_REGULAR);

    g_free (path);
    if (!found)
        return "no web process extension in " UC_WEBVIEW_EXTENSION_DIR " (no origin headers, no device state for pages)";
    webkit_web_context_set_web_process_extensions_directory (context, UC_WEBVIEW_EXTENSION_DIR);
    if (global.extensions_handler == 0)
        global.extensions_handler = g_signal_connect (context, "initialize-web-process-extensions",
                                                      G_CALLBACK (on_initialize_web_process_extensions), NULL);
    global.headers = uc_site_wk_headers_new (context);
    return "web process extension " UC_WEBVIEW_EXTENSION_DIR;
}

/* the views' origin header tables, in creation order, to the web processes */
static void
publish_headers (void)
{
    GPtrArray *tables;

    if (global.headers == NULL)
        return;
    tables = g_ptr_array_new ();
    for (guint i = 0; global.views != NULL && i < global.views->len; i++)
        g_ptr_array_add (tables, ((uc_webview *) g_ptr_array_index (global.views, i))->headers);
    uc_site_wk_headers_publish (global.headers, tables);
    g_ptr_array_unref (tables);
}

UC_EXPORT int
uc_webview_global_init (void *egl_display, const char *data_dir, const char *cache_dir)
{
    GError *error = NULL;
    UcWpeDisplay *display;
    const char *pdf_viewer_state, *throttling_state, *extension_state;

    if (global.display != NULL) {
        if (!on_ui_thread (G_STRFUNC))
            return -1;
        g_debug ("ucwebview: already initialised");
        return 0;
    }

    display = uc_wpe_display_new (egl_display);
    if (!wpe_display_connect (WPE_DISPLAY (display), &error)) {
        g_warning ("ucwebview: cannot connect the WPE display: %s", error != NULL ? error->message : "unknown error");
        g_clear_error (&error);
        g_object_unref (display);
        return -1;
    }
    /* WebKit's screen manager, settings proxy and pasteboard use the primary display (ScreenManagerWPE.cpp:43,
     * SystemSettingsManagerProxyWPE.cpp:207); the first constructed display is primary already
     * (WPEDisplay.cpp:125-129), this makes it explicit. */
    wpe_display_set_primary (WPE_DISPLAY (display));

    {
        /* Tap or scroll: WebKit's gesture controller reads WPE_SETTING_DRAG_THRESHOLD from the view's display on
         * every touch move (WPEGestureDetector.cpp:63-69, default 8 px, WPESettings.cpp:106) and gives up the tap
         * once the finger is farther than that from the press. The library's display uses 16 px, measured on the
         * remote's panel (uc_events.h); UC_WEBVIEW_DRAG_THRESHOLD=<px> overrides it. */
        const char *env = g_getenv ("UC_WEBVIEW_DRAG_THRESHOLD");
        gboolean env_invalid = FALSE;
        uint32_t threshold = uc_events_display_drag_threshold (env, &env_invalid);

        if (env_invalid)
            g_warning ("ucwebview: UC_WEBVIEW_DRAG_THRESHOLD must be 1..200 px, ignored");
        if (wpe_settings_set_uint32 (wpe_display_get_settings (WPE_DISPLAY (display)), WPE_SETTING_DRAG_THRESHOLD,
                                     threshold, WPE_SETTINGS_SOURCE_APPLICATION, &error)) {
            global.drag_threshold = threshold;
            g_message ("ucwebview: touch drag threshold %u px (%s)", threshold,
                       env != NULL && !env_invalid && *env != '\0' ? "UC_WEBVIEW_DRAG_THRESHOLD" : "default");
        } else {
            /* WebKit keeps its own default then */
            global.drag_threshold = 8;
            g_warning ("ucwebview: cannot set the drag threshold: %s", error != NULL ? error->message : "unknown error");
            g_clear_error (&error);
        }
    }

    global.display = display;
    global.ui_thread = g_thread_self ();
    /* file: URLs are blocked unless the process enables them (uc_uri_policy.h); read once, logged below */
    global.allow_file_urls = env_true ("UC_WEBVIEW_ALLOW_FILE_URLS");
    /* what the log may show of URIs and input (uc_uri_policy.h, uc_trace.h) */
    global.log_full_urls = env_true ("UC_WEBVIEW_LOG_FULL_URLS");
    uc_trace_set_enabled (env_true ("UC_WEBVIEW_TRACE"));
    {
        /* Frame pacing (uc_wpe_screen.c): UC_WEBVIEW_REFRESH_RATE wins over the host's value, read once here */
        const char *env = g_getenv ("UC_WEBVIEW_REFRESH_RATE");
        UcRefreshSource source;
        int mhz;

        global.env_refresh_mhz = uc_refresh_parse (env);
        if (env != NULL && *env != '\0' && global.env_refresh_mhz == 0)
            g_warning ("ucwebview: UC_WEBVIEW_REFRESH_RATE=%s ignored, expected a number of Hz in 20..240", env);
        mhz = apply_refresh_rate (&source);
        uc_wpe_display_set_vsync_trace (display, env_true ("UC_WEBVIEW_TRACE"));
        if (source == UC_REFRESH_SOURCE_ENV && global.host_refresh_mhz > 0)
            g_message ("ucwebview: display refresh rate %.3f Hz (UC_WEBVIEW_REFRESH_RATE, the host reports %.3f Hz)",
                       mhz / 1000.0, global.host_refresh_mhz / 1000.0);
        else if (source == UC_REFRESH_SOURCE_DEFAULT)
            g_message ("ucwebview: display refresh rate %.3f Hz (default until the host sets one)", mhz / 1000.0);
        else
            g_message ("ucwebview: display refresh rate %.3f Hz (%s)", mhz / 1000.0, uc_refresh_source_name (source));
    }
    global.session = create_session (data_dir, cache_dir);
    g_signal_connect (global.session, "download-started", G_CALLBACK (on_download_started), NULL);
    global.pdf_viewer = uc_pdf_policy_viewer_requested ();
    global.settings = settings_new (&pdf_viewer_state, &throttling_state);
    {
        const char *ua_source = setup_default_user_agent (global.settings);
        g_message ("ucwebview: default user agent \"%s\" (%s)", global.default_ua, ua_source);
    }
    /* site settings (library 0.8.0): what the host set before, the extension for origin headers */
    if (global.tls == NULL)
        global.tls = uc_tls_exceptions_new ();
    if (global.views == NULL)
        global.views = g_ptr_array_new ();
    apply_tls_policy ();
    apply_color_scheme ();
    extension_state = setup_extension ();
    g_message ("ucwebview %d: WPE WebKit %u.%u.%u, WPEPlatform %u.%u.%u, %s, %s%s%s, %s, %s", UC_WEBVIEW_API_VERSION,
               webkit_get_major_version (), webkit_get_minor_version (), webkit_get_micro_version (),
               wpe_platform_get_major_version (), wpe_platform_get_minor_version (), wpe_platform_get_micro_version (),
               egl_display != NULL ? "DMA-BUF frames on the host EGLDisplay" : "shared memory frames",
               global.allow_file_urls ? "file: URLs allowed (UC_WEBVIEW_ALLOW_FILE_URLS)" : "file: URLs blocked",
               uc_trace_enabled () ? ", input trace on (UC_WEBVIEW_TRACE)" : "",
               global.log_full_urls ? ", full URLs in the log (UC_WEBVIEW_LOG_FULL_URLS)" : "", pdf_viewer_state,
               throttling_state);
    g_message ("ucwebview: site settings: %s, TLS errors %s, %u host(s) with a certificate exception, %s color scheme",
               extension_state, global.tls_ignore ? "ignored" : "checked", uc_tls_exceptions_size (global.tls),
               global.dark ? "dark" : "light");
    return 0;
}

UC_EXPORT int
uc_webview_global_get_drag_threshold (void)
{
    if (global.drag_threshold != 0)
        return (int) global.drag_threshold;
    /* before uc_webview_global_init(): what it will set */
    return (int) uc_events_display_drag_threshold (g_getenv ("UC_WEBVIEW_DRAG_THRESHOLD"), NULL);
}

UC_EXPORT void
uc_webview_global_shutdown (void)
{
    if (global.display == NULL)
        return;
    if (!on_ui_thread (G_STRFUNC))
        return;
    if (global.n_views > 0) {
        g_warning ("ucwebview: shutdown with %d view(s) alive, leaking them", global.n_views);
        return;
    }
    g_clear_object (&global.settings);
    g_clear_pointer (&global.headers, uc_site_wk_headers_free);
    g_signal_handlers_disconnect_by_func (global.session, G_CALLBACK (on_download_started), NULL);
    g_clear_object (&global.session);
    g_clear_object (&global.display);
    global.ui_thread = NULL;
    global.drag_threshold = 0;      /* read again by the next uc_webview_global_init() */
    global.env_refresh_mhz = 0;     /* read again by the next uc_webview_global_init(); the host's value stays */
}

/* ---- WebKitWebView signals ----------------------------------------------------- */

static void schedule_magnification_report (uc_webview *wv);

static void
on_load_changed (WebKitWebView *web_view, WebKitLoadEvent event, gpointer user_data)
{
    uc_webview *wv = user_data;
    if (event == WEBKIT_LOAD_STARTED) {
        wv->last_load_failed = FALSE;
        wv->tls_failure_errors = 0;
    }
    /* a load after a termination has a web process again (uc_page_wk.h) */
    uc_page_wk_load_changed (wv->page, event);
    if (event == WEBKIT_LOAD_COMMITTED) {
        /* the document the rendered probes run in is gone: their requestAnimationFrame callbacks never come */
        if (wv->rendered != NULL)
            uc_rendered_cancel_all (wv->rendered, "navigation");
        /* a pinch over the old page means nothing on the new one */
        if (wv->gesture != NULL)
            uc_gesture_controller_abort (wv->gesture);
        /* WebKit resets its page scale to 1 at a main-frame commit without notify::magnification
         * (WebPageProxy.cpp:8824-8829); the web process' own reset and a history restore notify (WebPage.cpp:8233-8250,
         * WebLocalFrameLoaderClient.cpp:1594-1600) */
        schedule_magnification_report (wv);
    }
    /* requests end before the host hears of the load: dialogs of the old document at COMMITTED, authentication
     * requests WebKit gave up at STARTED/FINISHED (uc_requests_wk.h) */
    uc_wk_requests_load_changed (wv->requests, event);
    /* device state (0.9.0): a page from the back/forward cache has the state of its first load */
    if (event == WEBKIT_LOAD_FINISHED)
        uc_device_wk_load_finished (wv->device);
    CALLBACK (wv, load_changed, (int) event, webkit_web_view_get_uri (web_view));
}

static gboolean
on_load_failed (WebKitWebView *web_view, WebKitLoadEvent event, const char *failing_uri, GError *error, gpointer user_data)
{
    uc_webview *wv = user_data;
    gboolean refused;
    unsigned tls_errors = wv->tls_failure_errors;
    UcLoadFailure failure;

    (void) web_view; (void) event;
    /* the flags belong to this failure only (load-failed comes right after the TLS signal) */
    wv->tls_failure_errors = 0;
    /* WebKit finished its authentication request before this signal; ours end now. A response the library refused
     * (download, PDF, unknown type, uc_requests_wk.c) fails with FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE: no FAILED
     * for it since 0.7.0, the host had response_refused() (WebKit's own handler shows nothing for it either,
     * WebKitWebView.cpp:616-631) */
    refused = uc_wk_requests_load_failed (wv->requests, error);
    if (g_error_matches (error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        return FALSE; /* a navigation replaced by a newer one */
    if (refused)
        return FALSE; /* WebKit emits FINISHED next */
    wv->last_load_failed = TRUE;
    uc_load_failure_from_error (&failure, error, tls_errors);
    {
        /* the message is libsoup's, GIO's or WebKit's and may name a URI (libsoup 3.4.4's "Invalid URI “%s” in
         * Location response header", soup-session.c:1147, carries a redirect target with its path and query):
         * scrubbed like the URI itself; the host gets it as it is in load_failed_detail() */
        char *loggable = uc_uri_policy_loggable (failing_uri, global.log_full_urls);
        char *message = uc_uri_policy_scrub_text (failure.message, global.log_full_urls);
        g_message ("ucwebview: load failed: %s: %s (%s %d)", loggable, message,
                   *failure.domain != '\0' ? failure.domain : "no domain", failure.code);
        g_free (message);
        g_free (loggable);
    }
    /* library 0.10.0: the reason first, then FAILED, once per failure (uc_load_error.h) */
    CALLBACK (wv, load_failed_detail, failing_uri, failure.reason, failure.domain, failure.code, failure.tls_errors,
              failure.message);
    CALLBACK (wv, load_changed, UC_WEBVIEW_LOAD_FAILED, failing_uri);
    return FALSE; /* let WebKit show its error page and emit FINISHED */
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

/* The web process of the page's document is gone (library 0.7.5): WebKit keeps the input method state of its focused
 * element, without a focus_out, and replays it with the view's focus (uc_ime_state.h, "Focus reports") */
static void
ime_web_process_gone (uc_webview *wv)
{
    WPEInputMethodContext *ime = uc_wpe_view_get_ime (wv->view);
    if (UC_IS_WPE_IME (ime))
        uc_wpe_ime_web_process_gone (UC_WPE_IME (ime), webkit_web_view_get_input_method_context (wv->web_view));
}

/* WebKit notifies page-id only when the page moves to the web process of a navigation, the process swap at its
 * commit (WebPageProxy.cpp:1608-1616, WebKitWebView.cpp:578-581, 3260-3263), right before the commit's load-changed
 * (WebPageProxy.cpp:6305-6307). */
static void
on_page_id_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) object; (void) pspec;
    ime_web_process_gone (user_data);
}

static void
on_web_process_terminated (WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, gpointer user_data)
{
    uc_webview *wv = user_data;
    (void) web_view;
    g_warning ("ucwebview: web process terminated (reason %d)", (int) reason);
    /* no messages to the page until the next load (uc_page_wk.h) */
    uc_page_wk_web_process_terminated (wv->page);
    if (wv->rendered != NULL)
        uc_rendered_cancel_all (wv->rendered, "web process terminated");
    /* touches in flight never come back to the gesture controller: start afresh (a pinch ends with CANCEL) */
    if (wv->gesture != NULL)
        wpe_gesture_controller_cancel (WPE_GESTURE_CONTROLLER (wv->gesture));
    /* WebKit closes no pending dialog here (WebKitWebView.cpp:5405-5421): the library does, with request_cancelled()
     * before the termination report */
    uc_wk_requests_web_process_terminated (wv->requests);
    /* the focused element died with the process, and the process of the next load keeps the page id (no page-id
     * notify): ime focus out before the termination report */
    ime_web_process_gone (wv);
    CALLBACK (wv, web_process_terminated, (int) reason);
}

/* A main-frame load failed on its TLS certificate while the policy is FAIL (WebKitWebView.cpp:2783-2799,
 * WebKitNavigationClient.cpp:62-73): the host's certificate exceptions (library 0.8.0). TRUE: accepted, the library
 * loads the URI again and WebKit reports no failure. FALSE: WebKit emits load-failed next, synchronously and with the
 * same error (WebKitWebView.cpp:2793-2798), and on_load_failed() reports the flags kept here (library 0.10.0). */
static gboolean
on_load_failed_with_tls_errors (WebKitWebView *web_view, const char *failing_uri, GTlsCertificate *certificate,
                                GTlsCertificateFlags errors, gpointer user_data)
{
    uc_webview *wv = user_data;
    gboolean accepted = uc_site_wk_tls_error (web_view, failing_uri, certificate, errors, global.tls,
                                              allow_certificate, NULL, global.log_full_urls);

    /* a TLS failure without flags is still CERTIFICATE (its error is G_TLS_ERROR, uc_load_error.c) */
    wv->tls_failure_errors = accepted ? 0 : (unsigned) errors;
    return accepted;
}

/* ---- callbacks from the WPE subclasses ------------------------------------------ */

static UcFrame *
find_frame (uc_webview *wv, void *handle)
{
    for (GList *l = wv->frames; l != NULL; l = l->next)
        if (l->data == handle)
            return l->data;
    return NULL;
}

static void
frame_free (UcFrame *frame)
{
    g_clear_pointer (&frame->pixels, g_bytes_unref);
    g_clear_object (&frame->buffer);
    g_free (frame);
}

static void
frame_do_rendered (UcFrame *frame)
{
    if (frame->rendered)
        return;
    frame->rendered = TRUE;
    /* -> WPEView::buffer-rendered -> AcceleratedBackingStore::frameDone(): the web process may render the
     * next frame (AcceleratedBackingStore.cpp:71-74, 301-305). */
    wpe_view_buffer_rendered (WPE_VIEW (frame->owner->view), frame->buffer);
}

static void
frame_do_release (UcFrame *frame)
{
    uc_webview *wv = frame->owner;
    frame_do_rendered (frame);
    wv->frames = g_list_remove (wv->frames, frame);
    /* -> WPEView::buffer-released -> ReleaseBuffer to the web process (AcceleratedBackingStore.cpp:312-319).
     * Our ref goes afterwards; the EGLImage lives as long as the WPEBuffer (WPEBufferDMABuf.cpp:71-88). */
    wpe_view_buffer_released (WPE_VIEW (wv->view), frame->buffer);
    frame_free (frame);
}

static gboolean
deferred_frame_ops (gpointer user_data)
{
    uc_webview *wv = user_data;
    GList *l = wv->frames;

    wv->deferred_source = 0;
    while (l != NULL) {
        GList *next = l->next;
        UcFrame *frame = l->data;
        if (frame->released_pending)
            frame_do_release (frame);
        else if (frame->rendered_pending)
            frame_do_rendered (frame);
        l = next;
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_deferred (uc_webview *wv)
{
    if (wv->deferred_source == 0)
        wv->deferred_source = g_idle_add_full (G_PRIORITY_HIGH, deferred_frame_ops, wv, NULL);
}

gboolean
uc_webview_internal_render_buffer (uc_webview *wv, WPEBuffer *buffer, GError **error)
{
    UcFrame *frame = g_new0 (UcFrame, 1);
    char fourcc[5];

    frame->owner = wv;
    frame->buffer = g_object_ref (buffer);
    frame->info.width = wpe_buffer_get_width (buffer);
    frame->info.height = wpe_buffer_get_height (buffer);
    frame->info.handle = frame;

    if (WPE_IS_BUFFER_DMA_BUF (buffer)) {
        WPEBufferDMABuf *dmabuf = WPE_BUFFER_DMA_BUF (buffer);
        GError *import_error = NULL;

        frame->info.fourcc = wpe_buffer_dma_buf_get_format (dmabuf);
        frame->info.modifier = wpe_buffer_dma_buf_get_modifier (dmabuf);
        frame->info.n_planes = (int) wpe_buffer_dma_buf_get_n_planes (dmabuf);
        if (uc_wpe_display_get_egl (global.display) != NULL) {
            /* eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) on the host display with the per-plane fd/offset/pitch and,
             * when EGL_EXT_image_dma_buf_import_modifiers is present, MODIFIER_LO/HI (WPEBufferDMABuf.cpp:117-178);
             * no GL context needs to be current. Cached in the buffer, returned again for re-imports. */
            frame->info.egl_image = wpe_buffer_import_to_egl_image (buffer, &import_error);
            if (frame->info.egl_image == NULL) {
                g_warning ("ucwebview: cannot import %dx%d %s frame: %s", frame->info.width, frame->info.height,
                           uc_fourcc_to_string (frame->info.fourcc, fourcc),
                           import_error != NULL ? import_error->message : "unknown error");
                g_propagate_error (error, import_error);
                frame_free (frame);
                return FALSE;
            }
        } else {
            /* no host EGLDisplay: map the dma-buf through GBM (WPEBufferDMABuf.cpp import_to_pixels). The GBytes is
             * (transfer none) and owned by the buffer (WPEBuffer.cpp:282-291); frame_free() unrefs frame->pixels, so
             * take a reference as the shared-memory path does (without it every frame dropped the buffer's own
             * reference: a GLib-CRITICAL per frame and a crash in free() after the next process swap). */
            GBytes *pixels = wpe_buffer_import_to_pixels (buffer, error);
            if (pixels == NULL) {
                frame_free (frame);
                return FALSE;
            }
            frame->pixels = g_bytes_ref (pixels);
            frame->info.pixels = g_bytes_get_data (frame->pixels, NULL);
            frame->info.stride = (int) wpe_buffer_dma_buf_get_stride (dmabuf, 0);
        }
    } else if (WPE_IS_BUFFER_SHM (buffer)) {
        WPEBufferSHM *shm = WPE_BUFFER_SHM (buffer);
        frame->pixels = g_bytes_ref (wpe_buffer_shm_get_data (shm));
        frame->info.pixels = g_bytes_get_data (frame->pixels, NULL);
        frame->info.stride = (int) wpe_buffer_shm_get_stride (shm);
        frame->info.fourcc = UC_FOURCC_ARGB8888; /* WPE_PIXEL_FORMAT_ARGB8888 is the only WPEPixelFormat (WPEBufferSHM.h:48-50) */
        frame->info.modifier = UC_DRM_MOD_LINEAR;
        frame->info.n_planes = 1;
    } else {
        g_set_error (error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "unsupported buffer type %s", G_OBJECT_TYPE_NAME (buffer));
        frame_free (frame);
        return FALSE;
    }

    wv->frames = g_list_append (wv->frames, frame);
    /* rendered requests waiting for a frame end from an idle, after frame_ready (uc_rendered.h) */
    if (wv->rendered != NULL)
        uc_rendered_frame (wv->rendered);
    if (wv->cb.frame_ready == NULL) {
        /* no consumer (e.g. a test that only loads): keep WebKit rendering */
        frame->released_pending = TRUE;
        schedule_deferred (wv);
        return TRUE;
    }
    wv->in_render_buffer = TRUE;
    CALLBACK (wv, frame_ready, &frame->info);
    wv->in_render_buffer = FALSE;
    return TRUE;
}

void
uc_webview_internal_cursor (uc_webview *wv, const char *name)
{
    CALLBACK (wv, cursor_changed, name);
}

void
uc_webview_internal_ime_focus (uc_webview *wv, gboolean focused, unsigned hints, unsigned purpose, const char *surrounding, int cursor)
{
    uc_trace_ime_focus (wv, focused, hints, purpose, surrounding, cursor);
    CALLBACK (wv, ime_focus, focused ? 1 : 0, hints, purpose, surrounding, cursor);
}

void
uc_webview_internal_ime_cursor_area (uc_webview *wv, int x, int y, int w, int h)
{
    CALLBACK (wv, ime_cursor_area, x, y, w, h);
}

void
uc_webview_internal_ime_surrounding (uc_webview *wv, const char *text, int cursor, int anchor)
{
    uc_trace_ime_surrounding (wv, text, cursor, anchor);
    CALLBACK (wv, ime_surrounding, text, cursor, anchor);
}

/* ---- uc_webview_when_rendered() (library 0.5.0) -------------------------------- */

/* The probe (uc_rendered_probe_script()), run in the library's script world (UC_RENDERED_WORLD) by the web process
 * extension (UC_EXT_SCRIPT_MESSAGE, uc_page_wk.h).
 *  - It runs after every message the view sent to the page before it: both go over the web process connection to
 *    the same WebPage (WebKitWebView.cpp:5598-5627, WebPageProxy.cpp:6731-6733), the extension runs it inside its
 *    message (WebPageGLib.cpp:99-123), and a zoom rebuilds the style and lays out right in its own message
 *    (WebPage.cpp:2821-2836, LocalFrame.cpp:1085-1130).
 *  - With JavaScriptCore, not with webkit_web_view_evaluate_javascript() (up to 0.8.x): WebKit forces a user gesture
 *    on every script of its UI process API (WebKitWebView.cpp:4494-4502, ScriptController.cpp:654-663), which gave
 *    every page that a host waited for sticky user activation and armed its "leave this page?" question. Without a
 *    gesture the MessageChannel hop that ended the transient activation before the requestAnimationFrame callbacks is
 *    gone too.
 *  - Two requestAnimationFrame callbacks: the first runs at the start of the next rendering update, before its
 *    layout and paint (Page.cpp:2329-2337); the second at the start of the update after it, which WebKit starts only
 *    when the compositor has taken the first one (LayerTreeHost.cpp:141-153, 438-457; the compositor posts that
 *    before it swaps and sends the frame, ThreadedCompositor.cpp:585-607). Callbacks of an isolated world run like
 *    the page's (ScriptedAnimationController.cpp:177-190).
 *  - The id goes back through the message handler of this world only (UserMessageHandlersNamespace.cpp:81-101);
 *    the page's world has no window.webkit for it (LocalDOMWindow.cpp:862-888) and cannot fake the report. The
 *    handler and the extension share the world (uc_ext_protocol.h).
 * The script returns at once, so the message's GTask, which holds a reference on the WebKitWebView
 * (WebKitWebView.cpp:5611), ends at once too; a reply that waited for the callbacks would keep the view alive in a
 * document that was navigated away from, where they never come. */
/* The library's own request after a magnification change while the trace is on (uc_trace.h): its data is the scale
 * in thousandths, its result only a trace line. Never called. */
static void
magnification_rendered_marker (void)
{
}

/* the end of a request (uc_rendered.h): the trace line, then the host's callback */
static void
on_rendered_finished (UcRenderedFunc func, gpointer data, const UcRenderedResult *result, gpointer user_data)
{
    uc_webview *wv = user_data;
    uc_webview_rendered_cb cb = (uc_webview_rendered_cb) func;

    if (func == (UcRenderedFunc) magnification_rendered_marker) {
        uc_trace_magnification_rendered (wv, GPOINTER_TO_UINT (data) / 1000.0, result);
        return;
    }
    uc_trace_rendered (wv, result);
    callback_depth++;
    cb (wv, (int) result->status, data);
    callback_depth--;
}

/* WebKitUserContentManager::script-message-received::ucwebviewRendered (2022 API: the value is a JSCValue,
 * WebKitUserContentManager.cpp:429-431) */
static void
on_rendered_message (WebKitUserContentManager *manager, JSCValue *value, gpointer user_data)
{
    uc_webview *wv = user_data;
    double id;

    (void) manager;
    if (wv->rendered == NULL || !jsc_value_is_number (value))
        return;
    id = jsc_value_to_double (value);
    if (!(id >= 1.0 && id <= (double) G_MAXUINT))
        return;
    if (!uc_rendered_update_done (wv->rendered, (guint) id))
        g_debug ("ucwebview: rendered report %u for no pending request (timed out or cancelled)", (guint) id);
}

/* The extension's reply: the probe ran (it returns its id at once). Else it did not run (no page reachable: before
 * the view's first load or after its web process terminated, uc_page_wk.h; no extension; an exception): its request
 * ends as cancelled, from the main context. No error text is logged. */
static void
on_rendered_probe_done (GObject *object, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = uc_page_wk_send_finish (object, result, &error);
    uc_webview *wv = g_object_get_data (object, "ucwebview");   /* NULL once uc_webview_free() ran */
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

/* a script of the library for its world, through the extension (uc_ext_protocol.h); callback may be NULL */
static void
host_script (uc_webview *wv, const char *script, GAsyncReadyCallback callback, gpointer user_data)
{
    GVariant *params = g_variant_new (UC_EXT_SCRIPT_VARIANT_TYPE, script);

    if (callback != NULL)
        uc_page_wk_send_with_reply (wv->page, UC_EXT_SCRIPT_MESSAGE, params, callback, user_data);
    else
        uc_page_wk_send (wv->page, UC_EXT_SCRIPT_MESSAGE, params);
}

/* a request with the probe script; 0 = pending, -1 = refused (too many pending) */
static int
rendered_request (uc_webview *wv, UcRenderedFunc func, gpointer data)
{
    guint id = uc_rendered_add (wv->rendered, func, data);
    char *script;

    if (id == 0)
        return -1;
    script = uc_rendered_probe_script (id);
    host_script (wv, script, on_rendered_probe_done, GUINT_TO_POINTER (id));
    g_free (script);
    return 0;
}

UC_EXPORT int
uc_webview_when_rendered (uc_webview *wv, uc_webview_rendered_cb cb, void *user)
{
    if (wv == NULL || wv->web_view == NULL || wv->rendered == NULL) {
        g_warning ("ucwebview: %s(): invalid web view", G_STRFUNC);
        return -1;
    }
    if (!on_ui_thread (G_STRFUNC))
        return -1;
    if (cb == NULL) {
        g_warning ("ucwebview: %s(): no callback, refused", G_STRFUNC);
        return -1;
    }
    if (wv->freeing) {
        g_warning ("ucwebview: %s() while the view is freed, refused", G_STRFUNC);
        return -1;
    }
    if (rendered_request (wv, (UcRenderedFunc) cb, user) != 0) {
        g_warning ("ucwebview: %s(): %d requests pending, refused", G_STRFUNC, UC_RENDERED_MAX_PENDING);
        return -1;
    }
    return 0;
}

/* ---- requests (library 0.7.0) --------------------------------------------------- */

UC_EXPORT int
uc_webview_request_reply (uc_webview *wv, uint64_t request, int accept, const char *text)
{
    CHECK_VIEW_VAL (wv, -1);
    if (wv->requests == NULL)
        return -1;
    return uc_wk_requests_reply (wv->requests, request, accept, text);
}

UC_EXPORT int
uc_webview_request_auth (uc_webview *wv, uint64_t request, const char *user, const char *password)
{
    CHECK_VIEW_VAL (wv, -1);
    if (wv->requests == NULL)
        return -1;
    return uc_wk_requests_auth (wv->requests, request, user, password);
}

/* ---- site settings (library 0.8.0) ------------------------------------------------------------------------- */

UC_EXPORT int
uc_webview_set_origin_headers (uc_webview *wv, const char *origin, const char *const *names,
                               const char *const *values, size_t count)
{
    char *normalized;
    const char *reason = NULL;

    CHECK_VIEW_VAL (wv, -1);
    if (global.headers == NULL) {
        g_warning ("ucwebview: set_origin_headers() refused: the library's web process extension is not installed "
                   "(%s/%s)", UC_WEBVIEW_EXTENSION_DIR, UC_SITE_WK_EXTENSION_NAME);
        return -1;
    }
    normalized = uc_headers_origin (origin);
    if (normalized == NULL) {
        g_warning ("ucwebview: set_origin_headers() refused: the origin is not an http or https origin");
        return -1;
    }
    if (!uc_headers_list_valid (names, values, count, &reason)) {
        /* never the values: they are the secrets */
        g_warning ("ucwebview: set_origin_headers(%s) refused: %s", normalized, reason);
        g_free (normalized);
        return -1;
    }
    if (count == 0 && (wv->headers == NULL || uc_headers_table_lookup (wv->headers, normalized) == NULL)) {
        g_free (normalized);
        return 0;
    }
    if (wv->headers == NULL)
        wv->headers = uc_headers_table_new ();
    uc_headers_table_set (wv->headers, normalized, names, values, (guint) count,
                          uc_site_wk_headers_next_seq (global.headers));
    if (count == 0) {
        g_message ("ucwebview: origin headers for %s removed", normalized);
    } else {
        GString *list = g_string_new (NULL);
        for (size_t i = 0; i < count; i++)
            g_string_append_printf (list, "%s%s", i > 0 ? ", " : "", names[i]);
        g_message ("ucwebview: %" G_GSIZE_FORMAT " origin header(s) for %s: %s", (gsize) count, normalized, list->str);
        g_string_free (list, TRUE);
    }
    g_free (normalized);
    publish_headers ();
    return 0;
}

UC_EXPORT int
uc_webview_set_user_agent (uc_webview *wv, int preset, const char *custom)
{
    const char *ua = NULL, *reason = NULL;

    CHECK_VIEW_VAL (wv, -1);
    if (!uc_site_user_agent (preset, custom, &ua, &reason)) {
        g_warning ("ucwebview: set_user_agent(%d) refused: %s", preset, reason);
        return -1;
    }
    if (ua == NULL)
        ua = global.default_ua;     /* DEFAULT: WebKit's user agent with the Remote's token */
    if (wv->own_settings == NULL) {
        if (g_strcmp0 (ua, global.default_ua) == 0)
            return 0;   /* the shared settings have the default */
        /* The user agent lives in WebKitSettings, which all views share (one object per process). The view gets
         * its own copy: webkit_web_view_set_settings() applies the preferences and the custom user agent to the page
         * at once (WebKitWebView.cpp:815-834, 4104-4122). */
        wv->own_settings = settings_new (NULL, NULL);
        webkit_web_view_set_settings (wv->web_view, wv->own_settings);
    }
    webkit_settings_set_user_agent (wv->own_settings, ua);
    g_message ("ucwebview: user agent %s", uc_site_user_agent_name (preset));
    g_debug ("ucwebview: user agent %s", webkit_settings_get_user_agent (wv->own_settings));
    return 0;
}

UC_EXPORT int
uc_webview_global_set_cookies (const char *url, const uc_webview_cookie *cookies, size_t count, uc_webview_done_cb cb,
                               void *user)
{
    if (!on_ui_thread (G_STRFUNC))
        return -1;
    if (global.session == NULL) {
        g_warning ("ucwebview: %s() before uc_webview_global_init(), refused", G_STRFUNC);
        return -1;
    }
    return uc_site_wk_set_cookies (webkit_network_session_get_cookie_manager (global.session), url, cookies, count, cb,
                                   user, global.log_full_urls);
}

UC_EXPORT int
uc_webview_global_allow_certificate (const char *host, const char *sha256)
{
    const char *reason = NULL;
    char *normalized, *fingerprint;

    if (!on_ui_thread (G_STRFUNC))
        return -1;
    if (global.tls == NULL)
        global.tls = uc_tls_exceptions_new ();
    if (uc_tls_exceptions_add (global.tls, host, sha256, &reason) != 0) {
        g_warning ("ucwebview: allow_certificate() refused: %s", reason);
        return -1;
    }
    normalized = uc_site_host_normalize (host);
    fingerprint = uc_site_fingerprint_normalize (sha256);
    g_message ("ucwebview: certificate exception for %s: %s%s", normalized,
               fingerprint != NULL ? "the certificate with SHA-256 " : "any certificate",
               fingerprint != NULL ? fingerprint : "");
    g_free (normalized);
    g_free (fingerprint);
    return 0;
}

UC_EXPORT int
uc_webview_global_set_tls_errors_policy (int ignore)
{
    if (!on_ui_thread (G_STRFUNC))
        return -1;
    global.tls_ignore = ignore != 0;
    if (global.session != NULL)
        apply_tls_policy ();
    g_message ("ucwebview: TLS certificate errors %s", global.tls_ignore ? "ignored for every host" : "checked");
    return 0;
}

UC_EXPORT int
uc_webview_global_set_color_scheme (int scheme)
{
    if (!on_ui_thread (G_STRFUNC))
        return -1;
    if (scheme != UC_WEBVIEW_COLOR_SCHEME_LIGHT && scheme != UC_WEBVIEW_COLOR_SCHEME_DARK) {
        g_warning ("ucwebview: set_color_scheme(%d) refused: expected UC_WEBVIEW_COLOR_SCHEME_LIGHT or _DARK", scheme);
        return -1;
    }
    global.dark = scheme == UC_WEBVIEW_COLOR_SCHEME_DARK;
    if (global.display != NULL)
        apply_color_scheme ();
    g_message ("ucwebview: %s color scheme", global.dark ? "dark" : "light");
    return 0;
}

UC_EXPORT int
uc_webview_global_clear_data (unsigned types, uc_webview_done_cb cb, void *user)
{
    if (!on_ui_thread (G_STRFUNC))
        return -1;
    if (global.session == NULL) {
        g_warning ("ucwebview: %s() before uc_webview_global_init(), refused", G_STRFUNC);
        return -1;
    }
    return uc_site_wk_clear_data (webkit_network_session_get_website_data_manager (global.session), types, cb, user);
}

/* WebKitNetworkSession::download-started: <a download> becomes a download before any response policy
 * (WebPageProxy.cpp:5836-5844) and surfaces only here, for every view of the session; webkit_download_get_web_view()
 * names the view (WebKitDownload.cpp:674-682). Nothing is saved on the remote: cancelled, reported to its view. */
static void
on_download_started (WebKitNetworkSession *session, WebKitDownload *download, gpointer user_data)
{
    WebKitWebView *web_view = webkit_download_get_web_view (download);
    uc_webview *wv = web_view != NULL ? g_object_get_data (G_OBJECT (web_view), "ucwebview") : NULL;

    (void) session; (void) user_data;
    uc_wk_requests_download_started (wv != NULL ? wv->requests : NULL, download, global.log_full_urls);
}

/* ---- pinch zoom (library 0.6.0) ------------------------------------------------ */

/* UcPinchFunc of the view's gesture controller (uc_wpe_gesture.c): from WebKit's touch event replies, main context */
static void
on_pinch (UcPinchPhase phase, double scale, double x, double y, gpointer user_data)
{
    uc_webview *wv = user_data;

    uc_trace_pinch (wv, (int) phase, scale, x, y);
    CALLBACK (wv, pinch, (int) phase, scale, x, y);
}

static gboolean
report_magnification (gpointer user_data)
{
    uc_webview *wv = user_data;
    double magnification = webkit_web_view_get_magnification (wv->web_view);

    wv->magnification_source = 0;
    if (ABS (magnification - wv->magnification) < UC_MAGNIFY_EPSILON)
        return G_SOURCE_REMOVE;
    wv->magnification = magnification;
    uc_trace_magnification_changed (wv, magnification);
    CALLBACK (wv, magnification_changed, magnification);
    return G_SOURCE_REMOVE;
}

/* WebKit's value may have changed by itself: report it from the main context (callbacks never run inside a
 * uc_webview_* call), once per batch of changes */
static void
schedule_magnification_report (uc_webview *wv)
{
    if (wv->magnification_source == 0)
        wv->magnification_source = g_idle_add_full (G_PRIORITY_DEFAULT, report_magnification, wv, NULL);
}

/* notify::magnification: WebKit's page scale changed. Inside the host's own uc_webview_set_magnification() WebKit
 * notifies synchronously (WebPageProxy.cpp:6792-6797, WebKitWebView.cpp:3232-3235); every other change comes from
 * the web process: a reset at a commit or a history restore (WebLocalFrameLoaderClient.cpp:1594-1600). */
static void
on_magnification_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    uc_webview *wv = user_data;
    (void) object; (void) pspec;
    if (!wv->setting_magnification)
        schedule_magnification_report (wv);
}

/* the after script's result: the scale the page has now, -1 when it could not keep the focal point */
static void
on_magnify_script_done (GObject *object, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    GVariant *reply = uc_page_wk_send_finish (object, result, &error);
    guint id = GPOINTER_TO_UINT (user_data);
    gboolean ran = FALSE;
    double value = 0;

    if (reply != NULL && g_variant_is_of_type (reply, G_VARIANT_TYPE (UC_EXT_SCRIPT_REPLY_TYPE)))
        g_variant_get (reply, UC_EXT_SCRIPT_REPLY_TYPE, &ran, &value);
    g_clear_pointer (&reply, g_variant_unref);
    if (!ran) {
        g_debug ("ucwebview: magnification script %u did not run (%s %d)", id,
                 error != NULL ? g_quark_to_string (error->domain) : "exception", error != NULL ? error->code : 0);
        g_clear_error (&error);
        return;
    }
    if (value < 0)
        g_debug ("ucwebview: magnification %u: the page reported no visual viewport, the focal point moved", id);
}

UC_EXPORT int
uc_webview_set_magnification (uc_webview *wv, double scale, double x, double y)
{
    double current, zoom;
    char *before, *after;
    guint id;

    CHECK_VIEW_VAL (wv, -1);
    if (!uc_magnify_scale_valid (scale)) {
        g_warning ("ucwebview: magnification %g refused, expected %g..%g", scale, UC_MAGNIFY_MIN, UC_MAGNIFY_MAX);
        return -1;
    }
    uc_magnify_clamp_focal (&x, &y, wv->width, wv->height);
    current = webkit_web_view_get_magnification (wv->web_view);
    if (ABS (scale - current) < UC_MAGNIFY_EPSILON) {
        wv->magnification = current;
        return 0;
    }
    uc_trace_magnification (wv, current, scale, x, y);
    wv->magnification = scale;
    if (!uc_page_wk_reachable (wv->page)) {
        /* nothing loaded yet, or the web process is gone: no page to scroll, and a message would start a web process
         * for about:blank (uc_page_wk.h) */
        wv->setting_magnification = TRUE;
        webkit_web_view_set_magnification (wv->web_view, scale);
        wv->setting_magnification = FALSE;
        return 0;
    }
    /* A, M, B in this order over the web process connection (uc_magnify.h); A and B through the extension, which runs
     * them inside their messages, without a user gesture (since 0.9.0) */
    zoom = webkit_web_view_get_zoom_level (wv->web_view);
    if (++wv->magnify_id == 0)
        wv->magnify_id = 1;
    id = wv->magnify_id;
    before = uc_magnify_script_before (id);
    after = uc_magnify_script_after (id, x, y, zoom);
    host_script (wv, before, NULL, NULL);
    wv->setting_magnification = TRUE;
    webkit_web_view_set_magnification (wv->web_view, scale);
    wv->setting_magnification = FALSE;
    host_script (wv, after, on_magnify_script_done, GUINT_TO_POINTER (id));
    g_free (before);
    g_free (after);
    /* the render time for the trace: a request of the library's own after the change */
    if (uc_trace_enabled () && rendered_request (wv, (UcRenderedFunc) magnification_rendered_marker,
                                                 GUINT_TO_POINTER ((guint) (scale * 1000.0 + 0.5))) != 0)
        g_debug ("ucwebview: no render time for the magnification: too many rendered requests pending");
    return 0;
}

UC_EXPORT double
uc_webview_get_magnification (uc_webview *wv)
{
    CHECK_VIEW_VAL (wv, 0.0);
    return webkit_web_view_get_magnification (wv->web_view);
}

/* ---- device state for web pages (library 0.9.0) ----------------------------------------------------------------- */

UC_EXPORT int
uc_webview_set_device_api (uc_webview *wv, int enabled)
{
    CHECK_VIEW_VAL (wv, -1);
    if (!enabled == !uc_device_wk_enabled (wv->device))
        return 0;
    uc_device_wk_set_enabled (wv->device, enabled != 0);
    if (enabled) {
        char *state = uc_device_state_describe (uc_device_wk_state (wv->device));
        g_message ("ucwebview: device API on (%s): from their next load the top frame's pages get "
                   "navigator.getBattery(), window.deviceHost and the freeze and resume events%s", state,
                   global.headers != NULL ? "" : " (not here: no web process extension)");
        g_free (state);
    } else {
        g_message ("ucwebview: device API off");
    }
    return 0;
}

UC_EXPORT int
uc_webview_set_device_state (uc_webview *wv, const char *json)
{
    UcDeviceState before;
    GString *ignored;
    const char *error = NULL;

    CHECK_VIEW_VAL (wv, -1);
    before = *uc_device_wk_state (wv->device);
    ignored = g_string_new (NULL);
    if (!uc_device_wk_set_state (wv->device, json, ignored, &error)) {
        g_warning ("ucwebview: set_device_state() refused: %s", error);
        g_string_free (ignored, TRUE);
        return -1;
    }
    if (ignored->len > 0)
        g_warning ("ucwebview: set_device_state(): invalid %s ignored", ignored->str);
    g_string_free (ignored, TRUE);
    if (!uc_device_state_equal (&before, uc_device_wk_state (wv->device))) {
        char *state = uc_device_state_describe (uc_device_wk_state (wv->device));
        g_debug ("ucwebview: device state %s%s", state, uc_device_wk_enabled (wv->device) ? "" : " (device API off)");
        g_free (state);
    }
    return 0;
}

UC_EXPORT int
uc_webview_device_event (uc_webview *wv, const char *type, int64_t slept_ms)
{
    UcDeviceEvent event;

    CHECK_VIEW_VAL (wv, -1);
    if (!uc_device_event_parse (type, &event)) {
        g_warning ("ucwebview: device_event() refused: the type is not \"suspend\" or \"resume\"");
        return -1;
    }
    if (event == UC_DEVICE_EVENT_RESUME && slept_ms < 0) {
        g_warning ("ucwebview: device_event(\"resume\") refused: negative time slept");
        return -1;
    }
    if (!uc_device_wk_enabled (wv->device))
        return 0;
    uc_device_wk_event (wv->device, event, slept_ms);
    if (event == UC_DEVICE_EVENT_RESUME)
        g_message ("ucwebview: device event resume, slept %" G_GINT64_FORMAT " ms", (gint64) slept_ms);
    else
        g_message ("ucwebview: device event suspend");
    return 0;
}

/* ---- view lifecycle ------------------------------------------------------------ */

static gboolean
clamp_size (int *width, int *height)
{
    gboolean ok = TRUE;
    if (*width < UC_MIN_SIZE || *width > UC_MAX_SIZE) { *width = CLAMP (*width, UC_MIN_SIZE, UC_MAX_SIZE); ok = FALSE; }
    if (*height < UC_MIN_SIZE || *height > UC_MAX_SIZE) { *height = CLAMP (*height, UC_MIN_SIZE, UC_MAX_SIZE); ok = FALSE; }
    return ok;
}

static gboolean
can_create_view (const char *func)
{
    if (global.display == NULL) {
        g_warning ("ucwebview: %s() before uc_webview_global_init()", func);
        return FALSE;
    }
    return on_ui_thread (func);
}

/* callbacks: the library's own copy, made by uc_webview_new() or uc_webview_new_sized() */
static uc_webview *
view_new (int width, int height, const uc_webview_callbacks *callbacks, void *user)
{
    uc_webview *wv;
    WPEView *view;

    if (!clamp_size (&width, &height))
        g_warning ("ucwebview: view size clamped to %dx%d", width, height);

    wv = g_new0 (uc_webview, 1);
    wv->cb = *callbacks;
    wv->user = user;
    wv->width = width;
    wv->height = height;
    wv->scale = 1.0;
    uc_pointer_state_init (&wv->pointer);
    uc_key_state_init (&wv->keys);

    /* WebKit calls wpe_view_new(display) -> UcWpeDisplay::create_view, then attaches a toplevel from
     * create_toplevel (WPEDisplay.cpp:246-255); the display hands out the size set here. */
    uc_wpe_display_set_next_view (global.display, width, height, wv->scale);
    wv->web_view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                  "display", global.display,
                                                  "settings", global.settings,
                                                  "network-session", global.session,
                                                  NULL));
    view = webkit_web_view_get_wpe_view (wv->web_view);
    if (!UC_IS_WPE_VIEW (view)) {
        g_critical ("ucwebview: WebKit created a %s instead of a UcWpeView", view != NULL ? G_OBJECT_TYPE_NAME (view) : "(null)");
        g_object_unref (wv->web_view);
        g_free (wv);
        return NULL;
    }
    wv->view = UC_WPE_VIEW (view);
    uc_wpe_view_set_owner (wv->view, wv);
    wv->toplevel = wpe_view_get_toplevel (view);
    if (wv->toplevel == NULL) {
        /* WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL was switched off by someone: attach one ourselves */
        WPEToplevel *toplevel = uc_wpe_toplevel_new (global.display, width, height, wv->scale);
        wpe_view_set_toplevel (view, toplevel);
        g_object_unref (toplevel);
        wv->toplevel = wpe_view_get_toplevel (view);
    }
    /* wpe_toplevel_resized() in create_toplevel ran before the view joined, so push the size to the view now */
    wpe_view_resized (view, width, height);

    g_signal_connect (wv->web_view, "load-changed", G_CALLBACK (on_load_changed), wv);
    g_signal_connect (wv->web_view, "load-failed", G_CALLBACK (on_load_failed), wv);
    g_signal_connect (wv->web_view, "notify::title", G_CALLBACK (on_title_notify), wv);
    g_signal_connect (wv->web_view, "notify::estimated-load-progress", G_CALLBACK (on_progress_notify), wv);
    g_signal_connect (wv->web_view, "web-process-terminated", G_CALLBACK (on_web_process_terminated), wv);
    g_signal_connect (wv->web_view, "notify::page-id", G_CALLBACK (on_page_id_notify), wv);
    g_signal_connect (wv->web_view, "load-failed-with-tls-errors", G_CALLBACK (on_load_failed_with_tls_errors), wv);

    /* uc_webview_when_rendered(): WebKit gave the view a user content manager of its own (WebKitWebView.cpp:990-991);
     * the probe's handler lives in the library's world only. Registered before the first load, so every web process
     * of the view starts with it. */
    wv->rendered = uc_rendered_new (UC_RENDERED_TIMEOUT_MS, UC_RENDERED_GRACE_MS, on_rendered_finished, wv);
    wv->content_manager = webkit_web_view_get_user_content_manager (wv->web_view);
    if (!webkit_user_content_manager_register_script_message_handler (wv->content_manager, UC_RENDERED_HANDLER,
                                                                      UC_RENDERED_WORLD))
        g_warning ("ucwebview: cannot register the rendered probe's message handler: uc_webview_when_rendered() will "
                   "time out");
    g_signal_connect (wv->content_manager, "script-message-received::" UC_RENDERED_HANDLER,
                      G_CALLBACK (on_rendered_message), wv);
    g_object_set_data (G_OBJECT (wv->web_view), "ucwebview", wv);

    /* requests of the page (library 0.7.0): script dialogs, logins, new windows, window.close(), refused responses
     * (decide-policy, also the PDF rule of 0.6.1); the callbacks come from wv->cb, which lives as long as the view */
    {
        UcWkRequestsHost requests_host = { wv, &wv->cb, user, &callback_depth, global.log_full_urls, global.pdf_viewer };
        wv->requests = uc_wk_requests_new (wv->web_view, &requests_host);
    }

    /* device state for web pages (library 0.9.0): off until the host switches it on */
    wv->page = uc_page_wk_new (wv->web_view);
    wv->device = uc_device_wk_new (wv->web_view, wv->page);

    /* pinch zoom (library 0.6.0): the magnification as the host knows it, WebKit's own changes */
    wv->magnification = webkit_web_view_get_magnification (wv->web_view);
    g_signal_connect (wv->web_view, "notify::magnification", G_CALLBACK (on_magnification_notify), wv);
    if (wv->cb.pinch != NULL) {
        /* the host takes pinch reports: two fingers are the library's, one finger stays WebKit's (uc_pinch.h). The
         * pinch begins after the drag threshold, the distance WebKit's own controller needs before it scrolls. */
        wv->gesture = uc_gesture_controller_install (view, global.drag_threshold > 0 ? global.drag_threshold : 16.0,
                                                     on_pinch, wv);
        if (wv->gesture == NULL)
            g_warning ("ucwebview: the view has no gesture controller: no pinch reports");
    }

    /* Opaque white like WebKit's own default (WebKitWebViewWPE.cpp:226) and every browser: a page without a background
     * of its own (most plain documents) draws black text on it. Until 0.6.1 the library set black here, which left such
     * pages unreadable. */
    uc_webview_set_background_rgba (wv, 1, 1, 1, 1);
    wpe_view_map (view);       /* IsVisible: WebKit renders (WPEWebViewPlatform.cpp:80-83) */
    wpe_view_focus_in (view);   /* IsFocused: the IME filter may report editable focus (WPEWebViewPlatform.cpp:101-111) */

    global.n_views++;
    g_ptr_array_add (global.views, wv);
    g_debug ("ucwebview: view %p %dx%d created", (void *) wv, width, height);
    return wv;
}

UC_EXPORT uc_webview *
uc_webview_new (int width, int height, const uc_webview_callbacks *cb, void *user)
{
    uc_webview_callbacks callbacks;

    if (!can_create_view (G_STRFUNC))
        return NULL;
    /* The host's struct may come from an older header than ours: read the API version 1 callbacks only, never
     * sizeof (uc_webview_callbacks), which grows (uc_abi.h); later fields stay NULL. */
    uc_abi_callbacks_copy_v1 (&callbacks, cb);
    return view_new (width, height, &callbacks, user);
}

UC_EXPORT uc_webview *
uc_webview_new_sized (int width, int height, const uc_webview_callbacks *cb, size_t cb_size, void *user)
{
    static gboolean newer_host_logged;
    uc_webview_callbacks callbacks;

    if (!can_create_view (G_STRFUNC))
        return NULL;
    /* min(cb_size, sizeof callbacks) bytes of the host's struct, the rest NULL (uc_abi.h) */
    switch (uc_abi_callbacks_copy (&callbacks, cb, cb_size)) {
    case UC_ABI_COPY_TOO_SMALL:
        g_warning ("ucwebview: %s(): cb_size %" G_GSIZE_FORMAT " is smaller than the %" G_GSIZE_FORMAT " bytes of the "
                   "API version 1 callbacks, refused (pass sizeof (uc_webview_callbacks))", G_STRFUNC, (gsize) cb_size,
                   (gsize) UC_ABI_CALLBACKS_V1_SIZE);
        return NULL;
    case UC_ABI_COPY_LONGER:
        if (!newer_host_logged) {
            newer_host_logged = TRUE;
            g_message ("ucwebview: the host's callback struct has %" G_GSIZE_FORMAT " bytes, this library knows %"
                       G_GSIZE_FORMAT ": callbacks of the host's newer uc_webview.h are not called", (gsize) cb_size,
                       (gsize) sizeof callbacks);
        }
        break;
    case UC_ABI_COPY_OK:
        break;
    }
    return view_new (width, height, &callbacks, user);
}

UC_EXPORT void
uc_webview_free (uc_webview *wv)
{
    if (wv == NULL)
        return;
    if (!on_ui_thread (G_STRFUNC))
        return;
    if (callback_depth > 0) {
        g_critical ("ucwebview: uc_webview_free() called from inside a callback; leaking the view");
        return;
    }
    /* the pending rendered requests end here, inside the call (uc_webview.h threading contract); their callbacks
     * cannot add new ones */
    wv->freeing = TRUE;
    uc_rendered_free (wv->rendered, "view freed");
    wv->rendered = NULL;
    /* pending dialogs and logins end without a callback (uc_webview.h threading contract), before the web view goes */
    uc_wk_requests_free (wv->requests);
    wv->requests = NULL;
    if (wv->content_manager != NULL) {
        g_signal_handlers_disconnect_by_data (wv->content_manager, wv);
        webkit_user_content_manager_unregister_script_message_handler (wv->content_manager, UC_RENDERED_HANDLER,
                                                                       UC_RENDERED_WORLD);
        wv->content_manager = NULL;
    }
    g_object_set_data (G_OBJECT (wv->web_view), "ucwebview", NULL);
    /* the start script leaves the view's user content manager (0.9.0) */
    g_clear_pointer (&wv->device, uc_device_wk_free);
    g_clear_pointer (&wv->page, uc_page_wk_free);
    /* WebKit may still hand touch replies to the view's controller after this call (the web view can outlive it) */
    if (wv->gesture != NULL) {
        uc_gesture_controller_detach (wv->gesture);
        g_clear_object (&wv->gesture);
    }
    if (wv->magnification_source != 0)
        g_source_remove (wv->magnification_source);
    if (wv->deferred_source != 0)
        g_source_remove (wv->deferred_source);
    if (wv->rejected_source != 0)
        g_source_remove (wv->rejected_source);
    g_clear_pointer (&wv->rejected, g_ptr_array_unref);
    /* The host may not touch the frames' EGLImages after this call (uc_webview.h). */
    while (wv->frames != NULL)
        frame_do_release (wv->frames->data);
    memset (&wv->cb, 0, sizeof wv->cb);
    if (wv->view != NULL)
        uc_wpe_view_set_owner (wv->view, NULL);
    g_signal_handlers_disconnect_by_data (wv->web_view, wv);
    webkit_web_view_stop_loading (wv->web_view);
    g_clear_object (&wv->web_view);
    g_clear_object (&wv->own_settings);
    global.n_views--;
    g_ptr_array_remove (global.views, wv);
    /* the view's origin headers go; another view's for the same origin come back (uc_site_wk_headers_publish()) */
    if (wv->headers != NULL) {
        g_clear_pointer (&wv->headers, uc_headers_table_free);
        publish_headers ();
    }
    g_free (wv);
}

UC_EXPORT void
uc_webview_resize (uc_webview *wv, int width, int height)
{
    CHECK_VIEW (wv);
    if (!clamp_size (&width, &height))
        g_warning ("ucwebview: resize clamped to %dx%d", width, height);
    if (width == wv->width && height == wv->height)
        return;
    wv->width = width;
    wv->height = height;
    uc_wpe_display_set_next_view (global.display, width, height, wv->scale);
    /* UcWpeToplevel::resize -> wpe_toplevel_resized + wpe_view_resized -> WebKit setSize; the next frame has the new size */
    wpe_toplevel_resize (wv->toplevel, width, height);
}

UC_EXPORT void
uc_webview_set_mapped (uc_webview *wv, int mapped)
{
    CHECK_VIEW (wv);
    if (mapped)
        wpe_view_map (WPE_VIEW (wv->view));
    else
        wpe_view_unmap (WPE_VIEW (wv->view));
}

UC_EXPORT void
uc_webview_set_focused (uc_webview *wv, int focused)
{
    CHECK_VIEW (wv);
    if (focused)
        wpe_view_focus_in (WPE_VIEW (wv->view));
    else
        wpe_view_focus_out (WPE_VIEW (wv->view));
}

UC_EXPORT void
uc_webview_set_scale (uc_webview *wv, double scale)
{
    CHECK_VIEW (wv);
    if (!(scale > 0.0) || scale > 20.0) {
        g_warning ("ucwebview: invalid scale %g ignored", scale);
        return;
    }
    wv->scale = scale;
    /* -> WPEView notify::scale -> WebPageProxy::setIntrinsicDeviceScaleFactor (WPEWebViewPlatform.cpp:88-91) */
    wpe_toplevel_scale_changed (wv->toplevel, scale);
}

/* ---- navigation ------------------------------------------------------------------ */

static gboolean
report_rejected_loads (gpointer user_data)
{
    uc_webview *wv = user_data;
    GPtrArray *uris = wv->rejected;

    /* a callback may load again (and be rejected again): that queues a new batch and a new idle */
    wv->rejected = NULL;
    wv->rejected_source = 0;
    for (guint i = 0; i < uris->len; i++) {
        const char *uri = g_ptr_array_index (uris, i);
        UcLoadFailure failure;

        /* library 0.10.0: the reason first, then FAILED */
        uc_load_failure_scheme_refused (&failure);
        CALLBACK (wv, load_failed_detail, uri, failure.reason, failure.domain, failure.code, failure.tls_errors,
                  failure.message);
        CALLBACK (wv, load_changed, UC_WEBVIEW_LOAD_FAILED, uri);
    }
    g_ptr_array_unref (uris);
    return G_SOURCE_REMOVE;
}

/* The scheme allowlist (uc_uri_policy.h) in front of every API load. A rejected load leaves the current page
 * and a running load alone; the host hears UC_WEBVIEW_LOAD_FAILED (without STARTED/FINISHED) from an idle,
 * because callbacks never run inside a uc_webview_* call (uc_webview.h threading contract). */
static gboolean
load_allowed (uc_webview *wv, const char *what, const char *uri, UcUriVerdict verdict)
{
    char *loggable;

    if (verdict == UC_URI_ALLOWED)
        return TRUE;
    loggable = uc_uri_policy_loggable (uri, global.log_full_urls);
    g_warning ("ucwebview: %s %s rejected: %s", what, loggable, uc_uri_policy_describe (verdict));
    g_free (loggable);
    if (wv->rejected == NULL)
        wv->rejected = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (wv->rejected, g_strdup (uri));
    if (wv->rejected_source == 0)
        wv->rejected_source = g_idle_add_full (G_PRIORITY_DEFAULT, report_rejected_loads, wv, NULL);
    return FALSE;
}

UC_EXPORT void
uc_webview_load_uri (uc_webview *wv, const char *uri)
{
    CHECK_VIEW (wv);
    if (uri == NULL || *uri == '\0') {
        g_warning ("ucwebview: load_uri(NULL) ignored");
        return;
    }
    if (!load_allowed (wv, "load_uri", uri, uc_uri_policy_check (uri, global.allow_file_urls)))
        return;
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    webkit_web_view_load_uri (wv->web_view, uri);
}

UC_EXPORT void
uc_webview_load_html (uc_webview *wv, const char *html, const char *base_uri)
{
    CHECK_VIEW (wv);
    if (html == NULL) {
        g_warning ("ucwebview: load_html(NULL) ignored");
        return;
    }
    /* the base URI decides the document's origin and what relative URLs resolve to: same allowlist */
    if (!load_allowed (wv, "load_html base URI", base_uri,
                       uc_uri_policy_check_base (base_uri, global.allow_file_urls)))
        return;
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    webkit_web_view_load_html (wv->web_view, html, base_uri);
}

/* The navigation calls end the page's pending requests first (uc_wk_requests_host_navigation()): a dialog blocks the
 * page and the load would wait behind it. */
UC_EXPORT void
uc_webview_reload (uc_webview *wv)
{
    CHECK_VIEW (wv);
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    webkit_web_view_reload (wv->web_view);
}

UC_EXPORT void
uc_webview_stop (uc_webview *wv)
{
    CHECK_VIEW (wv);
    uc_wk_requests_host_navigation (wv->requests, FALSE);
    webkit_web_view_stop_loading (wv->web_view);
}

UC_EXPORT void
uc_webview_go_back (uc_webview *wv)
{
    CHECK_VIEW (wv);
    if (!webkit_web_view_can_go_back (wv->web_view))
        return;     /* nothing happens: the page's requests stay */
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    webkit_web_view_go_back (wv->web_view);
}

UC_EXPORT void
uc_webview_go_forward (uc_webview *wv)
{
    CHECK_VIEW (wv);
    if (!webkit_web_view_can_go_forward (wv->web_view))
        return;
    uc_wk_requests_host_navigation (wv->requests, TRUE);
    webkit_web_view_go_forward (wv->web_view);
}

UC_EXPORT int  uc_webview_can_go_back (uc_webview *wv) { CHECK_VIEW_VAL (wv, 0); return webkit_web_view_can_go_back (wv->web_view) ? 1 : 0; }
UC_EXPORT int  uc_webview_can_go_forward (uc_webview *wv) { CHECK_VIEW_VAL (wv, 0); return webkit_web_view_can_go_forward (wv->web_view) ? 1 : 0; }
UC_EXPORT const char *uc_webview_get_uri (uc_webview *wv)   { CHECK_VIEW_VAL (wv, NULL); return webkit_web_view_get_uri (wv->web_view); }
UC_EXPORT const char *uc_webview_get_title (uc_webview *wv) { CHECK_VIEW_VAL (wv, NULL); return webkit_web_view_get_title (wv->web_view); }

static void
on_javascript_done (GObject *object, GAsyncResult *result, gpointer user_data)
{
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (object), result, &error);
    (void) user_data;
    if (value == NULL) {
        /* an exception message of the page's script may hold the page's URLs */
        char *message = uc_uri_policy_scrub_text (error != NULL ? error->message : "unknown error",
                                                  global.log_full_urls);
        g_warning ("ucwebview: JavaScript failed: %s", message);
        g_free (message);
        g_clear_error (&error);
        return;
    }
    g_object_unref (value);
}

UC_EXPORT void
uc_webview_run_javascript (uc_webview *wv, const char *script)
{
    CHECK_VIEW (wv);
    if (script == NULL)
        return;
    webkit_web_view_evaluate_javascript (wv->web_view, script, -1, NULL, NULL, NULL, on_javascript_done, NULL);
}

UC_EXPORT void
uc_webview_set_zoom (uc_webview *wv, double zoom)
{
    CHECK_VIEW (wv);
    if (!(zoom > 0.0) || zoom > 20.0) {
        g_warning ("ucwebview: invalid zoom %g ignored", zoom);
        return;
    }
    webkit_web_view_set_zoom_level (wv->web_view, zoom);
}

UC_EXPORT void
uc_webview_set_background_rgba (uc_webview *wv, float r, float g, float b, float a)
{
    WebKitColor color;
    CHECK_VIEW (wv);
    color.red = CLAMP (r, 0.f, 1.f);
    color.green = CLAMP (g, 0.f, 1.f);
    color.blue = CLAMP (b, 0.f, 1.f);
    color.alpha = CLAMP (a, 0.f, 1.f);
    webkit_web_view_set_background_color (wv->web_view, &color);
}

/* ---- frames ------------------------------------------------------------------------- */

UC_EXPORT void
uc_webview_frame_rendered (uc_webview *wv, void *frame_handle)
{
    UcFrame *frame;
    CHECK_VIEW (wv);
    frame = find_frame (wv, frame_handle);
    if (frame == NULL) {
        g_warning ("ucwebview: frame_rendered(%p): unknown frame", frame_handle);
        return;
    }
    if (frame->rendered)
        return;
    if (wv->in_render_buffer) {
        /* called from inside frame_ready(): WebKit is still inside wpe_view_render_buffer()
         * (AcceleratedBackingStore.cpp:283-293), finish after it returned like the headless view does */
        frame->rendered_pending = TRUE;
        schedule_deferred (wv);
        return;
    }
    frame_do_rendered (frame);
}

UC_EXPORT void
uc_webview_frame_release (uc_webview *wv, void *frame_handle)
{
    UcFrame *frame;
    CHECK_VIEW (wv);
    frame = find_frame (wv, frame_handle);
    if (frame == NULL) {
        g_warning ("ucwebview: frame_release(%p): unknown frame", frame_handle);
        return;
    }
    if (wv->in_render_buffer) {
        frame->released_pending = TRUE;
        schedule_deferred (wv);
        return;
    }
    frame_do_release (frame);
}

/* ---- input -------------------------------------------------------------------------- */

static void
send_event (uc_webview *wv, WPEEvent *event)
{
    /* emits WPEView::event; WebKit's handler runs after ours (connect_after, WPEWebViewPlatform.cpp:113) */
    wpe_view_event (WPE_VIEW (wv->view), event);
    wpe_event_unref (event);
}

UC_EXPORT void
uc_webview_touch (uc_webview *wv, int id, int phase, double x, double y, uint32_t ms)
{
    int type;
    CHECK_VIEW (wv);
    type = uc_events_touch_phase_to_wpe (phase);
    if (type < 0 || id < 0) {
        g_warning ("ucwebview: touch(id %d, phase %d) ignored", id, phase);
        return;
    }
    /* the finger's press is the user's input in the page: it ends the dialog loop guard (uc_requests.h) */
    if (phase == UC_WEBVIEW_TOUCH_DOWN)
        uc_wk_requests_user_input (wv->requests);
    /* TOUCHSCREEN: WebKit feeds unhandled touches to its gesture controller (tap -> click, drag -> scroll,
     * WPEWebViewPlatform.cpp:433-495, PageClientImpl.cpp:265). */
    send_event (wv, wpe_event_touch_new ((WPEEventType) type, WPE_VIEW (wv->view), WPE_INPUT_SOURCE_TOUCHSCREEN,
                                         uc_events_time (ms), (WPEModifiers) uc_key_state_mods (&wv->keys),
                                         (guint32) id, x, y));
}

UC_EXPORT void
uc_webview_pointer_button (uc_webview *wv, int button, int pressed, double x, double y, unsigned mods, uint32_t ms)
{
    unsigned event_mods;
    guint press_count = 0;
    uint32_t time;
    CHECK_VIEW (wv);
    if (!uc_events_button_valid (button)) {
        g_warning ("ucwebview: pointer_button(%d) ignored", button);
        return;
    }
    time = uc_events_time (ms);
    event_mods = uc_pointer_state_button (&wv->pointer, button, pressed, uc_events_sanitize_mods (mods));
    if (pressed) {
        uc_wk_requests_user_input (wv->requests);     /* ends the dialog loop guard */
        /* double/triple click detection (WPEView.cpp:1045-1062, WPEViewQtQuick.cpp:447) */
        press_count = wpe_view_compute_press_count (WPE_VIEW (wv->view), x, y, (guint) button, time);
    }
    send_event (wv, wpe_event_pointer_button_new (pressed ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
                                                  WPE_VIEW (wv->view), WPE_INPUT_SOURCE_MOUSE, time,
                                                  (WPEModifiers) event_mods, (guint) button, x, y, press_count));
}

UC_EXPORT void
uc_webview_pointer_move (uc_webview *wv, double x, double y, unsigned mods, uint32_t ms)
{
    double dx, dy;
    CHECK_VIEW (wv);
    uc_pointer_state_move (&wv->pointer, x, y, &dx, &dy);
    send_event (wv, wpe_event_pointer_move_new (WPE_EVENT_POINTER_MOVE, WPE_VIEW (wv->view), WPE_INPUT_SOURCE_MOUSE,
                                                uc_events_time (ms),
                                                (WPEModifiers) uc_pointer_state_mods (&wv->pointer, uc_events_sanitize_mods (mods)),
                                                x, y, dx, dy));
}

UC_EXPORT void
uc_webview_scroll (uc_webview *wv, double dx, double dy, double x, double y, int precise, uint32_t ms)
{
    CHECK_VIEW (wv);
    /* precise: pixel deltas (x2.5 for a mouse source, WebEventFactoryWPE.cpp createWebWheelEvent); else wheel
     * ticks scaled by Scrollbar::pixelsPerLineStep */
    send_event (wv, wpe_event_scroll_new (WPE_VIEW (wv->view), WPE_INPUT_SOURCE_MOUSE, uc_events_time (ms),
                                          (WPEModifiers) uc_pointer_state_mods (&wv->pointer, uc_key_state_mods (&wv->keys)),
                                          dx, dy, precise ? TRUE : FALSE, FALSE, x, y));
}

UC_EXPORT void
uc_webview_key (uc_webview *wv, uint32_t xkb_keysym, uint32_t keycode, int pressed, unsigned mods, uint32_t ms)
{
    unsigned event_mods;
    CHECK_VIEW (wv);
    uc_trace_key (wv, xkb_keysym, keycode, pressed ? TRUE : FALSE);
    if (pressed)
        uc_wk_requests_user_input (wv->requests);     /* ends the dialog loop guard */
    event_mods = uc_key_state_update (&wv->keys, xkb_keysym, pressed ? TRUE : FALSE, uc_events_sanitize_mods (mods));
    uc_wpe_display_set_key_mods (global.display, event_mods);
    /* keyval is the XKB keysym: WebKit derives key, text and the Windows key code from it
     * (WebKeyboardEventWPE.cpp keyValueStringForWPEKeyval/windowsKeyCodeForWPEKeyval), keycode feeds
     * KeyboardEvent.code (keyCodeStringForWPEKeycode) and auto-repeat detection (2.54.2 WPEWebViewPlatform.cpp:438),
     * and, with the firmware's WebKit patch (key values for ChannelUp, ChannelDown, MediaPlayPause and Exit), the key
     * and keyCode of MediaPlayPause and Exit (README "Key names"). */
    send_event (wv, wpe_event_keyboard_new (pressed ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
                                            WPE_VIEW (wv->view), WPE_INPUT_SOURCE_KEYBOARD, uc_events_time (ms),
                                            (WPEModifiers) event_mods, (guint) keycode, (guint) xkb_keysym));
}

/* ---- IME ------------------------------------------------------------------------------ */

static UcWpeIme *
focused_ime (uc_webview *wv, const char *func)
{
    WPEInputMethodContext *ime = uc_wpe_view_get_ime (wv->view);
    if (!UC_IS_WPE_IME (ime) || !uc_wpe_ime_is_focused (UC_WPE_IME (ime))) {
        g_debug ("ucwebview: %s() without a focused editable element, ignored", func);
        return NULL;
    }
    return UC_WPE_IME (ime);
}

UC_EXPORT void
uc_webview_ime_commit (uc_webview *wv, const char *utf8)
{
    UcWpeIme *ime;
    CHECK_VIEW (wv);
    if (utf8 == NULL)
        return;
    ime = focused_ime (wv, G_STRFUNC);
    uc_trace_ime_commit (wv, utf8, ime != NULL);
    if (ime == NULL)
        return;
    if (!uc_wpe_ime_commit (ime, utf8))
        g_warning ("ucwebview: ime_commit: invalid UTF-8 ignored");
    else
        uc_wk_requests_user_input (wv->requests);     /* a key of the keyboard: ends the dialog loop guard */
}

UC_EXPORT void
uc_webview_ime_preedit (uc_webview *wv, const char *utf8, int cursor)
{
    UcWpeIme *ime;
    CHECK_VIEW (wv);
    ime = focused_ime (wv, G_STRFUNC);
    uc_trace_ime_preedit (wv, utf8, cursor, ime != NULL);
    if (ime == NULL)
        return;
    if (!uc_wpe_ime_preedit (ime, utf8, cursor))
        g_warning ("ucwebview: ime_preedit: invalid UTF-8 ignored");
    else if (utf8 != NULL && *utf8 != '\0')
        uc_wk_requests_user_input (wv->requests);     /* a key typed into the pre-edit (its end is no input) */
}

UC_EXPORT void
uc_webview_ime_delete_surrounding (uc_webview *wv, int offset, int n_chars)
{
    UcWpeIme *ime;
    CHECK_VIEW (wv);
    if (n_chars <= 0)
        return;
    ime = focused_ime (wv, G_STRFUNC);
    uc_trace_ime_delete_surrounding (wv, offset, n_chars, ime != NULL);
    if (ime == NULL)
        return;
    uc_wpe_ime_delete_surrounding (ime, offset, n_chars);
    uc_wk_requests_user_input (wv->requests);         /* the keyboard's backspace: ends the dialog loop guard */
}
