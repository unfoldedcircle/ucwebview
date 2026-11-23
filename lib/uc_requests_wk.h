/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Requests of the page to the host (library 0.7.0), the WebKit side: the WebKitWebView signals script-dialog,
 * authenticate, create, close and decide-policy (responses), WebKitNetworkSession::download-started, and every way a
 * request ends (uc_requests.h has the table and the rules, uc_webview.h "Requests" the contract).
 *
 * It uses the WebKit GLib API that WPE and WebKitGTK share and nothing of WPEPlatform, so that the same file runs in
 * the library (uc_webview.c) and in the WebKitGTK host harness of the request modes of ucwebview-test
 * (tests/webkitgtk-host.c). The owner of the WebKitWebView calls the hooks below from its own signal handlers, in
 * this order relative to its host callbacks:
 *   load-changed            uc_wk_requests_load_changed(), then load_changed()
 *   load-failed             uc_wk_requests_load_failed() (TRUE: no FAILED for the host), then load_changed(FAILED)
 *   web-process-terminated  uc_wk_requests_web_process_terminated(), then web_process_terminated()
 *   navigation calls        uc_wk_requests_host_navigation(), then the WebKit call
 *   the user's input        uc_wk_requests_user_input() with the event (the host's input calls only; a dialog the
 *                           event causes comes later, through the web process)
 *   download-started        uc_wk_requests_download_started() (the session's signal; the owner finds the view)
 * Everything runs on the UI thread from the GLib main context.
 */
#ifndef UC_REQUESTS_WK_H
#define UC_REQUESTS_WK_H

#include "uc_requests.h"
#include "uc_webview.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

/* What the requests need of their view's owner. cb points to the view's own copy of the callbacks and must stay
 * valid until uc_wk_requests_free(). callback_depth is the owner's counter of running host callbacks
 * (uc_webview_free() refuses to run inside one). */
typedef struct {
    uc_webview *wv;
    const uc_webview_callbacks *cb;
    void *user;
    int *callback_depth;
    gboolean log_full_urls;     /* UC_WEBVIEW_LOG_FULL_URLS */
    gboolean pdf_viewer;        /* UC_WEBVIEW_PDF_VIEWER: PDF responses are WebKit's */
} UcWkRequestsHost;

typedef struct _UcWkRequests UcWkRequests;

/* Connects to web_view's request signals; web_view must outlive the result. */
UcWkRequests *uc_wk_requests_new                  (WebKitWebView *web_view, const UcWkRequestsHost *host);
/* uc_webview_free(): every pending request ends without a callback, the signal handlers go. NULL is ignored. */
void          uc_wk_requests_free                 (UcWkRequests *requests);

/* Before a navigation call of the host: pending requests end (request_cancelled() from the main context). leaves_page:
 * a load, reload or history step (not stop()): the page's next dialogs get WebKit's default until the load starts. */
void          uc_wk_requests_host_navigation      (UcWkRequests *requests, gboolean leaves_page);
/* The host forwarded the user's input to the page: a touch down, a pointer button or key press, text input of the
 * keyboard. Not for release, move, cancel or scroll events, nor for anything the library or the host makes up: the
 * dialog loop guard ends (by design, uc_dialog_guard_user_input()). requests NULL is ignored. */
void          uc_wk_requests_user_input           (UcWkRequests *requests);
void          uc_wk_requests_load_changed         (UcWkRequests *requests, WebKitLoadEvent event);
gboolean      uc_wk_requests_load_failed          (UcWkRequests *requests, const GError *error);
void          uc_wk_requests_web_process_terminated (UcWkRequests *requests);
/* requests NULL: a download of no view of ours, cancelled all the same */
void          uc_wk_requests_download_started     (UcWkRequests *requests, WebKitDownload *download,
                                                   gboolean log_full_urls);

/* uc_webview_request_reply() and uc_webview_request_auth() after the view and thread checks: 0 or -1 */
int           uc_wk_requests_reply                (UcWkRequests *requests, guint64 id, int accept, const char *text);
int           uc_wk_requests_auth                 (UcWkRequests *requests, guint64 id, const char *user,
                                                   const char *password);

G_END_DECLS

#endif /* UC_REQUESTS_WK_H */
