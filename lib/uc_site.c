/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Site settings of library 0.8.0 without WebKit, see uc_site.h.
 */
#include "uc_site.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- hosts and certificates -------------------------------------------------------------------------------- */

char *
uc_site_host_normalize (const char *host)
{
    char *bare, *ascii, *lower;
    gsize len;

    if (host == NULL || *host == '\0')
        return NULL;
    len = strlen (host);
    /* an IPv6 address in brackets, as in a URL: WebKit compares it without them (SoupNetworkSession.cpp:321-332) */
    if (host[0] == '[' && len > 2 && host[len - 1] == ']')
        bare = g_strndup (host + 1, len - 2);
    else
        bare = g_strdup (host);
    if (g_hostname_is_ip_address (bare)) {
        lower = g_ascii_strdown (bare, -1);
        g_free (bare);
        return lower;
    }
    ascii = g_hostname_to_ascii (bare);
    g_free (bare);
    if (ascii == NULL || *ascii == '\0') {
        g_free (ascii);
        return NULL;
    }
    for (const char *p = ascii; *p != '\0'; p++)
        if (!g_ascii_isalnum (*p) && *p != '-' && *p != '.' && *p != '_') {
            g_free (ascii);
            return NULL;
        }
    lower = g_ascii_strdown (ascii, -1);
    g_free (ascii);
    return lower;
}

char *
uc_site_fingerprint_normalize (const char *sha256)
{
    GString *hex;

    if (sha256 == NULL)
        return NULL;
    hex = g_string_sized_new (64);
    for (const char *p = sha256; *p != '\0'; p++) {
        if (*p == ':')
            continue;
        if (!g_ascii_isxdigit (*p) || hex->len >= 64) {
            g_string_free (hex, TRUE);
            return NULL;
        }
        g_string_append_c (hex, g_ascii_tolower (*p));
    }
    if (hex->len != 64) {
        g_string_free (hex, TRUE);
        return NULL;
    }
    return g_string_free (hex, FALSE);
}

typedef struct {
    gboolean any;           /* any certificate of the host */
    GPtrArray *sha256;      /* char*, normalized fingerprints */
} TlsHost;

struct _UcTlsExceptions {
    GHashTable *hosts;      /* normalized host -> TlsHost* */
};

static void
tls_host_free (gpointer data)
{
    TlsHost *host = data;
    g_ptr_array_unref (host->sha256);
    g_free (host);
}

UcTlsExceptions *
uc_tls_exceptions_new (void)
{
    UcTlsExceptions *tls = g_new0 (UcTlsExceptions, 1);
    tls->hosts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, tls_host_free);
    return tls;
}

void
uc_tls_exceptions_free (UcTlsExceptions *tls)
{
    if (tls == NULL)
        return;
    g_hash_table_unref (tls->hosts);
    g_free (tls);
}

guint
uc_tls_exceptions_size (const UcTlsExceptions *tls)
{
    return g_hash_table_size (tls->hosts);
}

int
uc_tls_exceptions_add (UcTlsExceptions *tls, const char *host, const char *sha256, const char **reason)
{
    const char *dummy;
    char *key, *fingerprint = NULL;
    TlsHost *entry;

    if (reason == NULL)
        reason = &dummy;
    *reason = NULL;
    key = uc_site_host_normalize (host);
    if (key == NULL) {
        *reason = "not a host name or IP address (no scheme, port or path)";
        return -1;
    }
    if (sha256 != NULL) {
        fingerprint = uc_site_fingerprint_normalize (sha256);
        if (fingerprint == NULL) {
            *reason = "the fingerprint is not 64 hex digits (SHA-256)";
            g_free (key);
            return -1;
        }
    }
    entry = g_hash_table_lookup (tls->hosts, key);
    if (entry == NULL) {
        if (g_hash_table_size (tls->hosts) >= UC_SITE_TLS_MAX_HOSTS) {
            *reason = "too many hosts with exceptions";
            g_free (key);
            g_free (fingerprint);
            return -1;
        }
        entry = g_new0 (TlsHost, 1);
        entry->sha256 = g_ptr_array_new_with_free_func (g_free);
        g_hash_table_insert (tls->hosts, key, entry);
        key = NULL;
    }
    g_free (key);
    if (fingerprint == NULL) {
        entry->any = TRUE;
        return 0;
    }
    for (guint i = 0; i < entry->sha256->len; i++)
        if (strcmp (g_ptr_array_index (entry->sha256, i), fingerprint) == 0) {
            g_free (fingerprint);
            return 0;
        }
    if (entry->sha256->len >= UC_SITE_TLS_MAX_FINGERPRINTS) {
        *reason = "too many fingerprints for the host";
        g_free (fingerprint);
        return -1;
    }
    g_ptr_array_add (entry->sha256, fingerprint);
    return 0;
}

gboolean
uc_tls_exceptions_match (const UcTlsExceptions *tls, const char *host, const char *cert_sha256, gboolean *any)
{
    char *key = uc_site_host_normalize (host);
    const TlsHost *entry = key != NULL ? g_hash_table_lookup (tls->hosts, key) : NULL;
    gboolean match = FALSE;

    if (any != NULL)
        *any = FALSE;
    g_free (key);
    if (entry == NULL)
        return FALSE;
    if (cert_sha256 != NULL)
        for (guint i = 0; i < entry->sha256->len && !match; i++)
            match = g_ascii_strcasecmp (g_ptr_array_index (entry->sha256, i), cert_sha256) == 0;
    if (!match && entry->any) {
        match = TRUE;
        if (any != NULL)
            *any = TRUE;
    }
    return match;
}

/* ---- cookies ----------------------------------------------------------------------------------------------- */

static gboolean
has_byte (const char *s, const char *set, gboolean ctl)
{
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char) *p;
        if (ctl && (c < 0x20 || c == 0x7f))
            return TRUE;
        if (strchr (set, *p) != NULL)
            return TRUE;
    }
    return FALSE;
}

/* RFC 6265 section 4.1.1: cookie-name = token (RFC 9110 tchar) */
static gboolean
cookie_name_valid (const char *name)
{
    gsize len = name != NULL ? strlen (name) : 0;

    if (len == 0 || len > UC_SITE_COOKIE_MAX_NAME_BYTES)
        return FALSE;
    for (const char *p = name; *p != '\0'; p++)
        if (!g_ascii_isalnum (*p) && strchr ("!#$%&'*+-.^_`|~", *p) == NULL)
            return FALSE;
    return TRUE;
}

/* RFC 6265 section 4.1.1: cookie-value = *cookie-octet / ( DQUOTE *cookie-octet DQUOTE ), cookie-octet = %x21 /
 * %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E: visible ASCII without DQUOTE, comma, semicolon and backslash. Bytes above
 * 0x7e also matter for WebKit: it makes a WebCore::Cookie with String::fromUTF8() (CookieSoup.cpp:65-69), invalid
 * UTF-8 gives a null string, the network process then adds no cookie (CookieSoup.cpp:80-81,
 * NetworkStorageSessionSoup.cpp:500-503) and the done callback would still report OK. */
static gboolean
cookie_value_valid (const char *value)
{
    gsize len;
    const char *p, *end;

    if (value == NULL)
        return FALSE;
    len = strlen (value);
    if (len > UC_SITE_COOKIE_MAX_VALUE_BYTES)
        return FALSE;
    /* after the NULL check: NULL + 0 is undefined in C (clang's -fsanitize=pointer-overflow reports it) */
    p = value;
    end = value + len;
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        p++;
        end--;
    }
    for (; p < end; p++) {
        unsigned char c = (unsigned char) *p;
        if (c < 0x21 || c > 0x7e || c == '"' || c == ',' || c == ';' || c == '\\')
            return FALSE;
    }
    return TRUE;
}

void
uc_site_cookie_clear (UcCookie *cookie)
{
    if (cookie == NULL)
        return;
    g_clear_pointer (&cookie->name, g_free);
    if (cookie->value != NULL)
        explicit_bzero (cookie->value, strlen (cookie->value));   /* the secret (best effort) */
    g_clear_pointer (&cookie->value, g_free);
    g_clear_pointer (&cookie->domain, g_free);
    g_clear_pointer (&cookie->path, g_free);
}

gboolean
uc_site_cookie_check (const char *url, const char *name, const char *value, const char *domain, const char *path,
                      int secure, int http_only, int64_t expires, UcCookie *out, const char **reason)
{
    const char *dummy;
    GUri *uri;
    const char *scheme;
    char *host;

    if (reason == NULL)
        reason = &dummy;
    *reason = NULL;
    memset (out, 0, sizeof *out);
    uri = url != NULL ? g_uri_parse (url, G_URI_FLAGS_NONE, NULL) : NULL;
    if (uri == NULL) {
        *reason = "the URL is not a URL";
        return FALSE;
    }
    scheme = g_uri_get_scheme (uri);
    if (g_ascii_strcasecmp (scheme, "http") != 0 && g_ascii_strcasecmp (scheme, "https") != 0) {
        g_uri_unref (uri);
        *reason = "the URL is not http or https";
        return FALSE;
    }
    host = uc_site_host_normalize (g_uri_get_host (uri));
    g_uri_unref (uri);
    if (host == NULL) {
        *reason = "the URL has no valid host";
        return FALSE;
    }
    if (!cookie_name_valid (name)) {
        *reason = "the name is not an RFC 6265 token of 1..256 bytes (no white space, separators, control characters "
                  "or non-ASCII bytes)";
        goto fail;
    }
    if (!cookie_value_valid (value)) {
        *reason = "the value is NULL, longer than 4096 bytes or not RFC 6265 cookie-octets (visible ASCII without '\"', "
                  "',', ';', '\\'; optionally in double quotes)";
        goto fail;
    }
    if (!(expires == -1 || (expires >= 0 && expires <= UC_SITE_COOKIE_MAX_EXPIRES))) {
        *reason = "expires is neither -1 nor a time in seconds since 1970 up to the year 9999";
        goto fail;
    }
    if (path != NULL && *path != '\0' && (path[0] != '/' || has_byte (path, ";", TRUE) || !g_str_is_ascii (path))) {
        *reason = "the path does not start with '/' or has ';', control characters or non-ASCII bytes";
        goto fail;
    }
    if (domain == NULL || *domain == '\0') {
        out->domain = g_strdup (host);      /* host-only: libsoup matches a domain without a leading dot exactly */
    } else {
        char *d = uc_site_host_normalize (domain[0] == '.' ? domain + 1 : domain);
        gsize hl = strlen (host), dl = d != NULL ? strlen (d) : 0;

        if (d == NULL) {
            *reason = "the domain is not a host name";
            goto fail;
        }
        if (g_hostname_is_ip_address (d) || g_hostname_is_ip_address (host)) {
            /* an address is no domain: only itself, host-only */
            if (strcmp (d, host) != 0) {
                *reason = "the domain is not the URL's host or a domain it belongs to";
                g_free (d);
                goto fail;
            }
            out->domain = d;
        } else {
            if (!(strcmp (d, host) == 0 || (hl > dl && host[hl - dl - 1] == '.' && strcmp (host + hl - dl, d) == 0))) {
                *reason = "the domain is not the URL's host or a domain it belongs to";
                g_free (d);
                goto fail;
            }
            /* the Domain attribute's meaning (RFC 6265 section 5.3, step 6): the domain and its subdomains, which
             * libsoup writes with a leading dot */
            out->domain = g_strconcat (".", d, NULL);
            g_free (d);
        }
    }
    out->name = g_strdup (name);
    out->value = g_strdup (value);
    out->path = g_strdup (path != NULL && *path != '\0' ? path : "/");
    out->secure = secure != 0;
    out->http_only = http_only != 0;
    out->expires = expires;
    g_free (host);
    return TRUE;

fail:
    g_free (host);
    uc_site_cookie_clear (out);
    return FALSE;
}

gboolean
uc_site_private_file (const char *path)
{
    static const char *const journals[] = { "-journal", "-wal", "-shm" };
    int fd;

    if (path == NULL)
        return FALSE;
    fd = g_open (path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return FALSE;
    if (fchmod (fd, 0600) != 0) {
        close (fd);
        return FALSE;
    }
    close (fd);
    for (gsize i = 0; i < G_N_ELEMENTS (journals); i++) {
        char *journal = g_strconcat (path, journals[i], NULL);
        struct stat st;
        if (lstat (journal, &st) == 0 && S_ISREG (st.st_mode))
            g_chmod (journal, 0600);
        g_free (journal);
    }
    return TRUE;
}

/* ---- user agent -------------------------------------------------------------------------------------------- */

/* The presets claim the engine the remote really has: AppleWebKit/605.1.15 is the frozen token WebKit itself sends
 * (UserAgentGLib.cpp:101), so a site that serves Safari markup serves what WebKit renders. The platform parts are
 * the ones sites tell phones and computers apart by: Safari on an iPhone, Safari on a Mac ("Macintosh; Intel Mac OS
 * X 10_15_7" is what Safari has sent since macOS 11; WebKit's own quirk for sites that want a Mac,
 * UserAgentQuirks.cpp:239, says "Macintosh; Intel Mac OS X 10_15"). The default is WebKit's own,
 * "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/60.5 Safari/605.1.15" on the
 * remote (UserAgentGLib.cpp:79-122, no chassis information in the UI's sandbox). */
static const char ua_mobile[] =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/18.6 Mobile/15E148 Safari/604.1";
static const char ua_desktop[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/18.6 Safari/605.1.15";

gboolean
uc_site_user_agent (int preset, const char *custom, const char **ua, const char **reason)
{
    const char *dummy;

    if (reason == NULL)
        reason = &dummy;
    *reason = NULL;
    *ua = NULL;
    switch (preset) {
    case UC_SITE_USER_AGENT_DEFAULT:
        return TRUE;
    case UC_SITE_USER_AGENT_MOBILE:
        *ua = ua_mobile;
        return TRUE;
    case UC_SITE_USER_AGENT_DESKTOP:
        *ua = ua_desktop;
        return TRUE;
    case UC_SITE_USER_AGENT_CUSTOM:
        if (custom == NULL || *custom == '\0' || strlen (custom) > UC_SITE_USER_AGENT_MAX_BYTES) {
            *reason = "the custom user agent is empty or longer than 512 bytes";
            return FALSE;
        }
        for (const char *p = custom; *p != '\0'; p++)
            if ((unsigned char) *p < 0x20 || (unsigned char) *p > 0x7e) {
                *reason = "the custom user agent has a byte outside visible ASCII and space";
                return FALSE;
            }
        *ua = custom;
        return TRUE;
    default:
        *reason = "unknown preset";
        return FALSE;
    }
}

const char *
uc_site_user_agent_name (int preset)
{
    switch (preset) {
    case UC_SITE_USER_AGENT_DEFAULT: return "default";
    case UC_SITE_USER_AGENT_MOBILE:  return "mobile";
    case UC_SITE_USER_AGENT_DESKTOP: return "desktop";
    case UC_SITE_USER_AGENT_CUSTOM:  return "custom";
    default:                         return "unknown";
    }
}

/* ---- the Remote in the default user agent ---------------------------------------------------------------- */

gboolean
uc_site_version_valid (const char *version)
{
    gsize len;

    if (version == NULL)
        return FALSE;
    len = strlen (version);
    if (len == 0 || len > UC_SITE_VERSION_MAX_BYTES)
        return FALSE;
    for (const char *p = version; *p != '\0'; p++)
        if (!g_ascii_isalnum (*p) && strchr ("!#$%&'*+-.^_`|~", *p) == NULL)
            return FALSE;
    return TRUE;
}

char *
uc_site_release_version (const char *contents)
{
    char **lines;
    char *version = NULL;

    if (contents == NULL)
        return NULL;
    lines = g_strsplit (contents, "\n", -1);
    for (char **l = lines; *l != NULL && version == NULL; l++) {
        char *line = g_strstrip (*l);   /* also the CR of a CR LF line end */
        gsize len;

        if (!g_str_has_prefix (line, "UC_OS_VERSION="))
            continue;
        line += strlen ("UC_OS_VERSION=");
        len = strlen (line);
        if (len >= 2 && (line[0] == '"' || line[0] == '\'') && line[len - 1] == line[0]) {
            line[len - 1] = '\0';
            line++;
        }
        if (uc_site_version_valid (line) && line[0] != '$')    /* not the template's $BUILD_VERSION */
            version = g_strdup (line);
        else
            break;      /* the first UC_OS_VERSION counts, as in the shell; an invalid one gives none */
    }
    g_strfreev (lines);
    return version;
}

char *
uc_site_firmware_version (const char *path, const char *env_version, const char **source)
{
    const char *dummy;
    char *contents = NULL;

    if (source == NULL)
        source = &dummy;
    if (path != NULL && g_file_get_contents (path, &contents, NULL, NULL)) {
        char *version = uc_site_release_version (contents);
        g_free (contents);
        if (version != NULL) {
            *source = path;
            return version;
        }
    }
    if (uc_site_version_valid (env_version)) {
        *source = "UC_OS_VERSION";
        return g_strdup (env_version);
    }
    *source = env_version != NULL && *env_version != '\0' ? "no valid version: UC_OS_VERSION is not a version token"
                                                           : "no version: no readable release file, no UC_OS_VERSION";
    return NULL;
}

char *
uc_site_default_user_agent (const char *webkit_ua, const char *version)
{
    if (uc_site_version_valid (version))
        return g_strdup_printf ("%s %s/%s", webkit_ua, UC_SITE_UA_PRODUCT, version);
    return g_strdup_printf ("%s %s", webkit_ua, UC_SITE_UA_PRODUCT);
}

/* ---- website data ------------------------------------------------------------------------------------------ */

gboolean
uc_site_clear_types_valid (unsigned types)
{
    return types == UC_SITE_DATA_CACHE || types == UC_SITE_DATA_ALL;
}
