/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * URI scheme allowlist of libucwebview: which URIs a host may pass to uc_webview_load_uri() and use as the base
 * URI of uc_webview_load_html(). GLib only, so that the rules are unit-testable on a host without WPE.
 *
 *   http, https, about, data   always allowed
 *   file                       only with UC_WEBVIEW_ALLOW_FILE_URLS=1 (read once by uc_webview_global_init();
 *                              production default: blocked)
 *   anything else              rejected: other schemes (javascript:, blob:, ftp:, custom ones) and strings
 *                              without a scheme (bare paths, relative references, "localhost:8080" parses as
 *                              the scheme "localhost")
 *
 * These API loads are the only way to a file: document. Web content cannot navigate there by itself: WebCore
 * refuses page-initiated loads of local URLs from http(s), data and about documents
 * (FrameLoader::loadFrameRequest, FrameLoader.cpp:1513-1516, SecurityOrigin::canDisplay with
 * restrictAccessToLocal), while the API path (FrameLoader::load(FrameLoadRequest&&)) does not check it and WPE
 * grants every API file: load universal read access (sandbox extensions are compiled out,
 * WebKitFeatures.cmake:278, SandboxExtension.h:165, WebPageProxy.cpp:2138-2146).
 */
#ifndef UC_URI_POLICY_H
#define UC_URI_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    UC_URI_ALLOWED = 0,
    UC_URI_REJECTED_FILE,     /* file: while file URLs are blocked */
    UC_URI_REJECTED_SCHEME,   /* a scheme outside the allowlist */
    UC_URI_REJECTED_INVALID,  /* no scheme, or not a URI at all */
} UcUriVerdict;

/* The verdict for a URI given to uc_webview_load_uri(); NULL and "" are invalid. */
UcUriVerdict uc_uri_policy_check      (const char *uri, gboolean allow_file);
/* The verdict for the base URI of uc_webview_load_html(): NULL and "" stand for about:blank and are allowed. */
UcUriVerdict uc_uri_policy_check_base (const char *base_uri, gboolean allow_file);
/* A short reason for the log, never NULL. */
const char  *uc_uri_policy_describe   (UcUriVerdict verdict);
/* The URI for a log line, at most 200 bytes, never with user info (credentials). Unless full is TRUE
 * (UC_WEBVIEW_LOG_FULL_URLS=1) only the origin stays (library 0.10.1): "<scheme>://<host>[:<port>]", the port only
 * when the URI names one, an IP literal host in brackets ("http://[::1]:8080"), never a path, query or fragment
 * (paths carry reset tokens and ids as well as queries and fragments do); "<scheme>:" for a URI without a host
 * ("about:", "data:", "file:", "javascript:", "blob:", "mailto:") and for one that does not parse, "(no scheme)"
 * for a string without a scheme. full keeps path, query, fragment and payloads (not the user info). Free with
 * g_free(). */
char        *uc_uri_policy_loggable   (const char *uri, gboolean full);
/* A message for a log line (a GError message of WebKit, libsoup or GIO, a JavaScript exception) with every URI in it
 * replaced by uc_uri_policy_loggable(uri, full) (library 0.10.1). A URI starts at a scheme followed by "://", or at
 * about:, blob:, data:, file:, javascript: or mailto:, and ends at white space, a quote or an angle bracket; trailing
 * sentence punctuation stays outside. Host names alone ("Error resolving “example.com”") stay. NULL gives
 * "(null)". Free with g_free(). */
char        *uc_uri_policy_scrub_text (const char *text, gboolean full);

/* The library's boolean environment switches: "1", "true", "yes", "on" (case-insensitive) are true, anything
 * else (including NULL) false. */
gboolean     uc_env_value_true        (const char *value);

G_END_DECLS

#endif /* UC_URI_POLICY_H */
