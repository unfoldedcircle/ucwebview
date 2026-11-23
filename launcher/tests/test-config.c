/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for launcher-config.c (GLib only): run with `make check`.
 */
#include "launcher-config.h"

#include <string.h>

static const char *
fake_getenv (const char *name, gpointer user_data)
{
    GHashTable *env = user_data;
    return g_hash_table_lookup (env, name);
}

static void
test_env_bool (void)
{
    g_assert_true (launcher_env_bool ("1"));
    g_assert_true (launcher_env_bool ("true"));
    g_assert_true (launcher_env_bool ("TRUE"));
    g_assert_true (launcher_env_bool ("yes"));
    g_assert_true (launcher_env_bool ("On"));
    g_assert_false (launcher_env_bool ("0"));
    g_assert_false (launcher_env_bool ("false"));
    g_assert_false (launcher_env_bool (""));
    g_assert_false (launcher_env_bool (NULL));
    g_assert_false (launcher_env_bool ("2"));
}

static void
test_defaults (void)
{
    LauncherConfig config;
    GHashTable *env = g_hash_table_new (g_str_hash, g_str_equal);

    launcher_config_init (&config);
    launcher_config_load_env (&config, fake_getenv, env);

    g_assert_cmpstr (config.uri, ==, LAUNCHER_DEFAULT_URI);
    g_assert_cmpstr (config.background_color, ==, "black");
    g_assert_true (g_str_has_suffix (config.data_dir, "/" LAUNCHER_DEFAULT_DATA_SUBDIR));
    g_assert_false (config.developer_extras);
    g_assert_false (config.console_log);
    g_assert_false (config.headless);
    g_assert_false (config.no_dbus);
    g_assert_false (config.show_version);
    g_assert_cmpuint (config.timeout_sec, ==, 30);

    launcher_config_clear (&config);
    g_hash_table_unref (env);
}

static void
test_env (void)
{
    LauncherConfig config;
    GHashTable *env = g_hash_table_new (g_str_hash, g_str_equal);

    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_URI, (gpointer) "https://unfoldedcircle.com/");
    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_DEVELOPER_EXTRAS, (gpointer) "1");
    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_CONSOLE_LOG, (gpointer) "yes");
    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_BG_COLOR, (gpointer) "#202020");
    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_DATA_DIR, (gpointer) "/var/uc/wpe-launcher");

    launcher_config_init (&config);
    launcher_config_load_env (&config, fake_getenv, env);

    g_assert_cmpstr (config.uri, ==, "https://unfoldedcircle.com/");
    g_assert_true (config.developer_extras);
    g_assert_true (config.console_log);
    g_assert_cmpstr (config.background_color, ==, "#202020");
    g_assert_cmpstr (config.data_dir, ==, "/var/uc/wpe-launcher");

    /* empty values do not override the defaults */
    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_URI, (gpointer) "");
    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_DEVELOPER_EXTRAS, (gpointer) "0");
    launcher_config_clear (&config);
    launcher_config_init (&config);
    launcher_config_load_env (&config, fake_getenv, env);
    g_assert_cmpstr (config.uri, ==, LAUNCHER_DEFAULT_URI);
    g_assert_false (config.developer_extras);

    launcher_config_clear (&config);
    g_hash_table_unref (env);
}

static gboolean
parse (LauncherConfig *config, GError **error, ...)
{
    GPtrArray *args = g_ptr_array_new ();
    va_list ap;
    const char *arg;
    char **argv;
    int argc;
    gboolean ok;

    g_ptr_array_add (args, g_strdup ("wpe-launcher"));
    va_start (ap, error);
    while ((arg = va_arg (ap, const char *)) != NULL)
        g_ptr_array_add (args, g_strdup (arg));
    va_end (ap);
    g_ptr_array_add (args, NULL);

    argv = (char **) g_ptr_array_free (args, FALSE);
    argc = g_strv_length (argv);
    {
        char **argv_copy = argv; /* g_option_context_parse() modifies the array in place */
        ok = launcher_config_parse_args (config, &argc, &argv_copy, error);
    }
    g_strfreev (argv);
    return ok;
}

static void
test_args_positional_url (void)
{
    LauncherConfig config;
    GError *error = NULL;
    GHashTable *env = g_hash_table_new (g_str_hash, g_str_equal);

    g_hash_table_insert (env, (gpointer) LAUNCHER_ENV_URI, (gpointer) "http://from-env/");
    launcher_config_init (&config);
    launcher_config_load_env (&config, fake_getenv, env);
    g_assert_true (parse (&config, &error, "about:blank", NULL));
    g_assert_no_error (error);
    g_assert_cmpstr (config.uri, ==, "about:blank"); /* argv wins over KIOSK_URL */
    g_assert_false (config.headless);
    g_assert_false (config.no_dbus);
    launcher_config_clear (&config);

    /* no positional argument keeps the environment value */
    launcher_config_init (&config);
    launcher_config_load_env (&config, fake_getenv, env);
    g_assert_true (parse (&config, &error, NULL));
    g_assert_cmpstr (config.uri, ==, "http://from-env/");
    launcher_config_clear (&config);
    g_hash_table_unref (env);
}

static void
test_args_headless (void)
{
    LauncherConfig config;
    GError *error = NULL;

    launcher_config_init (&config);
    g_assert_true (parse (&config, &error, "--headless", "--timeout", "5", "about:blank", NULL));
    g_assert_no_error (error);
    g_assert_true (config.headless);
    g_assert_true (config.no_dbus); /* implied by --headless */
    g_assert_cmpuint (config.timeout_sec, ==, 5);
    g_assert_cmpstr (config.uri, ==, "about:blank");
    launcher_config_clear (&config);

    launcher_config_init (&config);
    g_assert_true (parse (&config, &error, "--headless", NULL));
    g_assert_cmpuint (config.timeout_sec, ==, 30);
    launcher_config_clear (&config);
}

static void
test_args_flags (void)
{
    LauncherConfig config;
    GError *error = NULL;

    launcher_config_init (&config);
    g_assert_true (parse (&config, &error, "--no-dbus", "-V", NULL));
    g_assert_true (config.no_dbus);
    g_assert_true (config.show_version);
    g_assert_false (config.headless);
    launcher_config_clear (&config);
}

static void
test_args_errors (void)
{
    LauncherConfig config;
    GError *error = NULL;

    launcher_config_init (&config);
    g_assert_false (parse (&config, &error, "http://a/", "http://b/", NULL));
    g_assert_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
    g_clear_error (&error);
    launcher_config_clear (&config);

    launcher_config_init (&config);
    g_assert_false (parse (&config, &error, "--timeout", "0", NULL));
    g_assert_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
    g_clear_error (&error);
    launcher_config_clear (&config);

    launcher_config_init (&config);
    g_assert_false (parse (&config, &error, "--bogus", NULL));
    g_assert_error (error, G_OPTION_ERROR, G_OPTION_ERROR_UNKNOWN_OPTION);
    g_clear_error (&error);
    launcher_config_clear (&config);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/config/env-bool", test_env_bool);
    g_test_add_func ("/config/defaults", test_defaults);
    g_test_add_func ("/config/env", test_env);
    g_test_add_func ("/config/args/positional-url", test_args_positional_url);
    g_test_add_func ("/config/args/headless", test_args_headless);
    g_test_add_func ("/config/args/flags", test_args_flags);
    g_test_add_func ("/config/args/errors", test_args_errors);
    return g_test_run ();
}
