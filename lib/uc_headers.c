/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Origin headers (library 0.8.0), see uc_headers.h. GLib only; compiled into the library and into its web process
 * extension.
 */
#include "uc_headers.h"

#include <string.h>

typedef struct {
    UcHeaderList list;
    guint64 seq;
} Entry;

struct _UcHeadersTable {
    GHashTable *entries;    /* origin (owned) -> Entry* */
};

/* ---- origins ----------------------------------------------------------------------------------------------- */

char *
uc_headers_origin (const char *uri)
{
    GUri *parsed;
    const char *scheme, *host;
    char *ascii = NULL, *lower, *origin = NULL;
    int port, default_port;

    if (uri == NULL || *uri == '\0')
        return NULL;
    parsed = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
    if (parsed == NULL)
        return NULL;
    scheme = g_uri_get_scheme (parsed);
    host = g_uri_get_host (parsed);
    if (g_ascii_strcasecmp (scheme, "http") == 0)
        default_port = 80;
    else if (g_ascii_strcasecmp (scheme, "https") == 0)
        default_port = 443;
    else
        default_port = 0;
    if (default_port == 0 || host == NULL || *host == '\0') {
        g_uri_unref (parsed);
        return NULL;
    }
    /* WebKit's URLs carry the host in ASCII (punycode) and lower case */
    if (!g_hostname_is_ip_address (host)) {
        ascii = g_hostname_to_ascii (host);
        if (ascii == NULL) {
            g_uri_unref (parsed);
            return NULL;
        }
        host = ascii;
    }
    lower = g_ascii_strdown (host, -1);
    port = g_uri_get_port (parsed);
    {
        gboolean ipv6 = strchr (lower, ':') != NULL;
        char *scheme_lower = g_ascii_strdown (scheme, -1);
        if (port > 0 && port != default_port)
            origin = g_strdup_printf ("%s://%s%s%s:%d", scheme_lower, ipv6 ? "[" : "", lower, ipv6 ? "]" : "", port);
        else
            origin = g_strdup_printf ("%s://%s%s%s", scheme_lower, ipv6 ? "[" : "", lower, ipv6 ? "]" : "");
        g_free (scheme_lower);
    }
    g_free (lower);
    g_free (ascii);
    g_uri_unref (parsed);
    return origin;
}

/* ---- names and values -------------------------------------------------------------------------------------- */

/* tchar of RFC 9110, section 5.6.2 */
static gboolean
is_tchar (char c)
{
    return g_ascii_isalnum (c) || (c != '\0' && strchr ("!#$%&'*+-.^_`|~", c) != NULL);
}

/* Headers that frame the message or that libsoup and WebKit set themselves: adding them breaks the request, fights
 * the cookie jar (use uc_webview_global_set_cookies() for cookies) or the user agent setting, or claims something
 * about the request's initiator (Origin, Referer, the Fetch metadata Sec-*). The Core-API spec forbids Host, Cookie,
 * Content-Length, Content-Type, Connection, Transfer-Encoding, Upgrade, Origin, Referer, Proxy-* and Sec-*
 * (the Core-API's web view entity, auth.header_name); the library refuses those and a few more. */
static const char *const reserved_names[] = {
    "host", "content-length", "content-type", "transfer-encoding", "connection", "keep-alive", "upgrade", "te",
    "trailer", "expect", "cookie", "cookie2", "origin", "referer", "user-agent",
};
static const char *const reserved_prefixes[] = { "proxy-", "sec-" };

gboolean
uc_headers_name_valid (const char *name)
{
    gsize len;

    if (name == NULL || *name == '\0')
        return FALSE;
    len = strlen (name);
    if (len > UC_HEADERS_MAX_NAME_BYTES)
        return FALSE;
    for (const char *p = name; *p != '\0'; p++)
        if (!is_tchar (*p))
            return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS (reserved_names); i++)
        if (g_ascii_strcasecmp (name, reserved_names[i]) == 0)
            return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS (reserved_prefixes); i++)
        if (g_ascii_strncasecmp (name, reserved_prefixes[i], strlen (reserved_prefixes[i])) == 0)
            return FALSE;
    return TRUE;
}

gboolean
uc_headers_value_valid (const char *value)
{
    gsize len;

    if (value == NULL)
        return FALSE;
    len = strlen (value);
    if (len > UC_HEADERS_MAX_VALUE_BYTES)
        return FALSE;
    for (const char *p = value; *p != '\0'; p++) {
        unsigned char c = (unsigned char) *p;
        if (c != '\t' && (c < 0x20 || c > 0x7e))
            return FALSE;
    }
    if (len > 0 && (value[0] == ' ' || value[0] == '\t' || value[len - 1] == ' ' || value[len - 1] == '\t'))
        return FALSE;
    return TRUE;
}

gboolean
uc_headers_list_valid (const char *const *names, const char *const *values, gsize n, const char **reason)
{
    const char *dummy;

    if (reason == NULL)
        reason = &dummy;
    *reason = NULL;
    if (n == 0)
        return TRUE;
    if (n > UC_HEADERS_MAX_COUNT) {
        *reason = "more than 32 headers";
        return FALSE;
    }
    if (names == NULL || values == NULL) {
        *reason = "names or values NULL";
        return FALSE;
    }
    for (gsize i = 0; i < n; i++) {
        if (!uc_headers_name_valid (names[i])) {
            *reason = "a header name that is not a token of at most 64 bytes or that the library does not add (Host, "
                      "Cookie, Content-Length, Content-Type, Origin, Referer, User-Agent, Proxy-*, Sec-*, ...)";
            return FALSE;
        }
        if (!uc_headers_value_valid (values[i])) {
            *reason = "a header value with control characters, non-ASCII bytes, white space at its ends or more "
                      "than 8192 bytes";
            return FALSE;
        }
        for (gsize j = 0; j < i; j++)
            if (g_ascii_strcasecmp (names[i], names[j]) == 0) {
                *reason = "a header name given twice";
                return FALSE;
            }
    }
    return TRUE;
}

/* ---- the table ---------------------------------------------------------------------------------------------- */

/* The values are the secrets: overwritten before they are freed (best effort; GVariant and WebKit copies stay). */
static void
wipe_strv (char **values)
{
    for (char **v = values; v != NULL && *v != NULL; v++)
        explicit_bzero (*v, strlen (*v));
    g_strfreev (values);
}

static void
entry_free (gpointer data)
{
    Entry *entry = data;
    g_strfreev (entry->list.names);
    wipe_strv (entry->list.values);
    g_free (entry);
}

static Entry *
entry_new (const char *const *names, const char *const *values, guint n, guint64 seq)
{
    Entry *entry = g_new0 (Entry, 1);

    entry->list.n = n;
    entry->list.names = g_new0 (char *, n + 1);
    entry->list.values = g_new0 (char *, n + 1);
    for (guint i = 0; i < n; i++) {
        entry->list.names[i] = g_strdup (names[i]);
        entry->list.values[i] = g_strdup (values[i]);
    }
    entry->seq = seq;
    return entry;
}

UcHeadersTable *
uc_headers_table_new (void)
{
    UcHeadersTable *table = g_new0 (UcHeadersTable, 1);
    table->entries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, entry_free);
    return table;
}

void
uc_headers_table_free (UcHeadersTable *table)
{
    if (table == NULL)
        return;
    g_hash_table_unref (table->entries);
    g_free (table);
}

guint
uc_headers_table_size (const UcHeadersTable *table)
{
    return g_hash_table_size (table->entries);
}

void
uc_headers_table_clear (UcHeadersTable *table)
{
    g_hash_table_remove_all (table->entries);
}

void
uc_headers_table_set (UcHeadersTable *table, const char *origin, const char *const *names, const char *const *values,
                      guint n, guint64 seq)
{
    g_return_if_fail (origin != NULL);
    if (n == 0) {
        g_hash_table_remove (table->entries, origin);
        return;
    }
    g_hash_table_replace (table->entries, g_strdup (origin), entry_new (names, values, n, seq));
}

const UcHeaderList *
uc_headers_table_lookup (const UcHeadersTable *table, const char *origin)
{
    Entry *entry;

    if (table == NULL || origin == NULL)
        return NULL;
    entry = g_hash_table_lookup (table->entries, origin);
    return entry != NULL ? &entry->list : NULL;
}

void
uc_headers_table_merge (UcHeadersTable *dst, const UcHeadersTable *src)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, src->entries);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        const Entry *entry = value;
        const Entry *have = g_hash_table_lookup (dst->entries, key);
        if (have != NULL && have->seq >= entry->seq)
            continue;
        g_hash_table_replace (dst->entries, g_strdup (key),
                              entry_new ((const char *const *) entry->list.names,
                                         (const char *const *) entry->list.values, entry->list.n, entry->seq));
    }
}

GVariant *
uc_headers_table_serialize (const UcHeadersTable *table, guint32 generation)
{
    GVariantBuilder origins;
    GHashTableIter iter;
    gpointer key, value;

    g_variant_builder_init (&origins, G_VARIANT_TYPE ("a{sa(ss)}"));
    g_hash_table_iter_init (&iter, table->entries);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        const Entry *entry = value;
        GVariantBuilder headers;

        g_variant_builder_init (&headers, G_VARIANT_TYPE ("a(ss)"));
        for (guint i = 0; i < entry->list.n; i++)
            g_variant_builder_add (&headers, "(ss)", entry->list.names[i], entry->list.values[i]);
        g_variant_builder_add (&origins, "{sa(ss)}", (const char *) key, &headers);
    }
    return g_variant_new ("(ua{sa(ss)})", generation, &origins);
}

gboolean
uc_headers_table_load (UcHeadersTable *table, GVariant *variant, guint32 *generation)
{
    GVariantIter *origins;
    const char *origin;
    GVariantIter *headers;
    guint32 incoming;

    if (variant == NULL || !g_variant_is_of_type (variant, G_VARIANT_TYPE (UC_HEADERS_VARIANT_TYPE)))
        return FALSE;
    g_variant_get (variant, "(ua{sa(ss)})", &incoming, &origins);
    if (*generation != 0 && incoming <= *generation) {
        g_variant_iter_free (origins);
        return FALSE;
    }
    *generation = incoming;
    uc_headers_table_clear (table);
    while (g_variant_iter_next (origins, "{&sa(ss)}", &origin, &headers)) {
        GPtrArray *names = g_ptr_array_new (), *values = g_ptr_array_new ();
        const char *name, *val;

        while (g_variant_iter_next (headers, "(&s&s)", &name, &val)) {
            g_ptr_array_add (names, (gpointer) name);
            g_ptr_array_add (values, (gpointer) val);
        }
        /* defence in depth: only the library sends this message, and it checked everything; an entry that is not a
         * normalized origin with valid headers is skipped all the same */
        {
            char *normalized = uc_headers_origin (origin);
            gboolean valid = normalized != NULL && strcmp (normalized, origin) == 0
                             && uc_headers_list_valid ((const char *const *) names->pdata,
                                                       (const char *const *) values->pdata, names->len, NULL);
            g_free (normalized);
            if (names->len > 0 && valid)
                uc_headers_table_set (table, origin, (const char *const *) names->pdata,
                                      (const char *const *) values->pdata, names->len, 0);
        }
        g_ptr_array_unref (names);
        g_ptr_array_unref (values);
        g_variant_iter_free (headers);
    }
    g_variant_iter_free (origins);
    return TRUE;
}

/* ---- the decision ------------------------------------------------------------------------------------------- */

/* the same document: equal up to the fragment */
static gboolean
same_document (const char *a, const char *b)
{
    gsize la, lb;

    if (a == NULL || b == NULL)
        return FALSE;
    la = strcspn (a, "#");
    lb = strcspn (b, "#");
    return la == lb && strncmp (a, b, la) == 0;
}

void
uc_headers_decide (const UcHeadersTable *table, const char *request_uri, const char *page_uri,
                   const char *redirect_from, const char *origin_header,
                   const UcHeaderList **strip, const UcHeaderList **add)
{
    char *request_origin, *from_origin = NULL;
    const UcHeaderList *entry;

    *strip = NULL;
    *add = NULL;
    if (table == NULL || g_hash_table_size (table->entries) == 0)
        return;
    request_origin = uc_headers_origin (request_uri);
    if (request_origin == NULL)
        return;
    /* redirected away from a decorated origin: what WebKit carried over goes (NetworkDataTaskSoup.cpp:788-837 keeps
     * every header but Authorization and Origin) */
    if (redirect_from != NULL) {
        from_origin = uc_headers_origin (redirect_from);
        if (from_origin != NULL && strcmp (from_origin, request_origin) != 0)
            *strip = uc_headers_table_lookup (table, from_origin);
    }
    entry = uc_headers_table_lookup (table, request_origin);
    if (entry != NULL && origin_header != NULL) {
        /* sent on behalf of another origin (a posted form, a cross-origin fetch), or of an opaque one ("null") */
        char *initiator = uc_headers_origin (origin_header);
        if (initiator == NULL || strcmp (initiator, request_origin) != 0)
            entry = NULL;
        g_free (initiator);
    }
    if (entry != NULL) {
        /* The page's own main resource: the page URI is the URI being loaded once the provisional load started
         * (WebKitWebPage.cpp:209-237). Everything else: only under a top-level document of the origin.
         * Library 0.8.3 no longer counts a request whose redirecting response has the page URI as the main resource:
         * send-request carries no frame, so an image, frame or no-cors fetch of another site's page that loads the
         * page's own URL, redirected into the origin, got the headers (a CSRF with the token). A cross-site main-frame
         * redirect into the origin still gets them on WPE: WPE swaps the web process for it
         * (WebKitWebContext.cpp:442-446, WebProcessPool.cpp:2330-2332 keeps the process only before its first commit),
         * and the new process starts with the target as page URI. In one process (a same-site redirect, or the view's
         * first navigation) the hop is judged by the previous page URI, of the other origin: no headers. */
        if (same_document (request_uri, page_uri)) {
            *add = entry;
        } else {
            char *top_origin = uc_headers_origin (page_uri);
            if (top_origin != NULL && strcmp (top_origin, request_origin) == 0)
                *add = entry;
            g_free (top_origin);
        }
    }
    g_free (from_origin);
    g_free (request_origin);
}

guint
uc_headers_apply (const UcHeadersTable *table, const char *request_uri, const char *page_uri,
                  const char *redirect_from, const UcHeaderOps *ops, gpointer data)
{
    const UcHeaderList *strip, *add;
    GHashTableIter iter;
    gpointer value;
    char *origin_header;
    guint removed = 0;

    if (table == NULL || g_hash_table_size (table->entries) == 0)
        return 0;
    origin_header = g_strdup (ops->get (data, "Origin"));
    uc_headers_decide (table, request_uri, page_uri, redirect_from, origin_header, &strip, &add);
    g_free (origin_header);
    for (guint i = 0; strip != NULL && i < strip->n; i++)
        ops->remove (data, strip->names[i]);
    /* The value rule: a configured header in a request that does not get it under the rules is a copy WebKit made,
     * whatever path it took (a redirect in another web process after a process swap, a hop the extension did not see
     * as a redirect). A page that sends exactly a configured value itself to another origin loses it too. */
    g_hash_table_iter_init (&iter, table->entries);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        const UcHeaderList *list = &((const Entry *) value)->list;
        if (list == add)
            continue;
        for (guint i = 0; i < list->n; i++) {
            const char *current = ops->get (data, list->names[i]);
            if (current != NULL && strcmp (current, list->values[i]) == 0) {
                ops->remove (data, list->names[i]);
                removed++;
            }
        }
    }
    for (guint i = 0; add != NULL && i < add->n; i++)
        ops->set (data, add->names[i], add->values[i]);
    return removed;
}
