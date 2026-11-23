/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 */
#include "launcher-config.h"

#include <string.h>

gboolean
launcher_env_bool (const char *value)
{
    if (value == NULL)
        return FALSE;
    return g_ascii_strcasecmp (value, "1") == 0 ||
           g_ascii_strcasecmp (value, "true") == 0 ||
           g_ascii_strcasecmp (value, "yes") == 0 ||
           g_ascii_strcasecmp (value, "on") == 0;
}

void
launcher_config_init (LauncherConfig *config)
{
    memset (config, 0, sizeof *config);
    config->uri = g_strdup (LAUNCHER_DEFAULT_URI);
    config->background_color = g_strdup (LAUNCHER_DEFAULT_BG_COLOR);
    /* $XDG_DATA_HOME/wpe-launcher, i.e. $HOME/.local/share/wpe-launcher */
    config->data_dir = g_build_filename (g_get_user_data_dir (), LAUNCHER_DEFAULT_DATA_SUBDIR, NULL);
    config->timeout_sec = LAUNCHER_DEFAULT_TIMEOUT_SEC;
}

void
launcher_config_clear (LauncherConfig *config)
{
    g_clear_pointer (&config->uri, g_free);
    g_clear_pointer (&config->data_dir, g_free);
    g_clear_pointer (&config->background_color, g_free);
}

static const char *
default_getenv (const char *name, gpointer user_data)
{
    (void) user_data;
    return g_getenv (name);
}

static void
replace_if_set (char **dest, const char *value)
{
    if (value != NULL && *value != '\0') {
        g_free (*dest);
        *dest = g_strdup (value);
    }
}

void
launcher_config_load_env (LauncherConfig *config, LauncherGetenvFunc getenv_func, gpointer user_data)
{
    if (getenv_func == NULL)
        getenv_func = default_getenv;

    replace_if_set (&config->uri, getenv_func (LAUNCHER_ENV_URI, user_data));
    replace_if_set (&config->data_dir, getenv_func (LAUNCHER_ENV_DATA_DIR, user_data));
    replace_if_set (&config->background_color, getenv_func (LAUNCHER_ENV_BG_COLOR, user_data));
    config->developer_extras = launcher_env_bool (getenv_func (LAUNCHER_ENV_DEVELOPER_EXTRAS, user_data));
    config->console_log = launcher_env_bool (getenv_func (LAUNCHER_ENV_CONSOLE_LOG, user_data));
}

gboolean
launcher_config_parse_args (LauncherConfig *config, int *argc, char ***argv, GError **error)
{
    gboolean headless = FALSE;
    gboolean no_dbus = FALSE;
    gboolean version = FALSE;
    int timeout = -1;
    char **remaining = NULL;
    const GOptionEntry entries[] = {
        { "headless", 0, 0, G_OPTION_ARG_NONE, &headless,
          "Render with WPE_PLATFORM=headless, exit 0 when the page finished loading, 1 on failure or timeout", NULL },
        { "timeout", 't', 0, G_OPTION_ARG_INT, &timeout,
          "Seconds to wait for the page in --headless mode (default 30)", "SEC" },
        { "no-dbus", 0, 0, G_OPTION_ARG_NONE, &no_dbus,
          "Do not export the org.unfoldedcircle.WebKiosk control interface", NULL },
        { "version", 'V', 0, G_OPTION_ARG_NONE, &version, "Print the version and exit", NULL },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &remaining, NULL, "[URL]" },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };
    GOptionContext *context = g_option_context_new ("[URL] - WPE WebKit kiosk launcher");
    gboolean ok;

    g_option_context_add_main_entries (context, entries, NULL);
    g_option_context_set_summary (context,
        "Fullscreen WPE WebKit browser on the WPEPlatform display selected by WPE_PLATFORM\n"
        "(drm, headless). The URL defaults to $" LAUNCHER_ENV_URI " or " LAUNCHER_DEFAULT_URI ".");
    ok = g_option_context_parse (context, argc, argv, error);
    g_option_context_free (context);
    if (!ok) {
        g_strfreev (remaining);
        return FALSE;
    }

    if (remaining != NULL && remaining[0] != NULL && remaining[1] != NULL) {
        g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                     "only one URL may be given (got '%s' and '%s')", remaining[0], remaining[1]);
        g_strfreev (remaining);
        return FALSE;
    }
    if (timeout != -1 && timeout <= 0) {
        g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                     "--timeout must be a positive number of seconds (got %d)", timeout);
        g_strfreev (remaining);
        return FALSE;
    }

    if (remaining != NULL && remaining[0] != NULL)
        replace_if_set (&config->uri, remaining[0]);
    g_strfreev (remaining);

    if (timeout > 0)
        config->timeout_sec = (guint) timeout;
    config->headless = headless;
    /* The smoke test must not depend on the system bus. */
    config->no_dbus = no_dbus || headless;
    config->show_version = version;
    return TRUE;
}
