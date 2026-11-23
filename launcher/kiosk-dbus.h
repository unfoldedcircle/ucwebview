/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * D-Bus control interface of wpe-launcher (GDBus, no WebKit dependency).
 *
 *   name       org.unfoldedcircle.WebKiosk
 *   object     /org/unfoldedcircle/WebKiosk
 *   interface  org.unfoldedcircle.WebKiosk
 *     Open(s uri)      load a URL
 *     Reload()         reload the current page
 *     GetUri() -> s    current URL ("" while nothing is loaded)
 *     Quit()           exit the launcher (systemd restarts the service)
 *
 * The name is requested asynchronously; if the bus is not reachable the
 * launcher keeps running without the interface (a warning is logged).
 * All functions and callbacks run on the thread-default main context of
 * the thread that called kiosk_dbus_new() (the GLib main thread).
 */
#ifndef UC_WPE_LAUNCHER_KIOSK_DBUS_H
#define UC_WPE_LAUNCHER_KIOSK_DBUS_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define KIOSK_DBUS_NAME      "org.unfoldedcircle.WebKiosk"
#define KIOSK_DBUS_PATH      "/org/unfoldedcircle/WebKiosk"
#define KIOSK_DBUS_INTERFACE "org.unfoldedcircle.WebKiosk"

typedef struct {
    void  (*open)    (const char *uri, gpointer user_data);
    void  (*reload)  (gpointer user_data);
    char *(*get_uri) (gpointer user_data); /* transfer full, NULL allowed */
    void  (*quit)    (gpointer user_data);
} KioskDBusHandlers;

typedef struct _KioskDBus KioskDBus;

KioskDBus  *kiosk_dbus_new                   (GBusType bus_type, const KioskDBusHandlers *handlers, gpointer user_data);
void        kiosk_dbus_free                  (KioskDBus *self);
gboolean    kiosk_dbus_is_exported           (const KioskDBus *self);
const char *kiosk_dbus_get_introspection_xml (void);

G_END_DECLS

#endif /* UC_WPE_LAUNCHER_KIOSK_DBUS_H */
