/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Site settings of library 0.8.0 that need no WebKit: the checks and tables behind uc_webview_global_set_cookies(),
 * uc_webview_global_allow_certificate(), uc_webview_set_user_agent() and uc_webview_global_clear_data(). GLib only,
 * unit-tested on a host without WPE (tests/test-site.c); the WebKit side is uc_site_wk.c.
 */
#ifndef UC_SITE_H
#define UC_SITE_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ---- hosts and certificates (uc_webview_global_allow_certificate()) --------------------------------------------- */

#define UC_SITE_TLS_MAX_HOSTS        64
#define UC_SITE_TLS_MAX_FINGERPRINTS 16     /* per host */

/* The host as WebKit compares it for its certificate exceptions (SoupNetworkSession::checkTLSErrors() looks the
 * request URL's host up case-insensitively, without the brackets of an IPv6 address, SoupNetworkSession.cpp:321-344):
 * lower case, ASCII (an IDN in punycode), an IPv6 address without brackets. NULL for "", a port, a path, user info,
 * white space or anything else that is not a host name or an IP address. Free with g_free(). */
char     *uc_site_host_normalize        (const char *host);
/* A SHA-256 fingerprint as 64 lower-case hex digits: upper case and ':' between the bytes (as openssl prints it)
 * are accepted. NULL when it is not 32 bytes in hex. Free with g_free(). */
char     *uc_site_fingerprint_normalize (const char *sha256);

typedef struct _UcTlsExceptions UcTlsExceptions;

UcTlsExceptions *uc_tls_exceptions_new   (void);
void             uc_tls_exceptions_free  (UcTlsExceptions *tls);
/* Adds an exception for host: the certificate with this fingerprint, or any certificate (sha256 NULL). Returns 0,
 * also when it was there already, -1 for an invalid host or fingerprint or a full table (*reason says which). */
int              uc_tls_exceptions_add   (UcTlsExceptions *tls, const char *host, const char *sha256,
                                          const char **reason);
/* Whether a certificate with the fingerprint cert_sha256 (64 lower-case hex digits) of host has an exception; *any
 * is TRUE when it is the "any certificate" one. host is normalized here. */
gboolean         uc_tls_exceptions_match (const UcTlsExceptions *tls, const char *host, const char *cert_sha256,
                                          gboolean *any);
/* Hosts with an exception (for the log line). */
guint            uc_tls_exceptions_size  (const UcTlsExceptions *tls);

/* ---- cookies (uc_webview_global_set_cookies()) -------------------------------------------------------------- */

#define UC_SITE_COOKIE_MAX_COUNT   64       /* per call */
#define UC_SITE_COOKIE_MAX_NAME_BYTES  256    /* the Core-API spec's limits of cookies[].name and .value */
#define UC_SITE_COOKIE_MAX_VALUE_BYTES 4096
#define UC_SITE_COOKIE_MAX_EXPIRES G_GINT64_CONSTANT (253402300799)  /* 9999-12-31T23:59:59Z */

/* A checked cookie, in the form SoupCookie wants it. */
typedef struct {
    char *name, *value;
    char *domain;       /* the URL's host (a host-only cookie) or ".example.com" (the domain and its subdomains) */
    char *path;
    gboolean secure, http_only;
    gint64 expires;     /* seconds since 1970 UTC, -1 for a session cookie */
} UcCookie;

/* Checks one cookie of uc_webview_global_set_cookies() for url (http or https) and fills out (free its strings
 * with uc_site_cookie_clear()):
 *  - name: an RFC 6265 token of 1..256 bytes; value: at most 4096 RFC 6265 cookie-octets (visible ASCII without
 *    '"', ',', ';' and '\\'), optionally in double quotes, "" is valid (library 0.8.3; before, any byte but control
 *    characters and ';', and WebKit dropped a cookie with invalid UTF-8 silently);
 *  - domain NULL or "": a host-only cookie of url's host; else url's host or a domain it is in ("example.com" and
 *    ".example.com" both mean the domain and its subdomains, as the Domain attribute does), an IP address only as
 *    itself (then host-only);
 *  - path NULL or "": "/"; else it starts with '/', no control characters or ';';
 *  - expires -1 (session) or 0 .. UC_SITE_COOKIE_MAX_EXPIRES (a time in the past deletes the cookie).
 * Returns FALSE with *reason (static text, never the value) for a cookie the library refuses. */
gboolean uc_site_cookie_check (const char *url, const char *name, const char *value, const char *domain,
                               const char *path, int secure, int http_only, int64_t expires, UcCookie *out,
                               const char **reason);
void     uc_site_cookie_clear (UcCookie *cookie);

/* Makes path a file only its owner may read and write (library 0.8.3): a missing file is created empty with mode 0600
 * (SQLite takes an empty file as a new database, and gives its journals, path-journal, -wal and -shm, the mode of the
 * database: SQLite's os_unix.c findCreateFileMode()), an existing one and its journals get 0600 (files of 0.8.0..0.8.2
 * were 0644, inside a 0700 directory). A symbolic link is not followed. FALSE when it failed. */
gboolean uc_site_private_file (const char *path);

/* ---- user agent (uc_webview_set_user_agent()) ---------------------------------------------------------------- */

/* The preset values, equal to UC_WEBVIEW_USER_AGENT_* (static asserts in uc_webview.c) */
#define UC_SITE_USER_AGENT_DEFAULT 0
#define UC_SITE_USER_AGENT_MOBILE  1
#define UC_SITE_USER_AGENT_DESKTOP 2
#define UC_SITE_USER_AGENT_CUSTOM  3
#define UC_SITE_USER_AGENT_MAX_BYTES 512

/* The user agent string of a preset: *ua NULL for DEFAULT (the library's default, uc_site_default_user_agent()), the
 * preset's text for MOBILE and DESKTOP (custom ignored), custom for CUSTOM. FALSE (with *reason) for an unknown
 * preset or a CUSTOM text that is empty, longer than UC_SITE_USER_AGENT_MAX_BYTES or has a byte outside visible ASCII
 * and space. */
gboolean    uc_site_user_agent      (int preset, const char *custom, const char **ua, const char **reason);
const char *uc_site_user_agent_name (int preset);

/* ---- the Remote in the default user agent (library 0.8.2) ------------------------------------------------- */

/* The product token the DEFAULT user agent ends with: "<WebKit's user agent> UCRemote/<firmware version>", or
 * "... UCRemote" when no version is known. One word, no spaces: an RFC 9110 product token. */
#define UC_SITE_UA_PRODUCT        "UCRemote"
/* The firmware's release file, UC_OS_VERSION=<version> (the firmware build writes it). The
 * remote-ui units load it with EnvironmentFile=, outside their sandbox, which hides the file itself. */
#define UC_SITE_RELEASE_FILE      "/etc/uc-release"
#define UC_SITE_VERSION_MAX_BYTES 64

/* A version for the product token: 1..64 tchar bytes of RFC 9110 ("2.13.2", "2.13.2-dev.3+5bf0cf38"). */
gboolean uc_site_version_valid       (const char *version);
/* UC_OS_VERSION of the text of /etc/uc-release (KEY=VALUE lines, # comments; a value in single or double quotes is
 * unquoted; CR LF line ends are accepted). NULL without a valid one. Free with g_free(). */
char    *uc_site_release_version     (const char *contents);
/* The firmware version: UC_OS_VERSION of the file at path when it is readable, else env_version (the environment's
 * UC_OS_VERSION, which the remote-ui units load from the same file), else NULL. *source (may be NULL) names the
 * source for the log, static text, or why there is none. Free with g_free(). */
char    *uc_site_firmware_version    (const char *path, const char *env_version, const char **source);
/* "<webkit_ua> UCRemote/<version>", or "<webkit_ua> UCRemote" for version NULL or invalid. Free with g_free(). */
char    *uc_site_default_user_agent  (const char *webkit_ua, const char *version);

/* ---- website data (uc_webview_global_clear_data()) ----------------------------------------------------------- */

/* equal to UC_WEBVIEW_DATA_* */
#define UC_SITE_DATA_CACHE 0x1u
#define UC_SITE_DATA_ALL   0xffffffffu

/* CACHE or ALL; every other value (unknown bits a newer host may pass) is refused. */
gboolean uc_site_clear_types_valid (unsigned types);

G_END_DECLS

#endif /* UC_SITE_H */
