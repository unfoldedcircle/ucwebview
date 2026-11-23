/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_site.c (GLib only), the site settings of library 0.8.0 that need no WebKit: hosts and
 * certificate fingerprints, cookie checks, user agent presets, website data types. Run with `make check` (also in
 * `make check-asan`).
 */
#include "uc_site.h"
#include "uc_webview.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

G_STATIC_ASSERT (UC_SITE_USER_AGENT_DEFAULT == UC_WEBVIEW_USER_AGENT_DEFAULT);
G_STATIC_ASSERT (UC_SITE_USER_AGENT_MOBILE == UC_WEBVIEW_USER_AGENT_MOBILE);
G_STATIC_ASSERT (UC_SITE_USER_AGENT_DESKTOP == UC_WEBVIEW_USER_AGENT_DESKTOP);
G_STATIC_ASSERT (UC_SITE_USER_AGENT_CUSTOM == UC_WEBVIEW_USER_AGENT_CUSTOM);
G_STATIC_ASSERT (UC_SITE_DATA_CACHE == UC_WEBVIEW_DATA_CACHE);
G_STATIC_ASSERT (UC_SITE_DATA_ALL == UC_WEBVIEW_DATA_ALL);

static const char sha_a[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char sha_b[] = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

static void
assert_host (const char *in, const char *expected)
{
    char *host = uc_site_host_normalize (in);
    g_assert_cmpstr (host, ==, expected);
    g_free (host);
}

static void
test_host (void)
{
    assert_host ("Example.COM", "example.com");
    assert_host ("192.168.1.10", "192.168.1.10");
    assert_host ("[::1]", "::1");
    assert_host ("FE80::1", "fe80::1");
    assert_host ("bücher.example", "xn--bcher-kva.example");
    assert_host ("my_host.local", "my_host.local");
    assert_host ("example.com:443", NULL);
    assert_host ("https://example.com", NULL);
    assert_host ("example.com/x", NULL);
    assert_host ("user@example.com", NULL);
    assert_host ("exa mple.com", NULL);
    assert_host ("", NULL);
    assert_host (NULL, NULL);
}

static void
assert_fingerprint (const char *in, const char *expected)
{
    char *fingerprint = uc_site_fingerprint_normalize (in);
    g_assert_cmpstr (fingerprint, ==, expected);
    g_free (fingerprint);
}

static void
test_fingerprint (void)
{
    char *upper = g_ascii_strup (sha_a, -1);
    GString *colons = g_string_new (NULL);

    assert_fingerprint (sha_a, sha_a);
    assert_fingerprint (upper, sha_a);
    for (int i = 0; i < 64; i += 2)
        g_string_append_printf (colons, "%s%c%c", i > 0 ? ":" : "", upper[i], upper[i + 1]);
    assert_fingerprint (colons->str, sha_a);       /* openssl x509 -fingerprint -sha256 */
    assert_fingerprint ("0123", NULL);
    assert_fingerprint ("", NULL);
    assert_fingerprint (NULL, NULL);
    {
        char *longer = g_strconcat (sha_a, "00", NULL);
        char *wrong = g_strdup (sha_a);
        wrong[10] = 'g';
        assert_fingerprint (longer, NULL);
        assert_fingerprint (wrong, NULL);
        g_free (longer);
        g_free (wrong);
    }
    g_free (upper);
    g_string_free (colons, TRUE);
}

static void
test_tls_exceptions (void)
{
    UcTlsExceptions *tls = uc_tls_exceptions_new ();
    const char *reason = NULL;
    gboolean any = TRUE;

    g_assert_false (uc_tls_exceptions_match (tls, "nas.local", sha_a, &any));
    g_assert_false (any);
    g_assert_cmpint (uc_tls_exceptions_add (tls, "NAS.local", sha_a, &reason), ==, 0);
    g_assert_true (uc_tls_exceptions_match (tls, "nas.local", sha_a, &any));
    g_assert_false (any);
    g_assert_false (uc_tls_exceptions_match (tls, "nas.local", sha_b, NULL));
    g_assert_false (uc_tls_exceptions_match (tls, "other.local", sha_a, NULL));
    g_assert_false (uc_tls_exceptions_match (tls, "nas.local", NULL, NULL));
    /* the same twice, and a second fingerprint */
    g_assert_cmpint (uc_tls_exceptions_add (tls, "nas.local", sha_a, NULL), ==, 0);
    g_assert_cmpint (uc_tls_exceptions_add (tls, "nas.local", sha_b, NULL), ==, 0);
    g_assert_true (uc_tls_exceptions_match (tls, "NAS.LOCAL", sha_b, NULL));
    g_assert_cmpuint (uc_tls_exceptions_size (tls), ==, 1);
    /* any certificate of a host */
    g_assert_cmpint (uc_tls_exceptions_add (tls, "[fd00::5]", NULL, NULL), ==, 0);
    g_assert_true (uc_tls_exceptions_match (tls, "fd00::5", sha_b, &any));
    g_assert_true (any);
    g_assert_true (uc_tls_exceptions_match (tls, "[FD00::5]", NULL, &any));
    /* refused */
    g_assert_cmpint (uc_tls_exceptions_add (tls, "nas.local:443", sha_a, &reason), ==, -1);
    g_assert_nonnull (strstr (reason, "host"));
    g_assert_cmpint (uc_tls_exceptions_add (tls, "nas.local", "abc", &reason), ==, -1);
    g_assert_nonnull (strstr (reason, "fingerprint"));
    g_assert_cmpint (uc_tls_exceptions_add (tls, NULL, NULL, &reason), ==, -1);
    /* limits */
    for (int i = 0; i < UC_SITE_TLS_MAX_FINGERPRINTS - 2; i++) {
        char fp[65];
        g_snprintf (fp, sizeof fp, "%064x", i + 1);
        g_assert_cmpint (uc_tls_exceptions_add (tls, "nas.local", fp, NULL), ==, 0);
    }
    {
        char fp[65];
        g_snprintf (fp, sizeof fp, "%064x", 999);
        g_assert_cmpint (uc_tls_exceptions_add (tls, "nas.local", fp, &reason), ==, -1);
        g_assert_nonnull (strstr (reason, "too many"));
    }
    for (int i = (int) uc_tls_exceptions_size (tls); i < UC_SITE_TLS_MAX_HOSTS; i++) {
        char host[32];
        g_snprintf (host, sizeof host, "h%d.local", i);
        g_assert_cmpint (uc_tls_exceptions_add (tls, host, NULL, NULL), ==, 0);
    }
    g_assert_cmpint (uc_tls_exceptions_add (tls, "one-more.local", NULL, &reason), ==, -1);
    /* a host that is in the table takes another fingerprint even when the table is full */
    g_assert_cmpint (uc_tls_exceptions_add (tls, "h63.local", sha_a, NULL), ==, 0);
    uc_tls_exceptions_free (tls);
    uc_tls_exceptions_free (NULL);
}

static void
test_cookie_ok (void)
{
    UcCookie c;
    const char *reason = NULL;

    g_assert_true (uc_site_cookie_check ("https://ha.example.com/lovelace", "session", "abc=def", NULL, NULL, 1, 1, -1,
                                         &c, &reason));
    g_assert_null (reason);
    g_assert_cmpstr (c.name, ==, "session");
    g_assert_cmpstr (c.value, ==, "abc=def");
    g_assert_cmpstr (c.domain, ==, "ha.example.com");  /* host-only */
    g_assert_cmpstr (c.path, ==, "/");
    g_assert_true (c.secure);
    g_assert_true (c.http_only);
    g_assert_cmpint (c.expires, ==, -1);
    uc_site_cookie_clear (&c);

    /* a parent domain, with and without the leading dot */
    g_assert_true (uc_site_cookie_check ("https://ha.example.com/", "a", "1", "example.com", "/app", 0, 0, 1893456000,
                                         &c, NULL));
    g_assert_cmpstr (c.domain, ==, ".example.com");
    g_assert_cmpstr (c.path, ==, "/app");
    g_assert_cmpint (c.expires, ==, 1893456000);
    uc_site_cookie_clear (&c);
    g_assert_true (uc_site_cookie_check ("https://ha.example.com/", "a", "1", ".Example.COM", "", 0, 0, 0, &c, NULL));
    g_assert_cmpstr (c.domain, ==, ".example.com");
    g_assert_cmpstr (c.path, ==, "/");
    uc_site_cookie_clear (&c);
    /* the host itself as Domain: the host and its subdomains */
    g_assert_true (uc_site_cookie_check ("http://ha.example.com:8123/", "a", "", "ha.example.com", NULL, 0, 0, -1, &c,
                                         NULL));
    g_assert_cmpstr (c.domain, ==, ".ha.example.com");
    g_assert_cmpstr (c.value, ==, "");
    uc_site_cookie_clear (&c);
    /* an IP address: only itself, host-only */
    g_assert_true (uc_site_cookie_check ("http://192.168.1.10:8123/", "a", "1", "192.168.1.10", NULL, 0, 0, -1, &c,
                                         NULL));
    g_assert_cmpstr (c.domain, ==, "192.168.1.10");
    uc_site_cookie_clear (&c);
    g_assert_true (uc_site_cookie_check ("http://[::1]:8080/", "a", "1", NULL, NULL, 0, 0, -1, &c, NULL));
    g_assert_cmpstr (c.domain, ==, "::1");
    uc_site_cookie_clear (&c);
    uc_site_cookie_clear (NULL);
}

static void
assert_cookie_refused (const char *url, const char *name, const char *value, const char *domain, const char *path,
                       int64_t expires, const char *reason_part)
{
    UcCookie c;
    const char *reason = NULL;

    g_assert_false (uc_site_cookie_check (url, name, value, domain, path, 0, 0, expires, &c, &reason));
    g_assert_nonnull (reason);
    g_assert_nonnull (strstr (reason, reason_part));
    g_assert_null (c.name);
    g_assert_null (c.domain);
}

static void
test_cookie_refused (void)
{
    char *long_name = g_strnfill (UC_SITE_COOKIE_MAX_NAME_BYTES + 1, 'n');
    char *long_value = g_strnfill (UC_SITE_COOKIE_MAX_VALUE_BYTES + 1, 'v');

    assert_cookie_refused ("ftp://example.com/", "a", "1", NULL, NULL, -1, "http");
    assert_cookie_refused ("not a url", "a", "1", NULL, NULL, -1, "URL");
    assert_cookie_refused (NULL, "a", "1", NULL, NULL, -1, "URL");
    assert_cookie_refused ("https://example.com/", "", "1", NULL, NULL, -1, "name");
    assert_cookie_refused ("https://example.com/", NULL, "1", NULL, NULL, -1, "name");
    assert_cookie_refused ("https://example.com/", "a b", "1", NULL, NULL, -1, "name");
    assert_cookie_refused ("https://example.com/", "a=b", "1", NULL, NULL, -1, "name");
    assert_cookie_refused ("https://example.com/", "a;", "1", NULL, NULL, -1, "name");
    assert_cookie_refused ("https://example.com/", "a", "1; Domain=evil", NULL, NULL, -1, "value");
    assert_cookie_refused ("https://example.com/", "a", "1\r\n", NULL, NULL, -1, "value");
    assert_cookie_refused ("https://example.com/", "a", NULL, NULL, NULL, -1, "value");
    assert_cookie_refused ("https://example.com/", long_name, "1", NULL, NULL, -1, "256");
    assert_cookie_refused ("https://example.com/", "a", long_value, NULL, NULL, -1, "4096");
    /* RFC 6265 (library 0.8.3): token names, cookie-octet values; before, WebKit dropped a cookie with invalid UTF-8
     * silently and the callback still reported OK */
    assert_cookie_refused ("https://example.com/", "a", "a b", NULL, NULL, -1, "cookie-octets");
    assert_cookie_refused ("https://example.com/", "a", "a,b", NULL, NULL, -1, "cookie-octets");
    assert_cookie_refused ("https://example.com/", "a", "a\"b", NULL, NULL, -1, "cookie-octets");
    assert_cookie_refused ("https://example.com/", "a", "a\\b", NULL, NULL, -1, "cookie-octets");
    assert_cookie_refused ("https://example.com/", "a", "Gr\xc3\xbc" "ezi", NULL, NULL, -1, "cookie-octets");
    assert_cookie_refused ("https://example.com/", "a", "\xff", NULL, NULL, -1, "cookie-octets");
    assert_cookie_refused ("https://example.com/", "a", "\"abc", NULL, NULL, -1, "cookie-octets");
    assert_cookie_refused ("https://example.com/", "gr\xc3\xbc" "n", "1", NULL, NULL, -1, "token");
    assert_cookie_refused ("https://example.com/", "a(b)", "1", NULL, NULL, -1, "token");
    assert_cookie_refused ("https://example.com/", "a", "1", NULL, "/gr\xc3\xbc" "n", -1, "path");
    assert_cookie_refused ("https://example.com/", "a", "1", NULL, NULL, -2, "expires");
    assert_cookie_refused ("https://example.com/", "a", "1", NULL, NULL, UC_SITE_COOKIE_MAX_EXPIRES + 1, "expires");
    assert_cookie_refused ("https://example.com/", "a", "1", NULL, "app", -1, "path");
    assert_cookie_refused ("https://example.com/", "a", "1", NULL, "/a;b", -1, "path");
    /* domains the URL's host is not in */
    assert_cookie_refused ("https://ha.example.com/", "a", "1", "other.com", NULL, -1, "domain");
    assert_cookie_refused ("https://ha.example.com/", "a", "1", "xample.com", NULL, -1, "domain");
    assert_cookie_refused ("https://ha.example.com/", "a", "1", "sub.ha.example.com", NULL, -1, "domain");
    assert_cookie_refused ("https://example.com/", "a", "1", "example.com:443", NULL, -1, "domain");
    assert_cookie_refused ("http://192.168.1.10/", "a", "1", "168.1.10", NULL, -1, "domain");
    assert_cookie_refused ("http://192.168.1.10/", "a", "1", "192.168.1.11", NULL, -1, "domain");
    g_free (long_name);
    g_free (long_value);
}

static void
test_cookie_limits (void)
{
    UcCookie c;
    char *name = g_strnfill (UC_SITE_COOKIE_MAX_NAME_BYTES, 'n');
    char *value = g_strnfill (UC_SITE_COOKIE_MAX_VALUE_BYTES, 'v');

    /* the spec's limits exactly (the Core-API's web view entity: name 256, value 4096) */
    g_assert_true (uc_site_cookie_check ("https://example.com/", name, value, NULL, NULL, 0, 0, -1, &c, NULL));
    uc_site_cookie_clear (&c);
    /* cookie-octets: '=' and the rest of visible ASCII but '"', ',', ';', '\\'; a quoted value; the empty value */
    g_assert_true (uc_site_cookie_check ("https://example.com/", "a", "x=y!#$%&'()*+-./:<>?@[]^_`{|}~", NULL, NULL, 0, 0,
                                         -1, &c, NULL));
    uc_site_cookie_clear (&c);
    g_assert_true (uc_site_cookie_check ("https://example.com/", "a", "\"quoted\"", NULL, NULL, 0, 0, -1, &c, NULL));
    g_assert_cmpstr (c.value, ==, "\"quoted\"");
    uc_site_cookie_clear (&c);
    g_assert_true (uc_site_cookie_check ("https://example.com/", "a", "\"\"", NULL, NULL, 0, 0, -1, &c, NULL));
    uc_site_cookie_clear (&c);
    g_assert_true (uc_site_cookie_check ("https://example.com/", "__Host-session!#$%&'*+-.^_`|~", "1", NULL, NULL, 1, 0,
                                         -1, &c, NULL));
    uc_site_cookie_clear (&c);
    g_free (name);
    g_free (value);
}

static void
test_user_agent (void)
{
    const char *ua = (const char *) 1, *reason = NULL;

    g_assert_true (uc_site_user_agent (UC_SITE_USER_AGENT_DEFAULT, "ignored", &ua, &reason));
    g_assert_null (ua);
    g_assert_true (uc_site_user_agent (UC_SITE_USER_AGENT_MOBILE, NULL, &ua, NULL));
    g_assert_nonnull (strstr (ua, "iPhone"));
    g_assert_nonnull (strstr (ua, "Mobile"));
    g_assert_nonnull (strstr (ua, "AppleWebKit/605.1.15"));
    g_assert_true (uc_site_user_agent (UC_SITE_USER_AGENT_DESKTOP, "ignored", &ua, NULL));
    g_assert_nonnull (strstr (ua, "Macintosh"));
    g_assert_null (strstr (ua, "Mobile"));
    g_assert_true (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, "MyRemote/1.0 (UC)", &ua, NULL));
    g_assert_cmpstr (ua, ==, "MyRemote/1.0 (UC)");
    /* refused */
    g_assert_false (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, NULL, &ua, &reason));
    g_assert_nonnull (reason);
    g_assert_false (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, "", &ua, &reason));
    g_assert_false (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, "a\r\nX-Evil: 1", &ua, &reason));
    g_assert_false (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, "Gr\xc3\xbc" "ezi", &ua, &reason));
    {
        char *long_ua = g_strnfill (UC_SITE_USER_AGENT_MAX_BYTES + 1, 'u');
        g_assert_false (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, long_ua, &ua, &reason));
        long_ua[UC_SITE_USER_AGENT_MAX_BYTES] = '\0';
        g_assert_true (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, long_ua, &ua, &reason));
        g_free (long_ua);
    }
    g_assert_false (uc_site_user_agent (4, NULL, &ua, &reason));
    g_assert_false (uc_site_user_agent (-1, NULL, &ua, &reason));
    g_assert_cmpstr (uc_site_user_agent_name (UC_SITE_USER_AGENT_MOBILE), ==, "mobile");
    g_assert_cmpstr (uc_site_user_agent_name (9), ==, "unknown");
}

static void
test_clear_types (void)
{
    g_assert_true (uc_site_clear_types_valid (UC_SITE_DATA_CACHE));
    g_assert_true (uc_site_clear_types_valid (UC_SITE_DATA_ALL));
    g_assert_false (uc_site_clear_types_valid (0));
    g_assert_false (uc_site_clear_types_valid (0x2));         /* a bit of a later release */
    g_assert_false (uc_site_clear_types_valid (0x3));
}

static void
assert_release (const char *contents, const char *expected)
{
    char *version = uc_site_release_version (contents);
    g_assert_cmpstr (version, ==, expected);
    g_free (version);
}

static void
test_release_version (void)
{
    /* the firmware's file as its build writes it */
    assert_release ("# Version, git hash & model set during build. File is read by the UC app system service file\n"
                    "UC_OS_VERSION=2.13.2-dev.3+5bf0cf38\nUC_OS_GITHASH=5bf0cf3874707f24d97c3c12b9eb0e706ce1167f\n"
                    "UC_MODEL=UCR3\n", "2.13.2-dev.3+5bf0cf38");
    assert_release ("UC_OS_VERSION=2.13.1", "2.13.1");
    assert_release ("UC_OS_VERSION=\"2.13.1\"\n", "2.13.1");
    assert_release ("UC_OS_VERSION='2.13.1'\r\nUC_MODEL=UCR2\r\n", "2.13.1");
    assert_release ("  UC_OS_VERSION=2.13.1  \n", "2.13.1");
    assert_release ("UC_MODEL=UCR3\n# UC_OS_VERSION=1.0 in a comment\n", NULL);
    assert_release ("UC_OS_VERSION=\n", NULL);
    assert_release ("UC_OS_VERSION=$BUILD_VERSION\n", NULL);                /* the template, not filled in */
    assert_release ("UC_OS_VERSION=2.13 beta\n", NULL);                     /* space */
    assert_release ("UC_OS_VERSION=2.13/1\n", NULL);                        /* '/' */
    assert_release ("UC_OS_VERSION=\"2.13.1\n", NULL);                      /* unbalanced quote */
    assert_release ("UC_OS_VERSION=x\nUC_OS_VERSION=y\n", "x");            /* the first one */
    assert_release ("UC_OS_VERSIONS=2\n", NULL);
    assert_release ("", NULL);
    assert_release (NULL, NULL);
    {
        char *long_line = g_strconcat ("UC_OS_VERSION=", "", NULL);
        char *v65 = g_strnfill (UC_SITE_VERSION_MAX_BYTES + 1, '1');
        char *v64 = g_strnfill (UC_SITE_VERSION_MAX_BYTES, '1');
        char *c65 = g_strconcat (long_line, v65, NULL), *c64 = g_strconcat (long_line, v64, NULL);
        assert_release (c65, NULL);
        assert_release (c64, v64);
        g_free (long_line);
        g_free (v65);
        g_free (v64);
        g_free (c65);
        g_free (c64);
    }
    g_assert_true (uc_site_version_valid ("2.13.2-dev.3+5bf0cf38"));
    g_assert_false (uc_site_version_valid (""));
    g_assert_false (uc_site_version_valid (NULL));
    g_assert_false (uc_site_version_valid ("2.13\r\n"));
}

static void
test_firmware_version (void)
{
    char *dir = g_dir_make_tmp ("test-site-XXXXXX", NULL);
    char *path = g_build_filename (dir, "uc-release", NULL);
    char *missing = g_build_filename (dir, "missing", NULL);
    const char *source = NULL;
    char *version;

    g_assert_true (g_file_set_contents (path, "UC_OS_VERSION=2.13.2\nUC_MODEL=UCR3\n", -1, NULL));
    /* the file wins over the environment (the same file in production) */
    version = uc_site_firmware_version (path, "9.9.9", &source);
    g_assert_cmpstr (version, ==, "2.13.2");
    g_assert_cmpstr (source, ==, path);
    g_free (version);
    /* not readable (the UI's sandbox): the environment the unit loaded from it */
    version = uc_site_firmware_version (missing, "2.13.2-dev.3+5bf0cf38", &source);
    g_assert_cmpstr (version, ==, "2.13.2-dev.3+5bf0cf38");
    g_assert_cmpstr (source, ==, "UC_OS_VERSION");
    g_free (version);
    /* a file without a version: the environment */
    g_assert_true (g_file_set_contents (path, "UC_MODEL=UCR3\n", -1, NULL));
    version = uc_site_firmware_version (path, "2.0.0", &source);
    g_assert_cmpstr (version, ==, "2.0.0");
    g_free (version);
    /* none */
    g_assert_null (uc_site_firmware_version (missing, NULL, &source));
    g_assert_nonnull (strstr (source, "no version"));
    g_assert_null (uc_site_firmware_version (missing, "bad version", &source));
    g_assert_nonnull (strstr (source, "not a version"));
    g_assert_null (uc_site_firmware_version (NULL, NULL, NULL));
    g_remove (path);
    g_rmdir (dir);
    g_free (path);
    g_free (missing);
    g_free (dir);
}

static guint
mode_of (const char *path)
{
    GStatBuf st;
    g_assert_cmpint (g_lstat (path, &st), ==, 0);
    return st.st_mode & 07777;
}

static void
test_private_file (void)
{
    char *dir = g_dir_make_tmp ("test-site-XXXXXX", NULL);
    char *db = g_build_filename (dir, "cookies.sqlite", NULL);
    char *journal = g_strconcat (db, "-journal", NULL);
    char *link = g_build_filename (dir, "link.sqlite", NULL);

    /* missing: created empty, 0600 */
    g_assert_true (uc_site_private_file (db));
    g_assert_cmpuint (mode_of (db), ==, 0600);
    /* existing, as 0.8.2 left it: 0644 with a journal; contents kept */
    g_assert_true (g_file_set_contents (db, "SQLite format 3", -1, NULL));
    g_assert_cmpint (g_chmod (db, 0644), ==, 0);
    g_assert_true (g_file_set_contents (journal, "j", -1, NULL));
    g_assert_cmpint (g_chmod (journal, 0644), ==, 0);
    g_assert_true (uc_site_private_file (db));
    g_assert_cmpuint (mode_of (db), ==, 0600);
    g_assert_cmpuint (mode_of (journal), ==, 0600);
    {
        char *contents = NULL;
        g_assert_true (g_file_get_contents (db, &contents, NULL, NULL));
        g_assert_cmpstr (contents, ==, "SQLite format 3");
        g_free (contents);
    }
    /* a symbolic link is not followed */
    g_assert_cmpint (symlink (db, link), ==, 0);
    g_assert_false (uc_site_private_file (link));
    g_assert_false (uc_site_private_file (NULL));
    g_remove (link);
    g_remove (journal);
    g_remove (db);
    g_rmdir (dir);
    g_free (link);
    g_free (journal);
    g_free (db);
    g_free (dir);
}

static void
test_default_user_agent (void)
{
    static const char webkit[] = "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/605.1.15 (KHTML, like Gecko) "
                                 "Version/60.5 Safari/605.1.15";
    char *ua = uc_site_default_user_agent (webkit, "2.13.2-dev.3+5bf0cf38");

    g_assert_cmpstr (ua, ==, "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/60.5 "
                             "Safari/605.1.15 UCRemote/2.13.2-dev.3+5bf0cf38");
    g_free (ua);
    ua = uc_site_default_user_agent (webkit, NULL);
    g_assert_true (g_str_has_suffix (ua, "Safari/605.1.15 UCRemote"));
    g_free (ua);
    ua = uc_site_default_user_agent (webkit, "a b");
    g_assert_true (g_str_has_suffix (ua, " UCRemote"));
    g_free (ua);
    /* the token is a valid custom user agent text too: visible ASCII and spaces */
    {
        const char *out = NULL;
        ua = uc_site_default_user_agent (webkit, "2.13.2");
        g_assert_true (uc_site_user_agent (UC_SITE_USER_AGENT_CUSTOM, ua, &out, NULL));
        g_free (ua);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/site/host", test_host);
    g_test_add_func ("/site/fingerprint", test_fingerprint);
    g_test_add_func ("/site/tls-exceptions", test_tls_exceptions);
    g_test_add_func ("/site/cookie-ok", test_cookie_ok);
    g_test_add_func ("/site/cookie-refused", test_cookie_refused);
    g_test_add_func ("/site/cookie-limits", test_cookie_limits);
    g_test_add_func ("/site/private-file", test_private_file);
    g_test_add_func ("/site/user-agent", test_user_agent);
    g_test_add_func ("/site/clear-types", test_clear_types);
    g_test_add_func ("/site/release-version", test_release_version);
    g_test_add_func ("/site/firmware-version", test_firmware_version);
    g_test_add_func ("/site/default-user-agent", test_default_user_agent);
    return g_test_run ();
}
