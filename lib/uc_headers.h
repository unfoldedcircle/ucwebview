/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Origin headers (library 0.8.0, uc_webview_set_origin_headers()): extra HTTP request headers for the requests to
 * one origin, for the "bearer" and "header" authentication of a web view entity. GLib only: the library and its
 * web process extension (uc_extension.c) share this file, and the rules are unit-tested on a host without WPE
 * (tests/test-headers.c).
 *
 * Why a web process extension: WebKit 2.54's UI process API has no hook into the requests of a page.
 * webkit_web_view_load_request() with headers reaches the main resource only, and WebKit's network process copies
 * every header of a request into its redirect except Authorization and Origin (NetworkDataTaskSoup.cpp:788-837), so
 * a custom header would follow a redirect to another site. WebKitWebPage::send-request in the web process sees every
 * request of the page before it goes out, the main resource, subresources, fetch() and XMLHttpRequest, and every
 * redirect of them again with the response that redirected (WebKitWebPage.cpp:313-325, 551-585); its changes to the
 * request's SoupMessageHeaders are what goes over the network.
 *
 * The rules (uc_headers_decide()):
 *  - a request to the origin gets the headers when the page's top-level document is of that origin, or when the
 *    request is that document's own main resource (the request URI is the page URI). A page of another site that
 *    embeds or fetches something from the origin does not get them, also not through a redirect, so the headers
 *    cannot be borrowed by other sites (0.8.3: before, a redirect whose response had the page URI counted as the
 *    main resource, and an image of another site's page with its own URL, redirected into the origin, got them);
 *  - never when the request carries an Origin header of another origin (or "null"): WebKit adds it to a form posted
 *    to the origin by another site and to a cross-origin fetch() (FrameLoader::addHTTPOriginIfNeeded,
 *    CachedResourceRequest::updateForAccessControl), the requests a forged one would be (CSRF). A link or a
 *    redirect of another site to the origin is a GET without Origin and gets them, like a SameSite=Lax cookie;
 *  - a redirect away from the origin drops them again (strip), whatever WebKit carried over;
 *  - the headers replace a header of the same name the page set itself;
 *  - (library 0.8.1) every request that does not get an origin's headers under these rules loses each header whose
 *    name and value equal one of that origin's configured headers (uc_headers_apply()). WebKit carries a header into
 *    a redirect with the value it had, so a copy that leaked into a request for another site carries the configured
 *    value, whatever path it took. That path is not always a redirect the extension sees as one: WPE swaps the web
 *    process at a cross-site main-frame navigation (WebKitWebContext.cpp:442-446), also at a server redirect, and
 *    the new process loads the redirected request (with WebKit's copies of the headers,
 *    NetworkDataTaskSoup.cpp:834-838 removes only Authorization and Origin) as a fresh load
 *    (WebPageProxy.cpp:6403-6421): send-request runs there without a redirected response, with the target as page
 *    URI. 0.8.0 stripped by name at redirects the extension saw and leaked X-Uc-Token there (seen in a device test).
 * Origins are "scheme://host[:port]" of http and https, host lower case and in ASCII (IDN), the default port left
 * out, an IPv6 address in brackets: the form webkit_uri_request_get_uri() has, so that the comparison is a string
 * comparison.
 */
#ifndef UC_HEADERS_H
#define UC_HEADERS_H

#include <glib.h>

G_BEGIN_DECLS

#define UC_HEADERS_MAX_COUNT       32       /* headers per origin */
#define UC_HEADERS_MAX_VALUE_BYTES 8192     /* per value */
#define UC_HEADERS_MAX_NAME_BYTES  64       /* the Core-API spec's limit of auth.header_name */

/* The name of the message the library sends to every web process (webkit_web_context_send_message_to_all_extensions)
 * and the format of its parameters and of the extension's initialization data: (generation, origin -> headers). */
#define UC_HEADERS_MESSAGE         "ucwebview.origin-headers.1"
#define UC_HEADERS_VARIANT_TYPE    "(ua{sa(ss)})"

/* The headers of one origin: n names and values, valid as long as the table is not changed. */
typedef struct {
    guint n;
    char **names;
    char **values;
} UcHeaderList;

typedef struct _UcHeadersTable UcHeadersTable;

/* The origin of an http or https URI ("https://example.com:8443"), NULL for anything else (other schemes, no
 * host, not a URI). Accepts a bare origin as well. Free with g_free(). */
char           *uc_headers_origin           (const char *uri);
/* A header name the library adds: an RFC 9110 token of at most UC_HEADERS_MAX_NAME_BYTES, not one that frames the
 * message, that WebKit and libsoup manage or that names the initiator: Host, Content-Length, Content-Type,
 * Transfer-Encoding, Connection, Keep-Alive, Upgrade, TE, Trailer, Expect, Cookie, Cookie2, Origin, Referer,
 * User-Agent, Proxy-*, Sec-* (case-insensitive). */
gboolean        uc_headers_name_valid       (const char *name);
/* A header value: at most UC_HEADERS_MAX_VALUE_BYTES of visible ASCII, spaces and tabs; no CR, LF, other control
 * characters or bytes above 0x7e (WebKit turns the headers of a WebKitURIRequest back into UTF-16 strings, libsoup
 * sends bytes: ASCII is the same in both), no leading or trailing white space. "" is valid. */
gboolean        uc_headers_value_valid      (const char *value);
/* The names and values of a uc_webview_set_origin_headers() call: n <= UC_HEADERS_MAX_COUNT, every name and value
 * valid, no name twice (case-insensitive). On failure *reason says why (static text). */
gboolean        uc_headers_list_valid       (const char *const *names, const char *const *values, gsize n,
                                             const char **reason);

UcHeadersTable *uc_headers_table_new        (void);
void            uc_headers_table_free       (UcHeadersTable *table);
guint           uc_headers_table_size       (const UcHeadersTable *table);
void            uc_headers_table_clear      (UcHeadersTable *table);
/* Replaces the headers of origin (normalized form, uc_headers_origin()); n 0 removes them. seq orders the entries of
 * several tables (uc_headers_table_merge()): the caller's counter, the later call has the higher number. The
 * strings are copied. */
void            uc_headers_table_set        (UcHeadersTable *table, const char *origin, const char *const *names,
                                             const char *const *values, guint n, guint64 seq);
const UcHeaderList *uc_headers_table_lookup (const UcHeadersTable *table, const char *origin);
/* Copies the entries of src into dst; for an origin in both the entry with the higher seq stays. */
void            uc_headers_table_merge      (UcHeadersTable *dst, const UcHeadersTable *src);
/* The table as UC_HEADERS_VARIANT_TYPE with generation (a floating reference). */
GVariant       *uc_headers_table_serialize  (const UcHeadersTable *table, guint32 generation);
/* Replaces the table's entries with those of variant when its generation is newer than *generation (the first
 * table, *generation 0, always is) and updates *generation; an older or equal one is ignored (a broadcast that was
 * queued for a web process before it started with newer initialization data). NULL or a variant of another type
 * changes nothing and returns FALSE. An entry whose origin is not in normalized form or whose headers break the rules of
 * uc_headers_list_valid() is skipped (library 0.8.3). */
gboolean        uc_headers_table_load       (UcHeadersTable *table, GVariant *variant, guint32 *generation);

/* What to do with one request (WebKitWebPage::send-request): request_uri the request's URI, page_uri the page's
 * main-frame URI (webkit_web_page_get_uri(), during a load the URI being loaded; may be NULL), redirect_from the URI
 * of the response that redirected the request (NULL for a first request), origin_header the value of the request's
 * Origin header (NULL without one). *strip: names to remove first (the headers of the origin the request is
 * redirected away from), *add: headers to set after that; either NULL. The value rule of uc_headers_apply() comes on
 * top of this. */
void            uc_headers_decide           (const UcHeadersTable *table, const char *request_uri,
                                             const char *page_uri, const char *redirect_from,
                                             const char *origin_header,
                                             const UcHeaderList **strip, const UcHeaderList **add);

/* The request's headers, as the extension sees them (SoupMessageHeaders of the WebKitURIRequest; a fake in the unit
 * test). get returns the header's value or NULL, valid until the next remove or set. WebKit keeps one value per
 * header name (HTTPHeaderMap), so a name has at most one value. */
typedef struct {
    const char *(*get)    (gpointer data, const char *name);
    void        (*remove) (gpointer data, const char *name);
    void        (*set)    (gpointer data, const char *name, const char *value);
} UcHeaderOps;

/* Applies the table to one request: uc_headers_decide() (with the request's Origin header), its strip by name, then
 * the value rule (for every origin whose headers the request does not get, a header with the name and the exact value
 * of one of them is removed), then the headers it gets. Returns the number of headers the value rule removed: a copy
 * of a configured header in a request that is not entitled to it. */
guint           uc_headers_apply            (const UcHeadersTable *table, const char *request_uri,
                                             const char *page_uri, const char *redirect_from,
                                             const UcHeaderOps *ops, gpointer data);

G_END_DECLS

#endif /* UC_HEADERS_H */
