/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Why a main-frame load failed (library 0.10.0), see uc_load_error.h.
 */
#include "uc_load_error.h"

#include "uc_webview.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
domain_is (const char *domain, GQuark quark)
{
    return domain != NULL && strcmp (domain, g_quark_to_string (quark)) == 0;
}

int
uc_load_error_reason (const char *domain, int code, unsigned tls_errors)
{
    if (tls_errors != 0)
        return UC_WEBVIEW_FAIL_CERTIFICATE;
    if (domain == NULL || *domain == '\0')
        return UC_WEBVIEW_FAIL_OTHER;
    if (domain_is (domain, G_RESOLVER_ERROR)) {
        /* TEMPORARY_FAILURE is also what the resolver says without a network (no DNS server reachable) */
        if (code == G_RESOLVER_ERROR_NOT_FOUND || code == G_RESOLVER_ERROR_TEMPORARY_FAILURE)
            return UC_WEBVIEW_FAIL_HOST_NOT_FOUND;
        return UC_WEBVIEW_FAIL_OTHER;
    }
    if (domain_is (domain, G_IO_ERROR)) {
        switch (code) {
        case G_IO_ERROR_HOST_NOT_FOUND:
            return UC_WEBVIEW_FAIL_HOST_NOT_FOUND;
        case G_IO_ERROR_NETWORK_UNREACHABLE:
        case G_IO_ERROR_HOST_UNREACHABLE:
            return UC_WEBVIEW_FAIL_NO_NETWORK;
        case G_IO_ERROR_CONNECTION_REFUSED:
            return UC_WEBVIEW_FAIL_CONNECTION_REFUSED;
        case G_IO_ERROR_TIMED_OUT:
        /* a host that never answers: the main request takes over the connection of WebKit's preconnect, which
         * WebKit cancels after 10 s (uc_load_error.h); WebKit's own cancellations are WEBKIT_NETWORK_ERROR */
        case G_IO_ERROR_CANCELLED:
            return UC_WEBVIEW_FAIL_TIMEOUT;
        default:
            return UC_WEBVIEW_FAIL_OTHER;
        }
    }
    if (domain_is (domain, G_TLS_ERROR))
        return UC_WEBVIEW_FAIL_CERTIFICATE;
    if (strcmp (domain, UC_LOAD_ERROR_WEBKIT_NETWORK_DOMAIN) == 0 && code == UC_LOAD_ERROR_WEBKIT_TIMEOUT)
        return UC_WEBVIEW_FAIL_TIMEOUT;
    return UC_WEBVIEW_FAIL_OTHER;
}

void
uc_load_failure_from_error (UcLoadFailure *failure, const GError *error, unsigned tls_errors)
{
    const char *domain = error != NULL ? g_quark_to_string (error->domain) : NULL;

    failure->domain = domain != NULL ? domain : "";
    failure->code = error != NULL ? error->code : 0;
    failure->message = error != NULL && error->message != NULL ? error->message : "";
    failure->reason = uc_load_error_reason (failure->domain, failure->code, tls_errors);
    failure->tls_errors = failure->reason == UC_WEBVIEW_FAIL_CERTIFICATE ? tls_errors : 0;
}

void
uc_load_failure_scheme_refused (UcLoadFailure *failure)
{
    failure->reason = UC_WEBVIEW_FAIL_SCHEME_REFUSED;
    failure->domain = "";
    failure->code = 0;
    failure->tls_errors = 0;
    failure->message = "";
}
