/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_load_error.c (GLib/GIO only): run with `make check`. The reasons of load_failed_detail()
 * (library 0.10.0) for the GErrors WebKit 2.54 hands to load-failed (uc_load_error.h). When the host has WebKitGTK
 * 2.54 (libwebkit2gtk-4.1.so.0 or libwebkitgtk-6.0.so.4, loaded with GModule, no build dependency), WebKit's own
 * WEBKIT_NETWORK_ERROR domain string is also checked against the one the mapping compares with.
 */
#include "uc_load_error.h"

#include "uc_webview.h"

#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>

#define RESOLVER "g-resolver-error-quark"
#define IO       "g-io-error-quark"
#define TLS      "g-tls-error-quark"

/* the strings the GErrors carry are the quark names GLib registers (no hand-written list) */
static void
test_domain_names (void)
{
    g_assert_cmpstr (g_quark_to_string (G_RESOLVER_ERROR), ==, RESOLVER);
    g_assert_cmpstr (g_quark_to_string (G_IO_ERROR), ==, IO);
    g_assert_cmpstr (g_quark_to_string (G_TLS_ERROR), ==, TLS);
}

static void
test_resolver (void)
{
    g_assert_cmpint (uc_load_error_reason (RESOLVER, G_RESOLVER_ERROR_NOT_FOUND, 0), ==, UC_WEBVIEW_FAIL_HOST_NOT_FOUND);
    /* "Temporary failure in name resolution": also the answer without any network */
    g_assert_cmpint (uc_load_error_reason (RESOLVER, G_RESOLVER_ERROR_TEMPORARY_FAILURE, 0), ==,
                     UC_WEBVIEW_FAIL_HOST_NOT_FOUND);
    g_assert_cmpint (uc_load_error_reason (RESOLVER, G_RESOLVER_ERROR_INTERNAL, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason (RESOLVER, 99, 0), ==, UC_WEBVIEW_FAIL_OTHER);
}

static void
test_io (void)
{
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_HOST_NOT_FOUND, 0), ==, UC_WEBVIEW_FAIL_HOST_NOT_FOUND);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_NETWORK_UNREACHABLE, 0), ==, UC_WEBVIEW_FAIL_NO_NETWORK);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_HOST_UNREACHABLE, 0), ==, UC_WEBVIEW_FAIL_NO_NETWORK);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_CONNECTION_REFUSED, 0), ==, UC_WEBVIEW_FAIL_CONNECTION_REFUSED);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_TIMED_OUT, 0), ==, UC_WEBVIEW_FAIL_TIMEOUT);
    /* the rest of GIO is not a reason of its own */
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_FAILED, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    /* the 10 s preconnect timeout of WebKit's network process cancels the main request's connection */
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_CANCELLED, 0), ==, UC_WEBVIEW_FAIL_TIMEOUT);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_CONNECTION_CLOSED, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_PROXY_FAILED, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_NOT_FOUND, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    /* the codes of one domain mean nothing in another (G_IO_ERROR_TIMED_OUT is 24) */
    g_assert_cmpint (uc_load_error_reason (RESOLVER, G_IO_ERROR_TIMED_OUT, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason ("SoupSessionError", G_IO_ERROR_CONNECTION_REFUSED, 0), ==,
                     UC_WEBVIEW_FAIL_OTHER);
}

static void
test_tls (void)
{
    /* the certificate path: load-failed-with-tls-errors' flags decide, whatever the domain */
    g_assert_cmpint (uc_load_error_reason (TLS, G_TLS_ERROR_BAD_CERTIFICATE, G_TLS_CERTIFICATE_UNKNOWN_CA), ==,
                     UC_WEBVIEW_FAIL_CERTIFICATE);
    g_assert_cmpint (uc_load_error_reason ("", 0, G_TLS_CERTIFICATE_EXPIRED), ==, UC_WEBVIEW_FAIL_CERTIFICATE);
    g_assert_cmpint (uc_load_error_reason (IO, G_IO_ERROR_FAILED, G_TLS_CERTIFICATE_BAD_IDENTITY), ==,
                     UC_WEBVIEW_FAIL_CERTIFICATE);
    /* a TLS error without flags (handshake, not TLS) */
    g_assert_cmpint (uc_load_error_reason (TLS, G_TLS_ERROR_BAD_CERTIFICATE, 0), ==, UC_WEBVIEW_FAIL_CERTIFICATE);
    g_assert_cmpint (uc_load_error_reason (TLS, G_TLS_ERROR_HANDSHAKE, 0), ==, UC_WEBVIEW_FAIL_CERTIFICATE);
}

static void
test_webkit (void)
{
    g_assert_cmpint (uc_load_error_reason (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN, -1001, 0), ==, UC_WEBVIEW_FAIL_TIMEOUT);
    /* WEBKIT_NETWORK_ERROR_FAILED, _TRANSPORT, _UNKNOWN_PROTOCOL, _FILE_DOES_NOT_EXIST */
    g_assert_cmpint (uc_load_error_reason (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN, 399, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN, 300, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN, 301, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN, 303, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    /* -1001 of another domain, policy errors */
    g_assert_cmpint (uc_load_error_reason ("WebKitPolicyError", -1001, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason ("WebKitPolicyError", 103, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    /* WEBKIT_NETWORK_ERROR_CANCELLED (302) is not a timeout (uc_webview.c reports no FAILED for it anyway) */
    g_assert_cmpint (uc_load_error_reason (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN, 302, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN, G_IO_ERROR_CANCELLED, 0), ==,
                     UC_WEBVIEW_FAIL_OTHER);
}

static void
test_no_domain (void)
{
    g_assert_cmpint (uc_load_error_reason (NULL, 0, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason ("", G_IO_ERROR_TIMED_OUT, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    /* prefixes and case are other domains */
    g_assert_cmpint (uc_load_error_reason ("g-io-error", G_IO_ERROR_TIMED_OUT, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason ("G-IO-ERROR-QUARK", G_IO_ERROR_TIMED_OUT, 0), ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpint (uc_load_error_reason ("webkitnetworkerror", -1001, 0), ==, UC_WEBVIEW_FAIL_OTHER);
}

/* the arguments of load_failed_detail() from a GError, as WebKit builds it (WebKitNavigationClient.cpp:66-67) */
static void
test_from_error (void)
{
    UcLoadFailure f;
    GError *e;

    e = g_error_new_literal (G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE,
                             "Error resolving \xe2\x80\x9cwww.example.com\xe2\x80\x9d: Temporary failure in name resolution");
    memset (&f, 0xa5, sizeof f);
    uc_load_failure_from_error (&f, e, 0);
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_HOST_NOT_FOUND);
    g_assert_cmpstr (f.domain, ==, RESOLVER);
    g_assert_cmpint (f.code, ==, 1);
    g_assert_cmpuint (f.tls_errors, ==, 0);
    g_assert_true (f.message == e->message);
    g_error_free (e);

    /* the TLS path: the flags are kept */
    e = g_error_new_literal (G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE, "Unacceptable TLS certificate");
    uc_load_failure_from_error (&f, e, G_TLS_CERTIFICATE_UNKNOWN_CA | G_TLS_CERTIFICATE_BAD_IDENTITY);
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_CERTIFICATE);
    g_assert_cmpstr (f.domain, ==, TLS);
    g_assert_cmpint (f.code, ==, G_TLS_ERROR_BAD_CERTIFICATE);
    g_assert_cmpuint (f.tls_errors, ==, G_TLS_CERTIFICATE_UNKNOWN_CA | G_TLS_CERTIFICATE_BAD_IDENTITY);
    g_assert_cmpstr (f.message, ==, "Unacceptable TLS certificate");
    g_error_free (e);

    /* WebKit's request timeout: code -1001 survives toWebKitError()'s unsigned round trip (WebKitPrivate.cpp:180-182) */
    e = g_error_new_literal (g_quark_from_static_string (UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN),
                             (int) (unsigned) -1001, "Request timed out");
    uc_load_failure_from_error (&f, e, 0);
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_TIMEOUT);
    g_assert_cmpint (f.code, ==, -1001);
    g_error_free (e);

    e = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "Could not connect to 127.0.0.1: Connection refused");
    uc_load_failure_from_error (&f, e, 0);
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_CONNECTION_REFUSED);
    g_assert_cmpstr (f.domain, ==, IO);
    g_assert_cmpint (f.code, ==, G_IO_ERROR_CONNECTION_REFUSED);
    g_error_free (e);

    /* NULL error, NULL message */
    uc_load_failure_from_error (&f, NULL, 0);
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_OTHER);
    g_assert_cmpstr (f.domain, ==, "");
    g_assert_cmpint (f.code, ==, 0);
    g_assert_cmpstr (f.message, ==, "");
    e = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "x");
    g_free (e->message);
    e->message = NULL;
    uc_load_failure_from_error (&f, e, 0);
    g_assert_cmpstr (f.message, ==, "");
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_OTHER);
    g_error_free (e);

    /* flags with another reason are impossible: any flag is CERTIFICATE, and 0 stays 0 for the others */
    e = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Socket I/O timed out");
    uc_load_failure_from_error (&f, e, 0);
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_TIMEOUT);
    g_assert_cmpuint (f.tls_errors, ==, 0);
    g_error_free (e);
}

static void
test_scheme_refused (void)
{
    UcLoadFailure f;

    memset (&f, 0xa5, sizeof f);
    uc_load_failure_scheme_refused (&f);
    g_assert_cmpint (f.reason, ==, UC_WEBVIEW_FAIL_SCHEME_REFUSED);
    g_assert_cmpstr (f.domain, ==, "");
    g_assert_cmpint (f.code, ==, 0);
    g_assert_cmpuint (f.tls_errors, ==, 0);
    g_assert_cmpstr (f.message, ==, "");
}

/* the values of the header, which the host compiles in (uc_webview.h) */
static void
test_values (void)
{
    g_assert_cmpint (UC_WEBVIEW_FAIL_OTHER, ==, 0);
    g_assert_cmpint (UC_WEBVIEW_FAIL_HOST_NOT_FOUND, ==, 1);
    g_assert_cmpint (UC_WEBVIEW_FAIL_NO_NETWORK, ==, 2);
    g_assert_cmpint (UC_WEBVIEW_FAIL_CONNECTION_REFUSED, ==, 3);
    g_assert_cmpint (UC_WEBVIEW_FAIL_TIMEOUT, ==, 4);
    g_assert_cmpint (UC_WEBVIEW_FAIL_CERTIFICATE, ==, 5);
    g_assert_cmpint (UC_WEBVIEW_FAIL_SCHEME_REFUSED, ==, 6);
}

/* WebKit's own quark: webkit_network_error_quark() registers UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN (WebKitError.cpp) */
static void
test_webkit_domain (void)
{
    static const char *const names[] = { "libwebkit2gtk-4.1.so.0", "libwebkitgtk-6.0.so.4", NULL };
    GQuark (*network_error_quark) (void) = NULL;
    GModule *module = NULL;

    for (int i = 0; names[i] != NULL && module == NULL; i++)
        module = g_module_open (names[i], G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    if (module == NULL || !g_module_symbol (module, "webkit_network_error_quark", (gpointer *) &network_error_quark)) {
        if (module != NULL)
            g_module_close (module);
        g_test_skip ("no WebKitGTK on this host");
        return;
    }
    g_assert_cmpstr (g_quark_to_string (network_error_quark ()), ==, UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN);
    g_module_close (module);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/load-error/domain-names", test_domain_names);
    g_test_add_func ("/load-error/resolver", test_resolver);
    g_test_add_func ("/load-error/io", test_io);
    g_test_add_func ("/load-error/tls", test_tls);
    g_test_add_func ("/load-error/webkit", test_webkit);
    g_test_add_func ("/load-error/no-domain", test_no_domain);
    g_test_add_func ("/load-error/from-error", test_from_error);
    g_test_add_func ("/load-error/scheme-refused", test_scheme_refused);
    g_test_add_func ("/load-error/values", test_values);
    g_test_add_func ("/load-error/webkit-domain", test_webkit_domain);
    return g_test_run ();
}
