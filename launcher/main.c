/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * wpe-launcher: fullscreen WPE WebKit kiosk browser on a WPEPlatform display.
 *
 * The display is chosen by the WPE_PLATFORM environment variable (the systemd
 * unit sets WPE_PLATFORM=drm; `--headless` sets WPE_PLATFORM=headless).  The
 * structure follows the WPEPlatform browser tutorial
 * (Source/WebKit/WPEPlatform/docs/tutorial-browser.md) and
 * Tools/MiniBrowser/wpe/main.cpp of WPE WebKit 2.54.1:
 *
 *  - wpe_display_get_default() returns a connected, borrowed display
 *    (WPEDisplay.cpp:283-320); it aborts with g_error() if WPE_PLATFORM
 *    names a module that is missing or fails to connect (WPEDisplay.cpp:302-311).
 *  - The WebKitWebView is created with g_object_new() and the construct-only
 *    "display" property (WebKitWebView.cpp:1370); there is no
 *    webkit_web_view_new() taking a WPEDisplay in the wpe-webkit-2.0 API.
 *  - WPEPlatform delivers events on the default GLib main context, so
 *    everything here runs on the main thread (WPEDRMSeat.cpp:128).
 *
 * Everything in this file runs on the main thread.
 */
#include "kiosk-dbus.h"
#include "launcher-config.h"

#include <errno.h>
#include <glib-unix.h>
#include <glib.h>
#include <gio/gio.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#define RELOAD_BACKOFF_MAX_SEC 30

typedef struct {
    LauncherConfig     config;
    GMainLoop         *loop;
    WebKitWebView     *web_view;
    KioskDBus         *dbus;
    char              *requested_uri;    /* what we asked for last (reload target) */
    guint              reload_source;    /* pending g_timeout for a reload, 0 if none */
    guint              reload_delay_sec; /* back-off, 0 = next reload after 1 s */
    guint              reload_attempts;
    guint              timeout_source;   /* --headless watchdog */
    gboolean           last_load_failed;
    gboolean           quitting;
    int                exit_code;
} Launcher;

/* ---- lifecycle ---------------------------------------------------------- */

static void
launcher_quit (Launcher *launcher, int exit_code)
{
    if (launcher->quitting)
        return;
    launcher->quitting = TRUE;
    launcher->exit_code = exit_code;
    g_main_loop_quit (launcher->loop);
}

static gboolean
on_unix_signal (gpointer user_data)
{
    Launcher *launcher = user_data;
    g_message ("terminating on signal");
    launcher_quit (launcher, 0);
    return G_SOURCE_REMOVE;
}

static gboolean
on_headless_timeout (gpointer user_data)
{
    Launcher *launcher = user_data;
    launcher->timeout_source = 0;
    g_warning ("headless: %s did not finish loading within %u s", launcher->requested_uri, launcher->config.timeout_sec);
    launcher_quit (launcher, 1);
    return G_SOURCE_REMOVE;
}

/* ---- navigation --------------------------------------------------------- */

static void
cancel_pending_reload (Launcher *launcher)
{
    if (launcher->reload_source != 0) {
        g_source_remove (launcher->reload_source);
        launcher->reload_source = 0;
    }
}

/* Loads @uri; bare paths and relative file names become file:// URIs like MiniBrowser does (main.cpp:720). */
static void
load_requested_uri (Launcher *launcher, const char *uri)
{
    char *normalized;

    if (g_uri_peek_scheme (uri) != NULL) {
        normalized = g_strdup (uri);
    } else {
        GFile *file = g_file_new_for_commandline_arg (uri);
        normalized = g_file_get_uri (file);
        g_object_unref (file);
    }

    cancel_pending_reload (launcher);
    g_free (launcher->requested_uri);
    launcher->requested_uri = normalized;
    g_message ("loading %s", normalized);
    webkit_web_view_load_uri (launcher->web_view, normalized);
}

static gboolean
on_reload_timeout (gpointer user_data)
{
    Launcher *launcher = user_data;
    launcher->reload_source = 0;
    launcher->reload_attempts++;
    g_message ("reloading %s (attempt %u)", launcher->requested_uri, launcher->reload_attempts);
    webkit_web_view_load_uri (launcher->web_view, launcher->requested_uri);
    return G_SOURCE_REMOVE;
}

/* Exponential back-off 1, 2, 4, ... RELOAD_BACKOFF_MAX_SEC seconds, reset by a successful load. */
static void
schedule_reload (Launcher *launcher, const char *why)
{
    guint delay;

    if (launcher->quitting || launcher->reload_source != 0)
        return;
    delay = launcher->reload_delay_sec != 0 ? launcher->reload_delay_sec : 1;
    launcher->reload_delay_sec = MIN (delay * 2, RELOAD_BACKOFF_MAX_SEC);
    g_message ("scheduling a reload in %u s after %s", delay, why);
    launcher->reload_source = g_timeout_add_seconds (delay, on_reload_timeout, launcher);
}

/* ---- WebKitWebView signals (WebKitWebView.cpp:1927, 1957, 2521) --------- */

static void
on_load_changed (WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data)
{
    Launcher *launcher = user_data;
    const char *uri = webkit_web_view_get_uri (web_view);

    switch (load_event) {
    case WEBKIT_LOAD_STARTED:
        launcher->last_load_failed = FALSE;
        g_message ("load started: %s", uri);
        break;
    case WEBKIT_LOAD_REDIRECTED:
        g_message ("load redirected: %s", uri);
        break;
    case WEBKIT_LOAD_COMMITTED:
        g_message ("load committed: %s", uri);
        break;
    case WEBKIT_LOAD_FINISHED:
        /* WebKit emits FINISHED after a failed load as well; only a real success resets the back-off. */
        if (launcher->last_load_failed)
            break;
        g_message ("load finished: %s", uri);
        launcher->reload_delay_sec = 0;
        launcher->reload_attempts = 0;
        if (launcher->config.headless)
            launcher_quit (launcher, 0);
        break;
    }
}

static gboolean
on_load_failed (WebKitWebView *web_view, WebKitLoadEvent load_event, const char *failing_uri, GError *error, gpointer user_data)
{
    Launcher *launcher = user_data;

    /* A navigation replaced by another one (e.g. D-Bus Open() during a load) is not a failure. */
    if (g_error_matches (error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
        g_debug ("load cancelled: %s", failing_uri);
        return FALSE;
    }

    launcher->last_load_failed = TRUE;
    g_warning ("load failed: %s: %s (%s %d)", failing_uri, error->message, g_quark_to_string (error->domain), error->code);
    if (launcher->config.headless) {
        launcher_quit (launcher, 1);
        return TRUE;
    }
    /* Kiosk mode: the local web server may still be starting, keep trying. */
    schedule_reload (launcher, "a load failure");
    return FALSE;
}

static void
on_web_process_terminated (WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, gpointer user_data)
{
    Launcher *launcher = user_data;
    const char *why;

    switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED:               why = "crashed"; break;
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT: why = "exceeded the memory limit"; break;
    case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:     why = "was terminated by the API"; break;
    default:                                       why = "terminated for an unknown reason"; break;
    }
    g_warning ("web process %s while showing %s", why, launcher->requested_uri);
    if (launcher->config.headless) {
        launcher_quit (launcher, 1);
        return;
    }
    schedule_reload (launcher, "the web process termination");
}

static gboolean
on_close (WebKitWebView *web_view, gpointer user_data)
{
    Launcher *launcher = user_data;
    /* window.close() in kiosk mode: go back to the start page instead of showing nothing. */
    g_message ("page requested close");
    if (launcher->config.headless)
        launcher_quit (launcher, 1);
    else
        load_requested_uri (launcher, launcher->config.uri);
    return TRUE;
}

/* ---- WPEPlatform signals -------------------------------------------------- */

static void
on_display_disconnected (WPEDisplay *display, GError *error, gpointer user_data)
{
    Launcher *launcher = user_data;
    /* MiniBrowser _exit(1)s here (main.cpp:283-291); systemd restarts the service. */
    g_warning ("WPE display disconnected: %s", error != NULL ? error->message : "no error information");
    launcher_quit (launcher, 1);
}

static void
on_view_closed (WPEView *view, gpointer user_data)
{
    Launcher *launcher = user_data;
    g_message ("WPE view closed");
    launcher_quit (launcher, 0);
}

/* ---- D-Bus handlers ------------------------------------------------------- */

static void
dbus_open (const char *uri, gpointer user_data)
{
    Launcher *launcher = user_data;
    load_requested_uri (launcher, uri);
}

static void
dbus_reload (gpointer user_data)
{
    Launcher *launcher = user_data;
    cancel_pending_reload (launcher);
    webkit_web_view_reload (launcher->web_view);
}

static char *
dbus_get_uri (gpointer user_data)
{
    Launcher *launcher = user_data;
    return g_strdup (webkit_web_view_get_uri (launcher->web_view));
}

static void
dbus_quit (gpointer user_data)
{
    Launcher *launcher = user_data;
    launcher_quit (launcher, 0);
}

/* ---- setup ---------------------------------------------------------------- */

static void
log_display (WPEDisplay *display)
{
    guint n_screens = wpe_display_get_n_screens (display);
    WPEDRMDevice *drm_device;

    g_message ("WPE display %s connected (WPE WebKit %u.%u.%u, WPEPlatform %u.%u.%u)",
               G_OBJECT_TYPE_NAME (display),
               webkit_get_major_version (), webkit_get_minor_version (), webkit_get_micro_version (),
               wpe_platform_get_major_version (), wpe_platform_get_minor_version (), wpe_platform_get_micro_version ());

    for (guint i = 0; i < n_screens; i++) {
        WPEScreen *screen = wpe_display_get_screen (display, i);
        if (screen != NULL)
            g_message ("screen %u: %dx%d px, scale %.2f, refresh %d mHz", i,
                       wpe_screen_get_width (screen), wpe_screen_get_height (screen),
                       wpe_screen_get_scale (screen), wpe_screen_get_refresh_rate (screen));
    }

    /* For the DRM platform this is the render device (panfrost card0/renderD128 on Remote 3) when the
     * KMS device has no render node (WPEDisplayDRM.cpp:371-372, 486-490); the KMS node itself is
     * chosen by udev enumeration of card* devices with connectors (WPEDisplayDRM.cpp:130-178). */
    drm_device = wpe_display_get_drm_device (display);
    if (drm_device != NULL)
        g_message ("render device: primary %s, render %s",
                   wpe_drm_device_get_primary_node (drm_device) != NULL ? wpe_drm_device_get_primary_node (drm_device) : "-",
                   wpe_drm_device_get_render_node (drm_device) != NULL ? wpe_drm_device_get_render_node (drm_device) : "-");
    else
        g_message ("no DRM render device (software rendering?)");
}

static WebKitNetworkSession *
create_network_session (const char *data_dir)
{
    char *cache_dir;
    WebKitNetworkSession *session;

    if (g_mkdir_with_parents (data_dir, 0700) != 0) {
        g_warning ("cannot create data directory %s (%s), using an ephemeral session", data_dir, g_strerror (errno));
        return webkit_network_session_new_ephemeral ();
    }
    cache_dir = g_build_filename (data_dir, "cache", NULL);
    g_message ("website data in %s, cache in %s", data_dir, cache_dir);
    session = webkit_network_session_new (data_dir, cache_dir);
    g_free (cache_dir);
    return session;
}

static void
setup_toplevel (Launcher *launcher)
{
    WPEView *view = webkit_web_view_get_wpe_view (launcher->web_view);
    WPEToplevel *toplevel;
    int width = 0, height = 0;

    if (view == NULL) {
        g_warning ("web view has no WPEView (legacy backend?)");
        return;
    }
    g_signal_connect (wpe_view_get_display (view), "disconnected", G_CALLBACK (on_display_disconnected), launcher);
    g_signal_connect (view, "closed", G_CALLBACK (on_view_closed), launcher);

    toplevel = wpe_view_get_toplevel (view);
    if (toplevel == NULL) {
        g_warning ("WPE view has no toplevel");
        return;
    }
    wpe_toplevel_set_title (toplevel, "wpe-launcher");
    /* The DRM toplevel is always the whole screen and implements no fullscreen vfunc
     * (WPEToplevelDRM.cpp:70-73), so a FALSE return is expected there. */
    if (!(wpe_toplevel_get_state (toplevel) & WPE_TOPLEVEL_STATE_FULLSCREEN)) {
        if (!wpe_toplevel_fullscreen (toplevel))
            g_debug ("toplevel does not support fullscreen requests");
    }
    wpe_toplevel_get_size (toplevel, &width, &height);
    g_message ("toplevel %dx%d, state 0x%x, scale %.2f", width, height,
               (unsigned) wpe_toplevel_get_state (toplevel), wpe_toplevel_get_scale (toplevel));
}

static void
print_version (void)
{
    g_print ("wpe-launcher %s (WPE WebKit %u.%u.%u, WPEPlatform %u.%u.%u)\n", UC_WPE_LAUNCHER_VERSION,
             webkit_get_major_version (), webkit_get_minor_version (), webkit_get_micro_version (),
             wpe_platform_get_major_version (), wpe_platform_get_minor_version (), wpe_platform_get_micro_version ());
}

int
main (int argc, char **argv)
{
    Launcher launcher = { 0 };
    GError *error = NULL;
    WPEDisplay *display;
    WebKitSettings *settings;
    WebKitNetworkSession *session;
    WebKitColor background;
    const char *inspector;

    g_set_prgname ("wpe-launcher");

    launcher_config_init (&launcher.config);
    launcher_config_load_env (&launcher.config, NULL, NULL);
    if (!launcher_config_parse_args (&launcher.config, &argc, &argv, &error)) {
        g_printerr ("wpe-launcher: %s\n", error->message);
        g_error_free (error);
        launcher_config_clear (&launcher.config);
        return 2;
    }
    if (launcher.config.show_version) {
        print_version ();
        launcher_config_clear (&launcher.config);
        return 0;
    }

    if (launcher.config.headless)
        g_setenv ("WPE_PLATFORM", "headless", TRUE);

    g_message ("wpe-launcher %s starting: WPE_PLATFORM=%s, url %s%s", UC_WPE_LAUNCHER_VERSION,
               g_getenv ("WPE_PLATFORM") != NULL ? g_getenv ("WPE_PLATFORM") : "(auto)",
               launcher.config.uri, launcher.config.headless ? " (headless, exit after load)" : "");
    inspector = g_getenv ("WEBKIT_INSPECTOR_SERVER");
    if (inspector != NULL)
        g_message ("remote Web Inspector requested at %s (needs %s=1)", inspector, LAUNCHER_ENV_DEVELOPER_EXTRAS);

    display = wpe_display_get_default ();
    if (display == NULL) {
        g_printerr ("wpe-launcher: no WPE platform could be connected (set WPE_PLATFORM=drm or headless)\n");
        launcher_config_clear (&launcher.config);
        return 1;
    }
    log_display (display);

    launcher.loop = g_main_loop_new (NULL, FALSE);
    g_unix_signal_add (SIGTERM, on_unix_signal, &launcher);
    g_unix_signal_add (SIGINT, on_unix_signal, &launcher);

    settings = webkit_settings_new_with_settings (
        "enable-developer-extras", launcher.config.developer_extras,
        "enable-write-console-messages-to-stdout", launcher.config.console_log,
        NULL);
    session = create_network_session (launcher.config.data_dir);

    launcher.web_view = WEBKIT_WEB_VIEW (g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                                       "display", display,
                                                       "settings", settings,
                                                       "network-session", session,
                                                       NULL));
    g_signal_connect (launcher.web_view, "load-changed", G_CALLBACK (on_load_changed), &launcher);
    g_signal_connect (launcher.web_view, "load-failed", G_CALLBACK (on_load_failed), &launcher);
    g_signal_connect (launcher.web_view, "web-process-terminated", G_CALLBACK (on_web_process_terminated), &launcher);
    g_signal_connect (launcher.web_view, "close", G_CALLBACK (on_close), &launcher);

    if (webkit_color_parse (&background, launcher.config.background_color))
        webkit_web_view_set_background_color (launcher.web_view, &background);
    else
        g_warning ("invalid %s '%s', keeping the default background", LAUNCHER_ENV_BG_COLOR, launcher.config.background_color);

    setup_toplevel (&launcher);

    if (!launcher.config.no_dbus) {
        const KioskDBusHandlers handlers = { dbus_open, dbus_reload, dbus_get_uri, dbus_quit };
        launcher.dbus = kiosk_dbus_new (G_BUS_TYPE_SYSTEM, &handlers, &launcher);
    }
    if (launcher.config.headless)
        launcher.timeout_source = g_timeout_add_seconds (launcher.config.timeout_sec, on_headless_timeout, &launcher);

    load_requested_uri (&launcher, launcher.config.uri);
    g_main_loop_run (launcher.loop);

    cancel_pending_reload (&launcher);
    if (launcher.timeout_source != 0)
        g_source_remove (launcher.timeout_source);
    kiosk_dbus_free (launcher.dbus);
    g_clear_object (&launcher.web_view);
    g_object_unref (session);
    g_object_unref (settings);
    g_main_loop_unref (launcher.loop);
    g_free (launcher.requested_uri);
    launcher_config_clear (&launcher.config);

    g_message ("exiting with status %d", launcher.exit_code);
    return launcher.exit_code;
}
