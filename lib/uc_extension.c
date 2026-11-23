/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * libucwebview-extension.so: the web process extension of libucwebview (library 0.8.0). Every WPEWebProcess loads
 * it from the directory the library sets on the default WebKitWebContext (uc_webview.c setup_extension(),
 * WebProcessExtensionManager.cpp:38-49, 101-123). It does two things:
 *  - the origin headers of uc_webview_set_origin_headers() (0.8.0, uc_headers.h has the rules). The table comes as
 *    the initialization data of the process and, after every change, as a message to all web processes;
 *    WebKitWebPage::send-request applies it to every request of every page of the process (WebKitWebPage.cpp:313-325).
 *  - the library's script world (0.9.0, uc_ext_protocol.h): it creates the WebKitScriptWorld UC_EXT_HOST_WORLD before
 *    the first page, so that the library's user content in that world and the extension share it, and runs the
 *    library's scripts there and in the page's world with JavaScriptCore, which knows no user gesture: the device
 *    state for web pages (uc_ext_device.c), uc_webview_when_rendered()'s probe and the magnification scripts
 *    (UC_EXT_SCRIPT_MESSAGE, below). The view's messages to its page come as WebKitWebPage::user-message-received.
 *
 * It runs inside the page's process for every site the view shows, so it stays small: no logging of values, no
 * allocation per request beyond the origin strings, nothing but the headers and scripts it was told about.
 *
 * UC_EXTENSION_WEBKITGTK41: the build for the WebKitGTK 4.1 host harness (tests/webkitgtk-host.c), whose web process
 * looks for the entry point of the old GLib API, webkit_web_extension_initialize_with_user_data
 * (WebProcessExtensionManager.cpp:71-89). Its WebKitWebExtension has the same signals as WebKitWebProcessExtension,
 * and WebKitWebPage, WebKitURIRequest and WebKitURIResponse are the same, so only the entry point differs.
 */
#include "uc_ext_device.h"
#include "uc_ext_protocol.h"
#include "uc_headers.h"

#include <gmodule.h>
#include <math.h>
#include <libsoup/soup.h>
#include <wpe/webkit-web-process-extension.h>

static UcHeadersTable *table;
static guint32 generation;
static WebKitScriptWorld *host_world;   /* UC_EXT_HOST_WORLD, for the life of the process */

static const char *
soup_get (gpointer data, const char *name)
{
    return soup_message_headers_get_one (data, name);
}

static void
soup_remove (gpointer data, const char *name)
{
    soup_message_headers_remove (data, name);
}

static void
soup_set (gpointer data, const char *name, const char *value)
{
    soup_message_headers_replace (data, name, value);
}

static const UcHeaderOps soup_ops = { soup_get, soup_remove, soup_set };

static gboolean
on_send_request (WebKitWebPage *page, WebKitURIRequest *request, WebKitURIResponse *redirected_response,
                 gpointer user_data)
{
    SoupMessageHeaders *headers;

    (void) user_data;
    if (uc_headers_table_size (table) == 0)
        return FALSE;
    /* NULL for a request that is not HTTP (WebKitURIRequest.cpp:166-179); the changes go back into the request
     * (webkitURIRequestGetResourceRequest(), WebKitWebPage.cpp:324) */
    headers = webkit_uri_request_get_http_headers (request);
    if (headers == NULL)
        return FALSE;
    /* the rules and the value rule of uc_headers.h; a process that took over a cross-site redirect sees it as a
     * first request (redirected_response NULL) */
    uc_headers_apply (table, webkit_uri_request_get_uri (request), webkit_web_page_get_uri (page),
                      redirected_response != NULL ? webkit_uri_response_get_uri (redirected_response) : NULL,
                      &soup_ops, headers);
    return FALSE;   /* other handlers run, the request goes out */
}

/* UC_EXT_SCRIPT_MESSAGE: the library's script in its world of the main frame's current document, run right here,
 * after every message the view sent to the page before (a zoom has laid the page out in its own message), with
 * JavaScriptCore: no user gesture, no ScriptController (WebKit's evaluate_javascript() forced one). The reply says
 * whether it ran and its number. An exception is not logged: it ends the library's request, which logs it. */
static gboolean
host_script (WebKitWebPage *page, WebKitUserMessage *message)
{
    GVariant *params = webkit_user_message_get_parameters (message);
    const char *script = NULL;
    gboolean ran = FALSE;
    double number = NAN;

    if (g_strcmp0 (webkit_user_message_get_name (message), UC_EXT_SCRIPT_MESSAGE) != 0)
        return FALSE;
    if (params != NULL && g_variant_is_of_type (params, G_VARIANT_TYPE (UC_EXT_SCRIPT_VARIANT_TYPE)))
        g_variant_get (params, "(&s)", &script);
    if (script != NULL) {
        JSCContext *context = webkit_frame_get_js_context_for_script_world (uc_ext_main_frame (page), host_world);
        JSCValue *result = jsc_context_evaluate_with_source_uri (context, script, -1, "ucwebview-host-script", 1);

        ran = jsc_context_get_exception (context) == NULL;
        jsc_context_clear_exception (context);
        if (ran && result != NULL && jsc_value_is_number (result))
            number = jsc_value_to_double (result);
        g_clear_object (&result);
        g_object_unref (context);
    }
    webkit_user_message_send_reply (message, webkit_user_message_new (UC_EXT_SCRIPT_MESSAGE,
                                                                      g_variant_new (UC_EXT_SCRIPT_REPLY_TYPE, ran,
                                                                                     number)));
    return TRUE;
}

/* WebKitWebPage::user-message-received: webkit_web_view_send_message_to_page() of the library, dispatched
 * synchronously with the view's other messages to the page (WebPageGLib.cpp:99-123). FALSE for a message of
 * another name: WebKit answers it as unhandled. */
static gboolean
on_page_message (WebKitWebPage *page, WebKitUserMessage *message, gpointer user_data)
{
    (void) user_data;
    return uc_ext_device_message (page, message) || host_script (page, message);
}

static void
on_page_created (GObject *extension, WebKitWebPage *page, gpointer user_data)
{
    (void) extension; (void) user_data;
    g_signal_connect (page, "send-request", G_CALLBACK (on_send_request), NULL);
    g_signal_connect (page, "user-message-received", G_CALLBACK (on_page_message), NULL);
}

/* WebKitWebProcessExtension::user-message-received: webkit_web_context_send_message_to_all_extensions() of the
 * library. A message queued for this process before it started can be older than its initialization data: the
 * generation decides (uc_headers_table_load()). */
static gboolean
on_user_message (GObject *extension, WebKitUserMessage *message, gpointer user_data)
{
    (void) extension; (void) user_data;
    if (g_strcmp0 (webkit_user_message_get_name (message), UC_HEADERS_MESSAGE) != 0)
        return FALSE;
    uc_headers_table_load (table, webkit_user_message_get_parameters (message), &generation);
    return TRUE;
}

static void
initialize (GObject *extension, GVariant *user_data)
{
    table = uc_headers_table_new ();
    uc_headers_table_load (table, user_data, &generation);
    /* before the first page: the web process maps the library's user content world of this name to this one
     * (uc_ext_protocol.h) */
    host_world = webkit_script_world_new_with_name (UC_EXT_HOST_WORLD);
    uc_ext_device_init (host_world);
    g_signal_connect (extension, "page-created", G_CALLBACK (on_page_created), NULL);
    g_signal_connect (extension, "user-message-received", G_CALLBACK (on_user_message), NULL);
}

#ifdef UC_EXTENSION_WEBKITGTK41
G_MODULE_EXPORT void webkit_web_extension_initialize_with_user_data (GObject *extension, GVariant *user_data);

G_MODULE_EXPORT void
webkit_web_extension_initialize_with_user_data (GObject *extension, GVariant *user_data)
{
    initialize (extension, user_data);
}
#else
G_MODULE_EXPORT void webkit_web_process_extension_initialize_with_user_data (WebKitWebProcessExtension *extension,
                                                                              GVariant *user_data);

G_MODULE_EXPORT void
webkit_web_process_extension_initialize_with_user_data (WebKitWebProcessExtension *extension, GVariant *user_data)
{
    initialize (G_OBJECT (extension), user_data);
}
#endif
