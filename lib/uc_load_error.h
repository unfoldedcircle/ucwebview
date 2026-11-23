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

G_END_DECLS

#endif /* UC_LOAD_ERROR_H */
