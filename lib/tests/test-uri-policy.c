/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_uri_policy.c (GLib only): run with `make check`.
 */
#include "uc_uri_policy.h"

#include <string.h>

static void
test_always_allowed (void)
{
    for (int allow_file = 0; allow_file <= 1; allow_file++) {
        g_assert_cmpint (uc_uri_policy_check ("http://localhost/", allow_file), ==, UC_URI_ALLOWED);
        g_assert_cmpint (uc_uri_policy_check ("https://example.com/a?b#c", allow_file), ==, UC_URI_ALLOWED);
        g_assert_cmpint (uc_uri_policy_check ("HTTPS://EXAMPLE.COM/", allow_file), ==, UC_URI_ALLOWED);
        g_assert_cmpint (uc_uri_policy_check ("about:blank", allow_file), ==, UC_URI_ALLOWED);
        g_assert_cmpint (uc_uri_policy_check ("data:text/html,<p>hi</p>", allow_file), ==, UC_URI_ALLOWED);
    }
}

static void
test_file (void)
{
    g_assert_cmpint (uc_uri_policy_check ("file:///app/data/emoji.html", FALSE), ==, UC_URI_REJECTED_FILE);
    g_assert_cmpint (uc_uri_policy_check ("FILE:///etc/passwd", FALSE), ==, UC_URI_REJECTED_FILE);
    g_assert_cmpint (uc_uri_policy_check ("file:/etc/passwd", FALSE), ==, UC_URI_REJECTED_FILE);
    g_assert_cmpint (uc_uri_policy_check ("file:///app/data/emoji.html", TRUE), ==, UC_URI_ALLOWED);
    g_assert_cmpint (uc_uri_policy_check ("file:///", TRUE), ==, UC_URI_ALLOWED);
}

static void
test_rejected (void)
{
    for (int allow_file = 0; allow_file <= 1; allow_file++) {
        g_assert_cmpint (uc_uri_policy_check ("javascript:alert(1)", allow_file), ==, UC_URI_REJECTED_SCHEME);
        g_assert_cmpint (uc_uri_policy_check ("blob:http://x/1", allow_file), ==, UC_URI_REJECTED_SCHEME);
        g_assert_cmpint (uc_uri_policy_check ("ftp://host/file", allow_file), ==, UC_URI_REJECTED_SCHEME);
        g_assert_cmpint (uc_uri_policy_check ("resource:///org/x", allow_file), ==, UC_URI_REJECTED_SCHEME);
        g_assert_cmpint (uc_uri_policy_check ("localhost:8080/", allow_file), ==, UC_URI_REJECTED_SCHEME);
        g_assert_cmpint (uc_uri_policy_check ("/app/data/emoji.html", allow_file), ==, UC_URI_REJECTED_INVALID);
        g_assert_cmpint (uc_uri_policy_check ("example.com", allow_file), ==, UC_URI_REJECTED_INVALID);
        g_assert_cmpint (uc_uri_policy_check (" http://x/", allow_file), ==, UC_URI_REJECTED_INVALID);
        g_assert_cmpint (uc_uri_policy_check ("", allow_file), ==, UC_URI_REJECTED_INVALID);
        g_assert_cmpint (uc_uri_policy_check (NULL, allow_file), ==, UC_URI_REJECTED_INVALID);
    }
}

static void
test_base (void)
{
    g_assert_cmpint (uc_uri_policy_check_base (NULL, FALSE), ==, UC_URI_ALLOWED);
    g_assert_cmpint (uc_uri_policy_check_base ("", FALSE), ==, UC_URI_ALLOWED);
    g_assert_cmpint (uc_uri_policy_check_base ("about:blank", FALSE), ==, UC_URI_ALLOWED);
    g_assert_cmpint (uc_uri_policy_check_base ("http://base/", FALSE), ==, UC_URI_ALLOWED);
    g_assert_cmpint (uc_uri_policy_check_base ("file:///", FALSE), ==, UC_URI_REJECTED_FILE);
    g_assert_cmpint (uc_uri_policy_check_base ("file:///", TRUE), ==, UC_URI_ALLOWED);
    g_assert_cmpint (uc_uri_policy_check_base ("javascript:1", TRUE), ==, UC_URI_REJECTED_SCHEME);
}

static void
test_describe (void)
{
    g_assert_nonnull (strstr (uc_uri_policy_describe (UC_URI_REJECTED_FILE), "UC_WEBVIEW_ALLOW_FILE_URLS=1"));
    g_assert_nonnull (uc_uri_policy_describe (UC_URI_REJECTED_SCHEME));
    g_assert_nonnull (uc_uri_policy_describe (UC_URI_REJECTED_INVALID));
    g_assert_cmpstr (uc_uri_policy_describe (UC_URI_ALLOWED), ==, "allowed");
}

static void
expect_loggable (const char *uri, gboolean full, const char *expected)
{
    char *text = uc_uri_policy_loggable (uri, full);
    g_assert_cmpstr (text, ==, expected);
    g_free (text);
}

static void
test_loggable (void)
{
    /* by default the origin only (0.10.1): no user info, path, query or fragment */
    expect_loggable ("https://user:secret@example.com/path?q=1", FALSE, "https://example.com");
    expect_loggable ("https://nonexistent-host.invalid/secret/path?token=1#frag", FALSE,
                     "https://nonexistent-host.invalid");
    expect_loggable ("https://example.com/cb?code=4/0AY0e-g7&state=xyz#access_token=ya29.a0Af", FALSE,
                     "https://example.com");
    expect_loggable ("https://example.com/reset/0123456789abcdef", FALSE, "https://example.com");
    expect_loggable ("http://localhost/configurator/#/login", FALSE, "http://localhost");
    expect_loggable ("http://localhost:8080", FALSE, "http://localhost:8080");
    expect_loggable ("https://host:8443/a/b?c", FALSE, "https://host:8443");
    /* the port only when the URI names one, also the default one */
    expect_loggable ("http://example.com:80/", FALSE, "http://example.com:80");
    expect_loggable ("http://example.com/", FALSE, "http://example.com");
    /* IP literals keep their brackets */
    expect_loggable ("http://[::1]:80/a", FALSE, "http://[::1]:80");
    expect_loggable ("http://[2001:db8::1]/a?b", FALSE, "http://[2001:db8::1]");
    expect_loggable ("http://192.168.1.2:8080/x", FALSE, "http://192.168.1.2:8080");
    expect_loggable ("HTTPS://Example.COM/x", FALSE, "https://Example.COM");
    /* no host: the scheme alone */
    expect_loggable ("file:///app/data/emoji.html", FALSE, "file:");
    expect_loggable ("file:/etc/passwd?x", FALSE, "file:");
    expect_loggable ("about:blank", FALSE, "about:");
    expect_loggable ("data:text/html,<input value=secret>", FALSE, "data:");
    expect_loggable ("data:text/html;base64,PGlucHV0Pg==", FALSE, "data:");
    expect_loggable ("javascript:send('secret')", FALSE, "javascript:");
    expect_loggable ("blob:http://x/1", FALSE, "blob:");
    expect_loggable ("mailto:someone@example.com", FALSE, "mailto:");
    expect_loggable ("/app/data/emoji.html", FALSE, "(no scheme)");
    expect_loggable ("/app/data/emoji.html", TRUE, "(no scheme)");
    expect_loggable ("http://[::1/x?secret", FALSE, "http:"); /* does not parse */
    expect_loggable (NULL, FALSE, "(null)");

    /* UC_WEBVIEW_LOG_FULL_URLS (full): everything but the user info */
    expect_loggable ("https://user:secret@example.com/path?q=1", TRUE, "https://example.com/path?q=1");
    expect_loggable ("https://example.com/cb?code=4/0AY0e-g7#access_token=ya29.a0Af", TRUE,
                     "https://example.com/cb?code=4/0AY0e-g7#access_token=ya29.a0Af");
    expect_loggable ("data:text/html,<p>hi</p>", TRUE, "data:text/html,<p>hi</p>");
    expect_loggable ("about:blank", TRUE, "about:blank");

    {
        GString *long_uri = g_string_new ("https://example.com/");
        char *text;
        for (int i = 0; i < 100; i++)
            g_string_append (long_uri, "\xc3\xa4"); /* U+00E4, two bytes: the cut must not split it */
        g_string_append (long_uri, "?token=secret");
        text = uc_uri_policy_loggable (long_uri->str, TRUE);
        g_assert_cmpuint (strlen (text), <=, 200 + 3);
        g_assert_true (g_str_has_suffix (text, "..."));
        g_assert_true (g_utf8_validate (text, -1, NULL));
        g_assert_null (strstr (text, "secret"));
        g_free (text);
        text = uc_uri_policy_loggable (long_uri->str, FALSE);
        g_assert_cmpstr (text, ==, "https://example.com");
        g_free (text);
        g_string_free (long_uri, TRUE);
    }
    {
        /* a host name of 250 bytes is cut as well */
        GString *long_host = g_string_new ("https://");
        char *text;
        for (int i = 0; i < 25; i++)
            g_string_append (long_host, "abcdefghi.");
        g_string_append (long_host, "example/x");
        text = uc_uri_policy_loggable (long_host->str, FALSE);
        g_assert_cmpuint (strlen (text), <=, 200 + 3);
        g_assert_true (g_str_has_suffix (text, "..."));
        g_free (text);
        g_string_free (long_host, TRUE);
    }
}

static void
expect_scrubbed (const char *text, gboolean full, const char *expected)
{
    char *scrubbed = uc_uri_policy_scrub_text (text, full);
    g_assert_cmpstr (scrubbed, ==, expected);
    g_free (scrubbed);
}

static void
test_scrub_text (void)
{
    /* messages without a URI stay as they are, host names included */
    expect_scrubbed ("Error resolving \xe2\x80\x9cnonexistent-host.invalid\xe2\x80\x9d: Name or service not known", FALSE,
                     "Error resolving \xe2\x80\x9cnonexistent-host.invalid\xe2\x80\x9d: Name or service not known");
    expect_scrubbed ("Could not connect to 127.0.0.1: Connection refused", FALSE,
                     "Could not connect to 127.0.0.1: Connection refused");
    expect_scrubbed ("Unacceptable TLS certificate", FALSE, "Unacceptable TLS certificate");
    expect_scrubbed ("talk about: this, see http: or data: later", FALSE, "talk about: this, see http: or data: later");
    expect_scrubbed ("", FALSE, "");
    expect_scrubbed (NULL, FALSE, "(null)");
    /* URIs become their loggable form, the sentence around them stays */
    expect_scrubbed ("Invalid URI https://user:pw@example.com:8443/secret/path?token=1: bad", FALSE,
                     "Invalid URI https://example.com:8443: bad");
    expect_scrubbed ("Invalid \xe2\x80\x9chttps\xe2\x80\x9d URI: https://example.com/a?b=c", FALSE,
                     "Invalid \xe2\x80\x9chttps\xe2\x80\x9d URI: https://example.com");
    expect_scrubbed ("Cannot load \xe2\x80\x9chttp://[::1]:80/x?y\xe2\x80\x9d.", FALSE,
                     "Cannot load \xe2\x80\x9chttp://[::1]:80\xe2\x80\x9d.");
    expect_scrubbed ("(see http://example.com/reset/abc).", FALSE, "(see http://example.com).");
    expect_scrubbed ("'https://a.example/x' and \"http://b.example/y\" <ws://c.example:9/z>", FALSE,
                     "'https://a.example' and \"http://b.example\" <ws://c.example:9>");
    expect_scrubbed ("at data:text/html,<p>secret</p> and javascript:alert(1) in file:///etc/passwd", FALSE,
                     "at data: and javascript: in file:");
    expect_scrubbed ("blob:https://example.com/0f1e-secret", FALSE, "blob:");
    /* libsoup's redirect error quotes the target, whatever its scheme */
    expect_scrubbed ("Invalid URI \xe2\x80\x9c" "ftp://files.example/secret?x\xe2\x80\x9d in Location response header", FALSE,
                     "Invalid URI \xe2\x80\x9c" "ftp://files.example\xe2\x80\x9d in Location response header");
    expect_scrubbed ("Invalid URI \xe2\x80\x9cintent:secret/path\xe2\x80\x9d in Location response header", FALSE,
                     "Invalid URI \xe2\x80\x9cintent:\xe2\x80\x9d in Location response header");
    expect_scrubbed ("Error loading data:text/html,<a\"b'c>\xe2\x80\x9dsecret</a>", FALSE, "Error loading data:");
    expect_scrubbed ("HTTP://Example.com/x", FALSE, "http://Example.com");
    /* full keeps path, query and fragment, never credentials */
    expect_scrubbed ("Invalid URI https://user:pw@example.com/p?q=1: bad", TRUE,
                     "Invalid URI https://example.com/p?q=1: bad");
}

static void
test_env_value (void)
{
    g_assert_true (uc_env_value_true ("1"));
    g_assert_true (uc_env_value_true ("true"));
    g_assert_true (uc_env_value_true ("YES"));
    g_assert_true (uc_env_value_true ("On"));
    g_assert_false (uc_env_value_true (NULL));
    g_assert_false (uc_env_value_true (""));
    g_assert_false (uc_env_value_true ("0"));
    g_assert_false (uc_env_value_true ("2"));
    g_assert_false (uc_env_value_true ("false"));
    g_assert_false (uc_env_value_true ("1 "));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/uri-policy/always-allowed", test_always_allowed);
    g_test_add_func ("/uri-policy/file", test_file);
    g_test_add_func ("/uri-policy/rejected", test_rejected);
    g_test_add_func ("/uri-policy/base", test_base);
    g_test_add_func ("/uri-policy/describe", test_describe);
    g_test_add_func ("/uri-policy/loggable", test_loggable);
    g_test_add_func ("/uri-policy/scrub-text", test_scrub_text);
    g_test_add_func ("/uri-policy/env-value", test_env_value);
    return g_test_run ();
}
