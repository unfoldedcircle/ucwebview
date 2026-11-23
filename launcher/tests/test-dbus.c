/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host integration test for kiosk-dbus.c on a private session bus started by
 * GTestDBus (needs dbus-daemon in PATH). Run with `make check`.
 *
 * The method calls are made asynchronously and the default main context is
 * iterated while waiting: the exported object is dispatched on that context,
 * so a g_dbus_connection_call_sync() from the same thread would deadlock.
 */
#include "kiosk-dbus.h"

#include <string.h>

typedef struct {
    int   opens;
    int   reloads;
    int   quits;
    int   get_uris;
    char *last_uri;
} Calls;

static void
h_open (const char *uri, gpointer user_data)
{
    Calls *calls = user_data;
    calls->opens++;
    g_free (calls->last_uri);
    calls->last_uri = g_strdup (uri);
}

static void
h_reload (gpointer user_data)
{
    ((Calls *) user_data)->reloads++;
}

static char *
h_get_uri (gpointer user_data)
{
    ((Calls *) user_data)->get_uris++;
    return g_strdup ("http://localhost/current");
}

static void
h_quit (gpointer user_data)
{
    ((Calls *) user_data)->quits++;
}

typedef struct {
    GVariant *result;
    GError   *error;
    gboolean  done;
} CallResult;

static void
on_call_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    CallResult *result = user_data;
    result->result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &result->error);
    result->done = TRUE;
}

static gboolean
on_wait_timeout (gpointer user_data)
{
    *(gboolean *) user_data = TRUE;
    return G_SOURCE_REMOVE;
}

/* Calls @method on the kiosk object (or @interface if given) and spins the main context until the reply arrives. */
static GVariant *
call (GDBusConnection *connection, const char *interface, const char *method, GVariant *parameters, GError **error)
{
    CallResult result = { NULL, NULL, FALSE };
    gboolean timed_out = FALSE;
    guint timeout = g_timeout_add_seconds (10, on_wait_timeout, &timed_out);

    g_dbus_connection_call (connection, KIOSK_DBUS_NAME, KIOSK_DBUS_PATH,
                            interface != NULL ? interface : KIOSK_DBUS_INTERFACE, method, parameters,
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_call_done, &result);
    while (!result.done && !timed_out)
        g_main_context_iteration (NULL, TRUE);
    if (!timed_out)
        g_source_remove (timeout);
    g_assert_false (timed_out);

    if (result.error != NULL)
        g_propagate_error (error, result.error);
    return result.result;
}

static void
wait_until_exported (KioskDBus *dbus)
{
    gboolean timed_out = FALSE;
    guint timeout = g_timeout_add_seconds (10, on_wait_timeout, &timed_out);

    while (!kiosk_dbus_is_exported (dbus) && !timed_out)
        g_main_context_iteration (NULL, TRUE);
    if (!timed_out)
        g_source_remove (timeout);
    g_assert_true (kiosk_dbus_is_exported (dbus));
}

static void
test_methods (void)
{
    Calls calls = { 0, 0, 0, 0, NULL };
    const KioskDBusHandlers handlers = { h_open, h_reload, h_get_uri, h_quit };
    KioskDBus *dbus = kiosk_dbus_new (G_BUS_TYPE_SESSION, &handlers, &calls);
    GDBusConnection *connection;
    GError *error = NULL;
    GVariant *reply;
    const char *uri = NULL;

    wait_until_exported (dbus);
    connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    g_assert_no_error (error);

    reply = call (connection, NULL, "Open", g_variant_new ("(s)", "https://unfoldedcircle.com/"), &error);
    g_assert_no_error (error);
    g_assert_nonnull (reply);
    g_variant_unref (reply);
    g_assert_cmpint (calls.opens, ==, 1);
    g_assert_cmpstr (calls.last_uri, ==, "https://unfoldedcircle.com/");

    reply = call (connection, NULL, "Open", g_variant_new ("(s)", ""), &error);
    g_assert_null (reply);
    g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
    g_clear_error (&error);
    g_assert_cmpint (calls.opens, ==, 1);

    reply = call (connection, NULL, "Reload", NULL, &error);
    g_assert_no_error (error);
    g_variant_unref (reply);
    g_assert_cmpint (calls.reloads, ==, 1);

    reply = call (connection, NULL, "GetUri", NULL, &error);
    g_assert_no_error (error);
    g_variant_get (reply, "(&s)", &uri);
    g_assert_cmpstr (uri, ==, "http://localhost/current");
    g_variant_unref (reply);
    g_assert_cmpint (calls.get_uris, ==, 1);

    reply = call (connection, NULL, "Quit", NULL, &error);
    g_assert_no_error (error);
    g_variant_unref (reply);
    g_assert_cmpint (calls.quits, ==, 1);

    reply = call (connection, NULL, "Bogus", NULL, &error);
    g_assert_null (reply);
    g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD);
    g_clear_error (&error);

    /* introspection advertises the four methods */
    reply = call (connection, "org.freedesktop.DBus.Introspectable", "Introspect", NULL, &error);
    g_assert_no_error (error);
    {
        const char *xml = NULL;
        g_variant_get (reply, "(&s)", &xml);
        g_assert_nonnull (strstr (xml, "name=\"Open\""));
        g_assert_nonnull (strstr (xml, "name=\"Reload\""));
        g_assert_nonnull (strstr (xml, "name=\"GetUri\""));
        g_assert_nonnull (strstr (xml, "name=\"Quit\""));
    }
    g_variant_unref (reply);

    kiosk_dbus_free (dbus);
    g_object_unref (connection);
    g_free (calls.last_uri);
}

static void
test_get_uri_null (void)
{
    /* a NULL handler result is reported as the empty string */
    Calls calls = { 0, 0, 0, 0, NULL };
    const KioskDBusHandlers handlers = { NULL, NULL, NULL, NULL };
    KioskDBus *dbus = kiosk_dbus_new (G_BUS_TYPE_SESSION, &handlers, &calls);
    GDBusConnection *connection;
    GError *error = NULL;
    GVariant *reply;
    const char *uri = NULL;

    wait_until_exported (dbus);
    connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    g_assert_no_error (error);

    reply = call (connection, NULL, "GetUri", NULL, &error);
    g_assert_no_error (error);
    g_variant_get (reply, "(&s)", &uri);
    g_assert_cmpstr (uri, ==, "");
    g_variant_unref (reply);

    reply = call (connection, NULL, "Open", g_variant_new ("(s)", "about:blank"), &error);
    g_assert_no_error (error); /* missing handler is not an error */
    g_variant_unref (reply);

    kiosk_dbus_free (dbus);
    g_object_unref (connection);
}

static void
test_introspection_xml (void)
{
    GError *error = NULL;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml (kiosk_dbus_get_introspection_xml (), &error);
    g_assert_no_error (error);
    g_assert_nonnull (g_dbus_node_info_lookup_interface (info, KIOSK_DBUS_INTERFACE));
    g_dbus_node_info_unref (info);
}

int
main (int argc, char **argv)
{
    GTestDBus *bus;
    int status;

    g_test_init (&argc, &argv, NULL);
    bus = g_test_dbus_new (G_TEST_DBUS_NONE);
    g_test_dbus_up (bus);

    g_test_add_func ("/dbus/introspection-xml", test_introspection_xml);
    g_test_add_func ("/dbus/methods", test_methods);
    g_test_add_func ("/dbus/get-uri-null", test_get_uri_null);
    status = g_test_run ();

    g_test_dbus_down (bus);
    g_object_unref (bus);
    return status;
}
