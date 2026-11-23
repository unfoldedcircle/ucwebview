/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Site settings of library 0.8.0, the WebKit side: cookies into the network session's cookie manager, website data
 * clearing, the certificate exceptions at a main-frame TLS failure, and the origin header table for the web process
 * extension (uc_headers.h has the rules, uc_site.h the checks, uc_webview.h "Site settings" the contract). Since
 * 0.10.3 also the main-frame loads the library repeats: an accepted certificate, and a load that took over the
 * connection of a preconnect which failed on the certificate (uc_load_error.h).
 *
 * Like uc_requests_wk.c it uses only the WebKit GLib API that WPE and WebKitGTK share, so that the library
 * (uc_webview.c) and the WebKitGTK host harness of ucwebview-test (tests/webkitgtk-host.c) run the same code. What
 * differs between the 2022 API of WPE 2.0 and WebKitGTK 4.1 (where the cookie manager and the TLS policy live, how
 * the web process extension directory is set) the owner passes in. Everything runs on the UI thread from the GLib
 * main context; the done callbacks come from an idle, never from inside a uc_webview_* call.
 */
#ifndef UC_SITE_WK_H
#define UC_SITE_WK_H

#include "uc_headers.h"
#include "uc_site.h"
#include "uc_webview.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

/* The file name of the library's web process extension in its directory (UC_WEBVIEW_EXTENSION_DIR). */
#define UC_SITE_WK_EXTENSION_NAME "libucwebview-extension.so"

/* uc_webview_global_set_cookies() after the thread and initialisation checks: checks every cookie (uc_site.h, plus
 * the public suffix rule of the cookie jar, SoupCookieJar drops those, soup-cookie-jar.c:613-618), then adds them
 * with webkit_cookie_manager_add_cookie() and calls cb once all are in. 0 or -1 (nothing added). */
int       uc_site_wk_set_cookies     (WebKitCookieManager *manager, const char *url, const uc_webview_cookie *cookies,
                                      gsize count, uc_webview_done_cb cb, void *user, gboolean log_full_urls);
/* uc_webview_global_clear_data() after the thread and initialisation checks. 0 or -1. */
int       uc_site_wk_clear_data      (WebKitWebsiteDataManager *manager, unsigned types, uc_webview_done_cb cb,
                                      void *user);

/* Accepts certificate for host in the network session (WPE 2.0: webkit_network_session_allow_tls_certificate_for_host,
 * WebKitGTK 4.1: webkit_web_context_allow_tls_certificate_for_host). */
typedef void (*UcSiteWkAllowFunc) (GTlsCertificate *certificate, const char *host, gpointer user_data);

/* The owner's WebKitWebView::load-failed-with-tls-errors handler calls this and returns its result. TRUE: an
 * exception of tls matched, the certificate is accepted for the URI's host and the URI loads again from an idle
 * (unless the view's "ucwebview" object data is gone by then: the view was freed; or another navigation came first,
 * see uc_site_wk_navigation()); WebKit then emits FINISHED without load-failed. FALSE: no exception (logged with the
 * certificate's fingerprint, which is what the host needs for one), or the same URI failed again within 10 s of the
 * library's own retry; WebKit goes on with load-failed. */
gboolean  uc_site_wk_tls_error       (WebKitWebView *web_view, const char *failing_uri, GTlsCertificate *certificate,
                                      GTlsCertificateFlags errors, const UcTlsExceptions *tls,
                                      UcSiteWkAllowFunc allow, gpointer allow_data, gboolean log_full_urls);
/* The owner calls this for every load-changed STARTED (started TRUE, first thing in its handler) and for every
 * navigation call of the host (load_uri, load_html, reload, stop, go_back, go_forward; started FALSE). A load the
 * library repeats from an idle (an accepted certificate, a cancelled load below) is not repeated when one of them came
 * between the failure and the idle (library 0.10.3). */
void      uc_site_wk_navigation      (WebKitWebView *web_view, gboolean started);
/* The owner's WebKitWebView::load-failed handler calls this after its own early returns (WebKit's own cancellation,
 * a refused response) and returns TRUE when it does (library 0.10.3): the main-frame load failed with
 * G_IO_ERROR_CANCELLED a moment after STARTED because it took over the connection of a preconnect that had failed on
 * the server's certificate (uc_load_error_cancelled_early()); the URI loads again from an idle, at most twice within
 * 10 s (UcLoadRetries), and the failure goes neither to the host nor to WebKit's error page; WebKit emits FINISHED.
 * FALSE: any other failure, or the guard's limit; logged either way when it was such a cancellation. */
gboolean  uc_site_wk_load_cancelled  (WebKitWebView *web_view, WebKitLoadEvent event, const char *failing_uri,
                                      const GError *error, gboolean log_full_urls);
/* SHA-256 of the certificate's DER encoding in lower-case hex, NULL without one. Free with g_free(). */
char     *uc_site_wk_certificate_sha256 (GTlsCertificate *certificate);

/* The origin headers of every view in one table for the extension (uc_headers.h): each view keeps its own table,
 * the owner merges them into this one after every change and sends it to the web processes. */
typedef struct _UcSiteWkHeaders UcSiteWkHeaders;

UcSiteWkHeaders *uc_site_wk_headers_new       (WebKitWebContext *context);
void             uc_site_wk_headers_free      (UcSiteWkHeaders *headers);
/* The next sequence number for uc_headers_table_set(). */
guint64          uc_site_wk_headers_next_seq  (UcSiteWkHeaders *headers);
/* Rebuilds the merged table from views (UcHeadersTable*, one per view; NULL entries are skipped) and, when it
 * changed, sends it to every web process (webkit_web_context_send_message_to_all_extensions). */
void             uc_site_wk_headers_publish   (UcSiteWkHeaders *headers, GPtrArray *views);
/* The initialization data of a web process about to start (WebKitWebContext::initialize-web-process-extensions, or
 * initialize-web-extensions of WebKitGTK 4.1): the current table, a floating reference. */
GVariant        *uc_site_wk_headers_user_data (UcSiteWkHeaders *headers);

G_END_DECLS

#endif /* UC_SITE_WK_H */
