/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * wpe-launcher configuration: environment keys and command line options.
 * Pure GLib, no WebKit dependency, so it can be unit tested on the host.
 */
#ifndef UC_WPE_LAUNCHER_CONFIG_H
#define UC_WPE_LAUNCHER_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/* Environment keys (set in /etc/default/wpe-launcher, see README.md). */
#define LAUNCHER_ENV_URI              "KIOSK_URL"
#define LAUNCHER_ENV_DEVELOPER_EXTRAS "WPE_LAUNCHER_DEVELOPER_EXTRAS"
#define LAUNCHER_ENV_CONSOLE_LOG      "WPE_LAUNCHER_CONSOLE_LOG"
#define LAUNCHER_ENV_BG_COLOR         "WPE_LAUNCHER_BG_COLOR"
#define LAUNCHER_ENV_DATA_DIR         "WPE_LAUNCHER_DATA_DIR"

#define LAUNCHER_DEFAULT_URI          "http://localhost/"
#define LAUNCHER_DEFAULT_BG_COLOR     "black"
#define LAUNCHER_DEFAULT_TIMEOUT_SEC  30
#define LAUNCHER_DEFAULT_DATA_SUBDIR  "wpe-launcher"

/* Lookup function used by launcher_config_load_env(); NULL means g_getenv(). */
typedef const char *(*LauncherGetenvFunc) (const char *name, gpointer user_data);

typedef struct {
    char    *uri;              /* page to load, positional argument > KIOSK_URL > default */
    char    *data_dir;         /* website data (cookies, local storage); cache below it */
    char    *background_color; /* CSS colour, painted before the first frame */
    gboolean developer_extras; /* enable-developer-extras (Web Inspector) */
    gboolean console_log;      /* enable-write-console-messages-to-stdout */
    gboolean headless;         /* --headless: load once, exit 0/1, WPE_PLATFORM=headless */
    gboolean no_dbus;          /* --no-dbus: do not own org.unfoldedcircle.WebKiosk */
    gboolean show_version;     /* --version */
    guint    timeout_sec;      /* --timeout for --headless, default 30 */
} LauncherConfig;

/* All functions run on the thread that calls main(); the struct is not shared. */
void     launcher_config_init       (LauncherConfig *config);
void     launcher_config_clear      (LauncherConfig *config);
void     launcher_config_load_env   (LauncherConfig *config, LauncherGetenvFunc getenv_func, gpointer user_data);
gboolean launcher_config_parse_args (LauncherConfig *config, int *argc, char ***argv, GError **error);

/* "1", "true", "yes", "on" (case-insensitive) are TRUE; anything else, including NULL, is FALSE. */
gboolean launcher_env_bool          (const char *value);

G_END_DECLS

#endif /* UC_WPE_LAUNCHER_CONFIG_H */
