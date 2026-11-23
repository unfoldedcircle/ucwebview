/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Why a main-frame load failed (library 0.10.0): the GError of WebKitWebView::load-failed as a
 * UC_WEBVIEW_FAIL_* reason for load_failed_detail(). GLib/GIO only, so that the mapping is unit-testable on a host
 * without WPE (tests/test-load-error.c).
 *
 * Where the errors come from in WPE WebKit 2.54:
 *  - WebKitNavigationClient.cpp:62-73, 85-92: the UI process builds the GError from the network process'
 *    ResourceError: domain g_quark_from_string(ResourceError::domain()), code toWebKitError(errorCode())
 *    (WebKitPrivate.cpp:135-184, passes every code it does not know unchanged), message the localized description.
 *  - ResourceErrorSoup.cpp:83-99: an error of libsoup or GIO keeps its GError domain and code (genericGError,
 *    httpError): g-resolver-error-quark for DNS, g-io-error-quark for the connection, g-tls-error-quark.
 *  - ResourceErrorSoup.cpp:101-107: a certificate WebKit does not accept is g-tls-error-quark
 *    G_TLS_ERROR_BAD_CERTIFICATE with the GTlsCertificateFlags in ResourceError::tlsErrors(); the UI process emits
 *    load-failed-with-tls-errors first and load-failed only when no handler took it (WebKitWebView.cpp:2783-2801), so
 *    the flags reach load-failed through the library's own handler (uc_webview.c).
 *  - A host that never answers: before a main-frame load the UI process asks the network process to preconnect
 *    (WebPageProxy.cpp:2343, 8190-8207; ENABLE_SERVER_PRECONNECT is on for GLib ports, PlatformEnableGlib.h:58-59).
 *    The PreconnectTask has a 10 s timeout (NetworkProcess.cpp:1747-1770) and cancels its load when it fires
 *    (PreconnectTask.cpp:119-124). libsoup hands the connection a preconnect is still opening to the next request
 *    for the same host (soup_session_steal_preconnection(), soup-session.c:1661-1682 in libsoup 3.4.4), so the main
 *    request fails 10 s after STARTED with g-io-error-quark G_IO_ERROR_CANCELLED "Operation was cancelled" (seen on
 *    WebKitGTK 2.54.0 with libsoup 3.6.5, with and without process swaps). That is TIMEOUT here; a load WebKit
 *    itself cancels is WEBKIT_NETWORK_ERROR_CANCELLED (WebKitWebView.cpp:2728, WebKitPrivate.cpp:138-139), which
 *    never reaches the mapping (uc_webview.c reports no FAILED for it).
 *  - Otherwise libsoup has no I/O timeout in WebKit's session ("timeout" 0, SoupNetworkSession.cpp:116-118): a
 *    connection whose SYN the kernel gives up on is g-io-error-quark G_IO_ERROR_TIMED_OUT. WebKit's own request
 *    timeout (ResourceErrorSoup.cpp:109-118, NetworkDataTaskSoup.cpp:324-340) is domain "WebKitNetworkError"
 *    (WEBKIT_NETWORK_ERROR, APIError.cpp:40-48, WebKitError.cpp:36-39), code -1001; it runs only for a request with
 *    a timeout interval (NetworkDataTaskSoup.cpp:336-340), which uc_webview_load_uri() does not set.
 * Domains are compared as strings (g_quark_to_string() of the GError's domain), never the message text.
 */
#ifndef UC_LOAD_ERROR_H
#define UC_LOAD_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

/* WEBKIT_NETWORK_ERROR's domain string (APIError.cpp:40-48) and the code of WebKit's request timeout
 * (ResourceErrorSoup.cpp:114-117, NSURLErrorTimedOut). */
#define UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN "WebKitNetworkError"
#define UC_LOAD_ERROR_WEBKIT_TIMEOUT        (-1001)

/* The UC_WEBVIEW_FAIL_* reason for a failure with this GError domain string (NULL = none) and code; tls_errors are
 * the GTlsCertificateFlags of load-failed-with-tls-errors (0 when there was none), any bit set means
 * UC_WEBVIEW_FAIL_CERTIFICATE. */
int  uc_load_error_reason (const char *domain, int code, unsigned tls_errors);

/* The arguments of load_failed_detail() for a load-failed GError (NULL allowed: OTHER, "", 0, ""); the strings point
 * into error or are static, valid as long as error is. tls_errors is kept only for UC_WEBVIEW_FAIL_CERTIFICATE. */
typedef struct {
    int reason;
    const char *domain;
    int code;
    unsigned tls_errors;
    const char *message;
} UcLoadFailure;

void uc_load_failure_from_error (UcLoadFailure *failure, const GError *error, unsigned tls_errors);
/* A URI the scheme allowlist refused (uc_uri_policy.h): UC_WEBVIEW_FAIL_SCHEME_REFUSED, "", 0, 0, "". */
void uc_load_failure_scheme_refused (UcLoadFailure *failure);

/*
 * A main-frame load that took over the connection of a preconnect which failed on the server's certificate (library
 * 0.10.3). WebKit's preconnect (above) gets the TLS check first when the web process' request comes late: its
 * accept-certificate handler rejects a certificate the session does not accept and cancels the preconnect's own
 * GCancellable (NetworkDataTaskSoup.cpp:159-162, 502-515, 300-310 in WebKit 2.54.2 and 2.54.3). libsoup still hands
 * the connection to the main request until the connect operation has completed: it checks only that the preconnect is
 * in CONNECTING (soup_session_steal_preconnection(), soup-session.c:1741-1764 in libsoup 3.8.1), and the connect runs
 * with the preconnect's GCancellable (soup-session.c:1794-1799, soup-connection.c:808-833), whose cancellation the
 * completion hands to the main request (connect_async_complete(), soup-session.c:1709-1738; GTask returns the
 * cancellation first). So the load fails with g-io-error-quark G_IO_ERROR_CANCELLED a moment after STARTED, without
 * load-failed-with-tls-errors: the main request never saw the certificate, no certificate exception can apply, and
 * the reason would be TIMEOUT. Another load of the URI checks the certificate itself.
 * TRUE when a load-failed is that: the URI is https, the load was provisional (WEBKIT_LOAD_STARTED), the error
 * domain string and code are G_IO_ERROR_CANCELLED, and it came less than UC_LOAD_ERROR_CANCELLED_EARLY_US after
 * STARTED (since_started_us; negative: unknown, FALSE). A host that never answers is cancelled by the preconnect's
 * 10 s timeout (NetworkProcess.cpp:1759-1770) and stays a failure (TIMEOUT).
 */
#define UC_LOAD_ERROR_CANCELLED_EARLY_US (5 * G_USEC_PER_SEC)
gboolean uc_load_error_cancelled_early (const char *uri, const char *domain, int code, gboolean provisional,
                                        gint64 since_started_us);

/* The loop guard of those repeated loads: a URI is loaded again at most UC_LOAD_RETRIES_MAX times within
 * UC_LOAD_RETRIES_WINDOW_US of the first repeat; then the failure goes to the host as it is. Zero-initialised. */
#define UC_LOAD_RETRIES_MAX       2
#define UC_LOAD_RETRIES_WINDOW_US (10 * G_USEC_PER_SEC)
typedef struct {
    char *uri;
    gint64 first_us;
    guint count;
} UcLoadRetries;

/* TRUE (and counted): uri may be loaded again at now_us (g_get_monotonic_time()). */
gboolean uc_load_retries_take  (UcLoadRetries *retries, const char *uri, gint64 now_us);
void     uc_load_retries_clear (UcLoadRetries *retries);

G_END_DECLS

#endif /* UC_LOAD_ERROR_H */
