/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * URI scheme allowlist of libucwebview, see uc_uri_policy.h.
 */
#include "uc_uri_policy.h"

#include <string.h>

#define UC_LOGGABLE_MAX 200

static const char *const always_allowed[] = { "http", "https", "about", "data" };

UcUriVerdict
uc_uri_policy_check (const char *uri, gboolean allow_file)
{
    /* RFC 3986 scheme, lower-cased; NULL when the string does not start with one (GLib >= 2.66) */
    const char *scheme = uri != NULL ? g_uri_peek_scheme (uri) : NULL;

    if (scheme == NULL)
        return UC_URI_REJECTED_INVALID;
    for (gsize i = 0; i < G_N_ELEMENTS (always_allowed); i++)
        if (strcmp (scheme, always_allowed[i]) == 0)
            return UC_URI_ALLOWED;
    if (strcmp (scheme, "file") == 0)
        return allow_file ? UC_URI_ALLOWED : UC_URI_REJECTED_FILE;
    return UC_URI_REJECTED_SCHEME;
}

UcUriVerdict
uc_uri_policy_check_base (const char *base_uri, gboolean allow_file)
{
    if (base_uri == NULL || *base_uri == '\0')
        return UC_URI_ALLOWED; /* webkit_web_view_load_html() uses about:blank */
    return uc_uri_policy_check (base_uri, allow_file);
}

const char *
uc_uri_policy_describe (UcUriVerdict verdict)
{
    switch (verdict) {
    case UC_URI_ALLOWED:
        return "allowed";
    case UC_URI_REJECTED_FILE:
        return "file: URLs are disabled (UC_WEBVIEW_ALLOW_FILE_URLS=1 enables them)";
    case UC_URI_REJECTED_SCHEME:
        return "scheme not allowed (http, https, about, data; file with UC_WEBVIEW_ALLOW_FILE_URLS=1)";
    case UC_URI_REJECTED_INVALID:
    default:
        return "not an absolute URI";
    }
}

static char *
cut_loggable (char *text)
{
    if (strlen (text) > UC_LOGGABLE_MAX) {
        /* cut on a UTF-8 character boundary */
        const char *end = g_utf8_find_prev_char (text, text + UC_LOGGABLE_MAX + 1);
        char *cut = g_strdup_printf ("%.*s...", (int) ((end != NULL ? end : text + UC_LOGGABLE_MAX) - text), text);
        g_free (text);
        text = cut;
    }
    return text;
}

/* Every log line with a URI goes through here (uc_uri_policy.h has the rules). */
char *
uc_uri_policy_loggable (const char *uri, gboolean full)
{
    const char *scheme;
    const char *host;
    GUri *parsed;
    char *text;

    if (uri == NULL)
        return g_strdup ("(null)");
    parsed = g_uri_parse (uri, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_ENCODED, NULL);
    if (parsed == NULL) {
        scheme = g_uri_peek_scheme (uri);
        return cut_loggable (scheme != NULL ? g_strdup_printf ("%s:", scheme) : g_strdup ("(no scheme)"));
    }
    scheme = g_uri_get_scheme (parsed);
    host = g_uri_get_host (parsed);
    if (full) {
        text = g_uri_to_string_partial (parsed, G_URI_HIDE_USERINFO);
    } else if (host != NULL && *host != '\0') {
        /* the origin of the URI: GUri keeps an IP literal without its brackets, and the port is -1 unless the URI
         * names one (no G_URI_FLAGS_SCHEME_NORMALIZE) */
        gboolean ip_literal = strchr (host, ':') != NULL;
        int port = g_uri_get_port (parsed);
        char port_text[16] = "";

        if (port >= 0)
            g_snprintf (port_text, sizeof port_text, ":%d", port);
        text = g_strdup_printf ("%s://%s%s%s%s", scheme, ip_literal ? "[" : "", host, ip_literal ? "]" : "",
                                port_text);
    } else {
        /* about:blank, data:, file:, javascript:, blob:, mailto: */
        text = g_strdup_printf ("%s:", scheme);
    }
    g_uri_unref (parsed);
    return cut_loggable (text);
}

/* RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static gboolean
is_scheme_char (char c)
{
    return g_ascii_isalnum (c) || c == '+' || c == '-' || c == '.';
}

/* schemes whose URIs have no "//" but still name a resource or carry content */
static const char *const opaque_schemes[] = { "about", "blob", "data", "file", "javascript", "mailto" };

/* The length of the scheme that starts a URI at text (0 for none): a scheme followed by "://", or one of the opaque
 * schemes followed by ':' and at least one character that belongs to the URI. Right after an opening quote “ any
 * scheme with ':' starts one: libsoup quotes a redirect target that way, whatever its scheme ("Invalid URI “%s” in
 * Location response header", soup-session.c:1147 in libsoup 3.4.4). */
static gsize
uri_start (const char *text, gboolean quoted)
{
    gsize n = 0;

    if (!g_ascii_isalpha (text[0]))
        return 0;
    while (is_scheme_char (text[n]))
        n++;
    if (text[n] != ':')
        return 0;
    if (text[n + 1] == '/' && text[n + 2] == '/')
        return n;
    if (quoted)
        return text[n + 1] != '\0' && !g_ascii_isspace (text[n + 1]) ? n : 0;
    for (gsize i = 0; i < G_N_ELEMENTS (opaque_schemes); i++)
        if (strlen (opaque_schemes[i]) == n && g_ascii_strncasecmp (text, opaque_schemes[i], n) == 0)
            return text[n + 1] != '\0' && !g_ascii_isspace (text[n + 1]) ? n : 0;
    return 0;
}

/* Where a URI in a message ends: at white space, an ASCII quote or angle bracket, or the typographic quotes GLib and
 * libsoup put around names (“ ” ‘ ’ « »). The payload of a data: or javascript: URI is page content that may hold
 * any of these, so such a URI ends at white space only (an encoded URI has none). */
static gsize
uri_length (const char *text, gsize scheme_length)
{
    static const char *const quotes[] = { "\xe2\x80\x9c", "\xe2\x80\x9d", "\xe2\x80\x98", "\xe2\x80\x99",
                                          "\xc2\xab", "\xc2\xbb" };
    gboolean payload = (scheme_length == 4 && g_ascii_strncasecmp (text, "data", 4) == 0)
                       || (scheme_length == 10 && g_ascii_strncasecmp (text, "javascript", 10) == 0);
    gsize n = 0;

    for (;;) {
        char c = text[n];
        gboolean quote = FALSE;

        if (c == '\0' || g_ascii_isspace (c))
            break;
        if (!payload) {
            if (c == '"' || c == '\'' || c == '<' || c == '>' || c == '`')
                break;
            for (gsize i = 0; i < G_N_ELEMENTS (quotes); i++)
                if (g_str_has_prefix (text + n, quotes[i]))
                    quote = TRUE;
            if (quote)
                break;
        }
        n++;
    }
    /* punctuation of the sentence around the URI: "... URI http://h/p: reason", "(see http://h/p)." (a payload
     * keeps its last characters) */
    while (!payload && n > scheme_length + 1 && strchr (".,:;!?)]}", text[n - 1]) != NULL)
        n--;
    return n;
}

char *
uc_uri_policy_scrub_text (const char *text, gboolean full)
{
    GString *out;
    const char *p;

    if (text == NULL)
        return g_strdup ("(null)");
    out = g_string_sized_new (strlen (text));
    p = text;
    while (*p != '\0') {
        gboolean quoted = p - text >= 3 && memcmp (p - 3, "\xe2\x80\x9c", 3) == 0;
        gsize scheme_length = p == text || !is_scheme_char (p[-1]) ? uri_start (p, quoted) : 0;
        gsize length = scheme_length > 0 ? uri_length (p, scheme_length) : 0;

        if (length > 0) {
            char *uri = g_strndup (p, length);
            char *loggable = uc_uri_policy_loggable (uri, full);
            g_string_append (out, loggable);
            g_free (loggable);
            g_free (uri);
            p += length;
        } else {
            g_string_append_c (out, *p);
            p++;
        }
    }
    return g_string_free (out, FALSE);
}

gboolean
uc_env_value_true (const char *value)
{
    return value != NULL && (g_str_equal (value, "1") || g_ascii_strcasecmp (value, "true") == 0
                             || g_ascii_strcasecmp (value, "yes") == 0 || g_ascii_strcasecmp (value, "on") == 0);
}
