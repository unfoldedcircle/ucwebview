/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Messages from a view to the library's web process extension, see uc_page_wk.h.
 */
#include "uc_page_wk.h"

#include <gio/gio.h>

struct _UcPageWk {
    WebKitWebView *web_view;    /* borrowed */
    gboolean process_gone;      /* web-process-terminated, no load started since */
};

UcPageWk *
uc_page_wk_new (WebKitWebView *web_view)
{
    UcPageWk *page = g_new0 (UcPageWk, 1);

    page->web_view = web_view;
    return page;
}

void
uc_page_wk_free (UcPageWk *page)
{
    g_free (page);
}

void
uc_page_wk_load_changed (UcPageWk *page, WebKitLoadEvent event)
{
    /* a load after a termination runs in a new web process (WebKit starts it for the load) */
    if (page != NULL && event == WEBKIT_LOAD_STARTED)
        page->process_gone = FALSE;
}

void
uc_page_wk_web_process_terminated (UcPageWk *page)
{
    if (page != NULL)
        page->process_gone = TRUE;
}

gboolean
uc_page_wk_reachable (UcPageWk *page)
{
    return page != NULL && !page->process_gone && webkit_web_view_get_uri (page->web_view) != NULL;
}

gboolean
uc_page_wk_send (UcPageWk *page, const char *name, GVariant *params)
{
    if (params != NULL)
        g_variant_ref_sink (params);
    if (!uc_page_wk_reachable (page)) {
        if (params != NULL)
            g_variant_unref (params);
        return FALSE;
    }
    /* the message takes the parameters; without a callback WebKit expects no reply (WebKitWebView.cpp:5606-5608) */
    webkit_web_view_send_message_to_page (page->web_view, webkit_user_message_new (name, params), NULL, NULL, NULL);
    if (params != NULL)
        g_variant_unref (params);
    return TRUE;
}

void
uc_page_wk_send_with_reply (UcPageWk *page, const char *name, GVariant *params, GAsyncReadyCallback callback,
                            gpointer user_data)
{
    if (params != NULL)
        g_variant_ref_sink (params);
    if (uc_page_wk_reachable (page)) {
        webkit_web_view_send_message_to_page (page->web_view, webkit_user_message_new (name, params), NULL, callback,
                                              user_data);
    } else {
        /* GTask defers the callback to the main context (g_task_return_* in the iteration that made the task) */
        GTask *task = g_task_new (page->web_view, NULL, callback, user_data);
        g_task_set_source_tag (task, uc_page_wk_send_with_reply);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "no page to take the message");
        g_object_unref (task);
    }
    if (params != NULL)
        g_variant_unref (params);
}

GVariant *
uc_page_wk_send_finish (GObject *source, GAsyncResult *result, GError **error)
{
    WebKitUserMessage *reply;
    GVariant *params;

    if (G_IS_TASK (result) && g_task_get_source_tag (G_TASK (result)) == uc_page_wk_send_with_reply)
        return g_task_propagate_pointer (G_TASK (result), error);
    reply = webkit_web_view_send_message_to_page_finish (WEBKIT_WEB_VIEW (source), result, error);
    if (reply == NULL)
        return NULL;
    params = webkit_user_message_get_parameters (reply);
    if (params != NULL)
        g_variant_ref (params);
    g_object_unref (reply);
    return params;
}
