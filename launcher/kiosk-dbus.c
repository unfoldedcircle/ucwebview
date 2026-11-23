/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 */
#include "kiosk-dbus.h"

static const char introspection_xml[] =
    "<node>"
    "  <interface name='" KIOSK_DBUS_INTERFACE "'>"
    "    <method name='Open'>"
    "      <arg type='s' name='uri' direction='in'/>"
    "    </method>"
    "    <method name='Reload'/>"
    "    <method name='GetUri'>"
    "      <arg type='s' name='uri' direction='out'/>"
    "    </method>"
    "    <method name='Quit'/>"
    "  </interface>"
    "</node>";

struct _KioskDBus {
    GBusType           bus_type;
    guint              owner_id;
    guint              registration_id;
    GDBusConnection   *connection;
    GDBusNodeInfo     *node_info;
    KioskDBusHandlers  handlers;
    gpointer           user_data;
    gboolean           exported;
};

const char *
kiosk_dbus_get_introspection_xml (void)
{
    return introspection_xml;
}

static void
handle_method_call (GDBusConnection       *connection,
                    const char            *sender,
                    const char            *object_path,
                    const char            *interface_name,
                    const char            *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
    KioskDBus *self = user_data;
    (void) connection;
    (void) object_path;
    (void) interface_name;

    if (g_strcmp0 (method_name, "Open") == 0) {
        const char *uri = NULL;
        g_variant_get (parameters, "(&s)", &uri);
        if (uri == NULL || *uri == '\0') {
            g_dbus_method_invocation_return_error_literal (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                           "uri must not be empty");
            return;
        }
        g_message ("D-Bus Open(%s) from %s", uri, sender);
        if (self->handlers.open != NULL)
            self->handlers.open (uri, self->user_data);
        g_dbus_method_invocation_return_value (invocation, NULL);
    } else if (g_strcmp0 (method_name, "Reload") == 0) {
        g_message ("D-Bus Reload() from %s", sender);
        if (self->handlers.reload != NULL)
            self->handlers.reload (self->user_data);
        g_dbus_method_invocation_return_value (invocation, NULL);
    } else if (g_strcmp0 (method_name, "GetUri") == 0) {
        char *uri = self->handlers.get_uri != NULL ? self->handlers.get_uri (self->user_data) : NULL;
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", uri != NULL ? uri : ""));
        g_free (uri);
    } else if (g_strcmp0 (method_name, "Quit") == 0) {
        g_message ("D-Bus Quit() from %s", sender);
        g_dbus_method_invocation_return_value (invocation, NULL);
        if (self->handlers.quit != NULL)
            self->handlers.quit (self->user_data);
    } else {
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                               "unknown method %s", method_name);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    NULL, /* get_property */
    NULL, /* set_property */
    { 0 }
};

static const char *
bus_name (GBusType bus_type)
{
    return bus_type == G_BUS_TYPE_SESSION ? "session" : "system";
}

static void
on_bus_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
    KioskDBus *self = user_data;
    GError *error = NULL;
    (void) name;

    self->registration_id = g_dbus_connection_register_object (connection, KIOSK_DBUS_PATH,
                                                               self->node_info->interfaces[0],
                                                               &interface_vtable, self, NULL, &error);
    if (self->registration_id == 0) {
        g_warning ("D-Bus: cannot export %s: %s", KIOSK_DBUS_PATH, error->message);
        g_error_free (error);
        return;
    }
    self->connection = g_object_ref (connection);
}

static void
on_name_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
    KioskDBus *self = user_data;
    (void) connection;

    self->exported = self->registration_id != 0;
    g_message ("D-Bus: owning %s on the %s bus (%s)", name, bus_name (self->bus_type), KIOSK_DBUS_PATH);
}

static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer user_data)
{
    KioskDBus *self = user_data;

    self->exported = FALSE;
    if (connection == NULL)
        g_warning ("D-Bus: %s bus not reachable, control interface %s disabled", bus_name (self->bus_type), name);
    else
        g_warning ("D-Bus: lost or could not acquire name %s (another owner, or the bus policy denies it)", name);
}

KioskDBus *
kiosk_dbus_new (GBusType bus_type, const KioskDBusHandlers *handlers, gpointer user_data)
{
    KioskDBus *self = g_new0 (KioskDBus, 1);
    GError *error = NULL;

    self->bus_type = bus_type;
    self->handlers = *handlers;
    self->user_data = user_data;
    self->node_info = g_dbus_node_info_new_for_xml (introspection_xml, &error);
    g_assert_no_error (error); /* static XML, a failure is a programming error */

    self->owner_id = g_bus_own_name (bus_type, KIOSK_DBUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                     on_bus_acquired, on_name_acquired, on_name_lost, self, NULL);
    return self;
}

void
kiosk_dbus_free (KioskDBus *self)
{
    if (self == NULL)
        return;
    if (self->owner_id != 0)
        g_bus_unown_name (self->owner_id);
    if (self->connection != NULL && self->registration_id != 0)
        g_dbus_connection_unregister_object (self->connection, self->registration_id);
    g_clear_object (&self->connection);
    g_clear_pointer (&self->node_info, g_dbus_node_info_unref);
    g_free (self);
}

gboolean
kiosk_dbus_is_exported (const KioskDBus *self)
{
    return self != NULL && self->exported;
}
