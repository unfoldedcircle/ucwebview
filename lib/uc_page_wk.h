/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Messages from a view to the library's web process extension in its page (library 0.9.0, uc_ext_protocol.h has the
 * protocol): webkit_web_view_send_message_to_page(), but only while there is a page to take them. WebKit sends a
 * message to the web process of the view's committed page and starts one for about:blank when the view has none
 * (WebPageProxy::ensureRunningProcess(), WebPageProxy.cpp:2204-2213): before the view's first load, and after its web
 * process terminated until the host loads again. A message then would give a view that the host left empty or crashed
 * a web process of its own, so it is not sent. WebKit's script API did not start one after a termination
 * (WebPageProxy::runJavaScriptInFrameInScriptWorld(), WebPageProxy.cpp:7541-7545: an error), so the library's
 * behaviour stays what it was.
 *
 * The view's owner (uc_webview.c, tests/webkitgtk-host.c) calls the hooks from its own load-changed and
 * web-process-terminated handlers, before it tells the host, so that a host that reacts inside its callback already
 * sees the new state. UI thread only; WebKit GLib API that WPE and WebKitGTK share.
 */
#ifndef UC_PAGE_WK_H
#define UC_PAGE_WK_H

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _UcPageWk UcPageWk;

/* web_view must outlive it */
UcPageWk *uc_page_wk_new                    (WebKitWebView *web_view);
void      uc_page_wk_free                   (UcPageWk *page);

/* the hooks: every load-changed of the view, and web-process-terminated */
void      uc_page_wk_load_changed           (UcPageWk *page, WebKitLoadEvent event);
void      uc_page_wk_web_process_terminated (UcPageWk *page);

/* TRUE while a message reaches a page without WebKit starting a web process for it: the view has loaded something
 * (it has a URI) and its web process did not terminate since its last load started. */
gboolean  uc_page_wk_reachable              (UcPageWk *page);

/* name with params (a floating reference is consumed; NULL for none) to the shown page, without a reply. FALSE when
 * the page is not reachable: nothing is sent. */
gboolean  uc_page_wk_send                   (UcPageWk *page, const char *name, GVariant *params);

/* The same with a reply: callback runs from the main context with the WebKitWebView as source object (which the
 * call keeps alive until then), also when the page is not reachable, nothing is sent and the result is an error
 * (G_IO_ERROR_NOT_CONNECTED). uc_page_wk_send_finish() returns the reply's parameters (a reference, NULL for a reply
 * without) or NULL with error, also WebKit's: WEBKIT_USER_MESSAGE_UNHANDLED_MESSAGE when no extension took it. */
void      uc_page_wk_send_with_reply        (UcPageWk *page, const char *name, GVariant *params,
                                             GAsyncReadyCallback callback, gpointer user_data);
GVariant *uc_page_wk_send_finish            (GObject *source, GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* UC_PAGE_WK_H */
