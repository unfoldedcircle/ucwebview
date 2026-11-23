/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Site settings of library 0.8.0, the WebKit side; see uc_site_wk.h.
 */
#include "uc_site_wk.h"

#include "uc_load_error.h"
#include "uc_uri_policy.h"

#include <libsoup/soup.h>
#include <string.h>

/* ---- done callbacks ------------------------------------------------------------------------------------------- */

typedef struct {
    uc_webview_done_cb cb;
    void *user;
    int status;
} Done;

static gboolean
done_idle (gpointer data)
{
    Done *done = data;
    done->cb (done->status, done->user);
    g_free (done);
    return G_SOURCE_REMOVE;
}

/* From an idle: never inside a uc_webview_* call, whatever WebKit and GTask do with their completions. */
static void
done_later (uc_webview_done_cb cb, void *user, int status)
{
    Done *done;

    if (cb == NULL)
        return;
    done = g_new0 (Done, 1);
    done->cb = cb;
    done->user = user;
    done->status = status;
    g_idle_add_full (G_PRIORITY_DEFAULT, done_idle, done, NULL);
}

/* ---- cookies ---------------------------------------------------------------------------------------------- */

typedef struct {
    guint pending;
    guint failed;
    guint total;
    uc_webview_done_cb cb;
    void *user;
    gboolean log_full_urls;
} CookieBatch;

static void
on_cookie_added (GObject *object, GAsyncResult *result, gpointer user_data)
{
    CookieBatch *batch = user_data;
    GError *error = NULL;

    if (!webkit_cookie_manager_add_cookie_finish (WEBKIT_COOKIE_MANAGER (object), result, &error)) {
        char *message = uc_uri_policy_scrub_text (error != NULL ? error->message : "unknown error",
                                                  batch->log_full_urls);
        batch->failed++;
        g_warning ("ucwebview: a cookie was not set: %s", message);
        g_free (message);
        g_clear_error (&error);
    }
    if (--batch->pending > 0)
        return;
    g_message ("ucwebview: %u cookie(s) set%s", batch->total - batch->failed, batch->failed > 0 ? ", some failed" : "");
    done_later (batch->cb, batch->user, batch->failed > 0 ? UC_WEBVIEW_DONE_FAILED : UC_WEBVIEW_DONE_OK);
    g_free (batch);
}

static SoupCookie *
soup_cookie_from (const UcCookie *c)
{
    SoupCookie *cookie = soup_cookie_new (c->name, c->value, c->domain, c->path, -1);

    soup_cookie_set_secure (cookie, c->secure);
    soup_cookie_set_http_only (cookie, c->http_only);
    if (c->expires >= 0) {
        /* an expiry time makes it a persistent cookie (CookieSoup.cpp: session = !expires), written to the
         * cookie database of a persistent session; a time in the past deletes the cookie */
        GDateTime *expires = g_date_time_new_from_unix_utc (c->expires);
        soup_cookie_set_expires (cookie, expires);
        g_date_time_unref (expires);
    }
    return cookie;
}

int
uc_site_wk_set_cookies (WebKitCookieManager *manager, const char *url, const uc_webview_cookie *cookies, gsize count,
                        uc_webview_done_cb cb, void *user, gboolean log_full_urls)
{
    UcCookie *checked;
    CookieBatch *batch;
    char *loggable = uc_uri_policy_loggable (url, log_full_urls);
    int result = 0;

    if (count > UC_SITE_COOKIE_MAX_COUNT || (count > 0 && cookies == NULL)) {
        g_warning ("ucwebview: set_cookies(%s) refused: %s", loggable,
                   count > 0 && cookies == NULL ? "no cookies" : "more than 64 cookies");
        g_free (loggable);
        return -1;
    }
    if (count == 0) {
        /* the URL still has to be one the cookies could be for */
        UcCookie probe;
        const char *reason = NULL;
        if (!uc_site_cookie_check (url, "probe", "", NULL, NULL, 0, 0, -1, &probe, &reason)
            && strstr (reason, "URL") != NULL) {
            g_warning ("ucwebview: set_cookies(%s) refused: %s", loggable, reason);
            g_free (loggable);
            return -1;
        }
        uc_site_cookie_clear (&probe);
        g_free (loggable);
        done_later (cb, user, UC_WEBVIEW_DONE_OK);
        return 0;
    }
    checked = g_new0 (UcCookie, count);
    for (gsize i = 0; i < count && result == 0; i++) {
        const uc_webview_cookie *c = &cookies[i];
        const char *reason = NULL;

        if (!uc_site_cookie_check (url, c->name, c->value, c->domain, c->path, c->secure, c->http_only, c->expires,
                                   &checked[i], &reason)) {
            /* the name is no secret, the value is never logged */
            g_warning ("ucwebview: set_cookies(%s) refused: cookie %" G_GSIZE_FORMAT " (%s): %s", loggable, i,
                       c->name != NULL && !strpbrk (c->name, "\r\n") ? c->name : "?", reason);
            result = -1;
        } else {
            const char *domain = checked[i].domain[0] == '.' ? checked[i].domain + 1 : checked[i].domain;
            if (!g_hostname_is_ip_address (domain) && soup_tld_domain_is_public_suffix (domain)) {
                g_warning ("ucwebview: set_cookies(%s) refused: cookie %" G_GSIZE_FORMAT " (%s): %s is a public suffix, "
                           "the cookie jar takes no cookie for it", loggable, i, checked[i].name, domain);
                result = -1;
            }
        }
    }
    if (result == 0) {
        batch = g_new0 (CookieBatch, 1);
        batch->pending = batch->total = (guint) count;
        batch->cb = cb;
        batch->user = user;
        batch->log_full_urls = log_full_urls;
        g_message ("ucwebview: setting %" G_GSIZE_FORMAT " cookie(s) for %s", count, loggable);
        for (gsize i = 0; i < count; i++) {
            SoupCookie *cookie = soup_cookie_from (&checked[i]);
            /* WebKit copies the cookie (WebKitCookieManager.cpp:291-300, WebCore::Cookie(SoupCookie*)) and the
             * network process puts it into the session's jar without a first-party check
             * (WebCookieManager.cpp:128-137, NetworkStorageSessionSoup.cpp:500-503) */
            webkit_cookie_manager_add_cookie (manager, cookie, NULL, on_cookie_added, batch);
            soup_cookie_free (cookie);
        }
    }
    for (gsize i = 0; i < count; i++)
        uc_site_cookie_clear (&checked[i]);
    g_free (checked);
    g_free (loggable);
    return result;
}

/* ---- website data ----------------------------------------------------------------------------------------- */

typedef struct {
    uc_webview_done_cb cb;
    void *user;
    const char *what;
} ClearRequest;

static void
on_data_cleared (GObject *object, GAsyncResult *result, gpointer user_data)
{
    ClearRequest *request = user_data;
    GError *error = NULL;
    gboolean ok = webkit_website_data_manager_clear_finish (WEBKIT_WEBSITE_DATA_MANAGER (object), result, &error);

    if (ok) {
        g_message ("ucwebview: website data cleared: %s", request->what);
    } else {
        /* a clear request has no URI, the switch UC_WEBVIEW_LOG_FULL_URLS does not matter here */
        char *message = uc_uri_policy_scrub_text (error != NULL ? error->message : "unknown error", FALSE);
        g_warning ("ucwebview: clearing website data (%s) failed: %s", request->what, message);
        g_free (message);
        g_clear_error (&error);
    }
    done_later (request->cb, request->user, ok ? UC_WEBVIEW_DONE_OK : UC_WEBVIEW_DONE_FAILED);
    g_free (request);
}

int
uc_site_wk_clear_data (WebKitWebsiteDataManager *manager, unsigned types, uc_webview_done_cb cb, void *user)
{
    ClearRequest *request;
    WebKitWebsiteDataTypes wk_types;

    if (!uc_site_clear_types_valid (types)) {
        g_warning ("ucwebview: clear_data(0x%x) refused: expected UC_WEBVIEW_DATA_CACHE or UC_WEBVIEW_DATA_ALL", types);
        return -1;
    }
    request = g_new0 (ClearRequest, 1);
    request->cb = cb;
    request->user = user;
    if (types == UC_SITE_DATA_CACHE) {
        /* the memory cache also drops the back/forward cache and cached web processes (WebsiteDataStore.cpp:937-945) */
        wk_types = WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE;
        request->what = "the memory and disk cache";
    } else {
        wk_types = WEBKIT_WEBSITE_DATA_ALL;
        request->what = "all";
    }
    g_message ("ucwebview: clearing website data: %s", request->what);
    /* timespan 0: everything, cookies included (with a timespan WebKit keeps the cookies,
     * WebKitWebsiteDataManager.cpp:1323-1338) */
    webkit_website_data_manager_clear (manager, wk_types, 0, NULL, on_data_cleared, request);
    return 0;
}

/* ---- certificate exceptions ------------------------------------------------------------------------------ */

#define TLS_RETRY_WINDOW_US (10 * G_USEC_PER_SEC)
#define TLS_RETRY_MAX       2       /* loads the library repeats for one URI and certificate within the window */

/* The loop guard (library 0.8.3: keyed on the certificate too). A load-balanced host that shows another certificate
 * with an exception of its own is accepted again; the same certificate is tried twice, because allow() goes
 * UI process -> network process while the repeated load goes UI -> web -> network process on another connection and
 * can arrive first once. */
typedef struct {
    char *uri;
    char *sha256;
    gint64 at;
    guint attempts;
} TlsRetry;

static void
tls_retry_free (gpointer data)
{
    TlsRetry *retry = data;
    g_free (retry->uri);
    g_free (retry->sha256);
    g_free (retry);
}

/* ---- loads the library repeats ------------------------------------------------------------------------- */

/* Per web view (object data): the navigations since the view was created, for the loads the library repeats from an
 * idle (library 0.10.3: a navigation that comes first wins), the time of the last STARTED and the loop guard of the
 * repeated cancelled loads (uc_load_error.h). */
#define LOADS_KEY "ucwebview-loads"

typedef struct {
    guint navigation;
    gint64 started_us;      /* g_get_monotonic_time() of the last STARTED, 0 before the first */
    UcLoadRetries cancelled;
} Loads;

static void
loads_free (gpointer data)
{
    Loads *loads = data;
    uc_load_retries_clear (&loads->cancelled);
    g_free (loads);
}

static Loads *
loads_of (WebKitWebView *web_view)
{
    Loads *loads = g_object_get_data (G_OBJECT (web_view), LOADS_KEY);

    if (loads == NULL) {
        loads = g_new0 (Loads, 1);
        g_object_set_data_full (G_OBJECT (web_view), LOADS_KEY, loads, loads_free);
    }
    return loads;
}

void
uc_site_wk_navigation (WebKitWebView *web_view, gboolean started)
{
    Loads *loads = loads_of (web_view);

    loads->navigation++;
    if (started)
        loads->started_us = g_get_monotonic_time ();
}

typedef struct {
    WebKitWebView *web_view;
    char *uri;
    char *loggable;
    guint navigation;
} Reload;

static gboolean
reload_idle (gpointer data)
{
    Reload *reload = data;

    /* the owner clears "ucwebview" in uc_webview_free(): a freed view loads nothing */
    if (g_object_get_data (G_OBJECT (reload->web_view), "ucwebview") == NULL) {
        /* nothing */
    } else if (loads_of (reload->web_view)->navigation != reload->navigation) {
        /* the host navigated, or another load started, after the failure (from its FINISHED callback, say): that
         * navigation stays (library 0.10.3; before, the repeated load replaced it) */
        g_message ("ucwebview: %s: not loaded again, another navigation came first", reload->loggable);
    } else {
        webkit_web_view_load_uri (reload->web_view, reload->uri);
    }
    g_object_unref (reload->web_view);
    g_free (reload->uri);
    g_free (reload->loggable);
    g_free (reload);
    return G_SOURCE_REMOVE;
}

/* Not inside the signal: WebKit emits FINISHED of the failed load after it (WebKitWebView.cpp:2774-2801), the new
 * load starts after that, from an idle. */
static void
reload_later (WebKitWebView *web_view, const char *uri, const char *loggable)
{
    Reload *reload = g_new0 (Reload, 1);

    reload->web_view = g_object_ref (web_view);
    reload->uri = g_strdup (uri);
    reload->loggable = g_strdup (loggable);
    reload->navigation = loads_of (web_view)->navigation;
    g_idle_add_full (G_PRIORITY_DEFAULT, reload_idle, reload, NULL);
}

gboolean
uc_site_wk_load_cancelled (WebKitWebView *web_view, WebKitLoadEvent event, const char *failing_uri,
                           const GError *error, gboolean log_full_urls)
{
    Loads *loads = loads_of (web_view);
    gint64 now = g_get_monotonic_time ();
    gint64 since = loads->started_us > 0 ? now - loads->started_us : -1;
    char *loggable;
    gboolean again;

    if (error == NULL
        || !uc_load_error_cancelled_early (failing_uri, g_quark_to_string (error->domain), error->code,
                                           event == WEBKIT_LOAD_STARTED, since))
        return FALSE;
    loggable = uc_uri_policy_loggable (failing_uri, log_full_urls);
    again = uc_load_retries_take (&loads->cancelled, failing_uri, now);
    if (again) {
        g_message ("ucwebview: %s: load cancelled %" G_GINT64_FORMAT " ms after it started, on the connection of "
                   "WebKit's preconnect, which had failed on the server's certificate; loading again", loggable,
                   since / 1000);
        reload_later (web_view, failing_uri, loggable);
    } else {
        g_message ("ucwebview: %s: load cancelled %" G_GINT64_FORMAT " ms after it started, again after %d repeated "
                   "loads within %d s; not loading it again", loggable, since / 1000, UC_LOAD_RETRIES_MAX,
                   (int) (UC_LOAD_RETRIES_WINDOW_US / G_USEC_PER_SEC));
    }
    g_free (loggable);
    return again;
}

char *
uc_site_wk_certificate_sha256 (GTlsCertificate *certificate)
{
    GByteArray *der = NULL;
    char *sha256;

    if (certificate == NULL)
        return NULL;
    g_object_get (certificate, "certificate", &der, NULL);
    if (der == NULL)
        return NULL;
    sha256 = g_compute_checksum_for_data (G_CHECKSUM_SHA256, der->data, der->len);
    g_byte_array_unref (der);
    return sha256;
}

gboolean
uc_site_wk_tls_error (WebKitWebView *web_view, const char *failing_uri, GTlsCertificate *certificate,
                      GTlsCertificateFlags errors, const UcTlsExceptions *tls, UcSiteWkAllowFunc allow,
                      gpointer allow_data, gboolean log_full_urls)
{
    GUri *uri = failing_uri != NULL ? g_uri_parse (failing_uri, G_URI_FLAGS_NONE, NULL) : NULL;
    char *host = uri != NULL ? uc_site_host_normalize (g_uri_get_host (uri)) : NULL;
    char *sha256 = uc_site_wk_certificate_sha256 (certificate);
    char *loggable = uc_uri_policy_loggable (failing_uri, log_full_urls);
    gboolean any = FALSE, handled = FALSE;
    TlsRetry *retry = g_object_get_data (G_OBJECT (web_view), "ucwebview-tls-retry");

    if (uri != NULL)
        g_uri_unref (uri);
    if (host == NULL || certificate == NULL) {
        /* nothing to match against: WebKit's own failure */
    } else if (!uc_tls_exceptions_match (tls, host, sha256, &any)) {
        g_message ("ucwebview: %s: the TLS certificate of %s is not valid (errors 0x%x) and has no exception; its "
                   "SHA-256 fingerprint is %s", loggable, host, (unsigned) errors, sha256 != NULL ? sha256 : "unknown");
    } else if (retry != NULL && g_strcmp0 (retry->uri, failing_uri) == 0 && g_strcmp0 (retry->sha256, sha256) == 0
               && g_get_monotonic_time () - retry->at < TLS_RETRY_WINDOW_US && retry->attempts >= TLS_RETRY_MAX) {
        /* accepted twice a moment ago and failed again with the same certificate: WebKit does not take the exception
         * (a network process that restarted, ...); no loop */
        g_warning ("ucwebview: %s: the TLS certificate of %s failed again after its exception was applied (errors "
                   "0x%x, SHA-256 %s)", loggable, host, (unsigned) errors, sha256 != NULL ? sha256 : "unknown");
    } else {
        g_message ("ucwebview: %s: the TLS certificate of %s is not valid (errors 0x%x), accepted by %s; loading "
                   "again", loggable, host, (unsigned) errors,
                   any ? "the exception for any certificate of the host" : "the exception for its SHA-256 fingerprint");
        /* WebKit keeps it per host for the session (SoupNetworkSession.cpp:334-349) */
        allow (certificate, host, allow_data);
        if (retry != NULL && g_strcmp0 (retry->uri, failing_uri) == 0 && g_strcmp0 (retry->sha256, sha256) == 0
            && g_get_monotonic_time () - retry->at < TLS_RETRY_WINDOW_US) {
            retry->attempts++;
        } else {
            retry = g_new0 (TlsRetry, 1);
            retry->uri = g_strdup (failing_uri);
            retry->sha256 = g_strdup (sha256);
            retry->at = g_get_monotonic_time ();
            retry->attempts = 1;
            g_object_set_data_full (G_OBJECT (web_view), "ucwebview-tls-retry", retry, tls_retry_free);
        }
        reload_later (web_view, failing_uri, loggable);
        handled = TRUE;
    }
    g_free (loggable);
    g_free (sha256);
    g_free (host);
    return handled;
}

/* ---- origin headers for the web process extension ------------------------------------------------------- */

struct _UcSiteWkHeaders {
    WebKitWebContext *context;      /* a reference */
    UcHeadersTable *merged;
    GVariant *published;            /* the origins of the last table sent, without the generation */
    guint32 generation;
    guint64 seq;
};

UcSiteWkHeaders *
uc_site_wk_headers_new (WebKitWebContext *context)
{
    UcSiteWkHeaders *headers = g_new0 (UcSiteWkHeaders, 1);
    headers->context = g_object_ref (context);
    headers->merged = uc_headers_table_new ();
    headers->generation = 1;
    return headers;
}

void
uc_site_wk_headers_free (UcSiteWkHeaders *headers)
{
    if (headers == NULL)
        return;
    g_clear_pointer (&headers->published, g_variant_unref);
    uc_headers_table_free (headers->merged);
    g_object_unref (headers->context);
    g_free (headers);
}

guint64
uc_site_wk_headers_next_seq (UcSiteWkHeaders *headers)
{
    return ++headers->seq;
}

void
uc_site_wk_headers_publish (UcSiteWkHeaders *headers, GPtrArray *views)
{
    GVariant *table, *origins;

    uc_headers_table_clear (headers->merged);
    for (guint i = 0; views != NULL && i < views->len; i++)
        if (g_ptr_array_index (views, i) != NULL)
            uc_headers_table_merge (headers->merged, g_ptr_array_index (views, i));
    table = g_variant_ref_sink (uc_headers_table_serialize (headers->merged, headers->generation + 1));
    origins = g_variant_get_child_value (table, 1);
    if (headers->published != NULL ? g_variant_equal (origins, headers->published)
                                   : g_variant_n_children (origins) == 0) {
        g_variant_unref (origins);
        g_variant_unref (table);
        return;
    }
    headers->generation++;
    g_clear_pointer (&headers->published, g_variant_unref);
    headers->published = origins;
    /* to every running web process, prewarmed and cached ones included (WebKitWebContext.cpp:1936-1945); messages
     * to one process keep their order with the page's loads over the same connection */
    webkit_web_context_send_message_to_all_extensions (headers->context,
                                                       webkit_user_message_new (UC_HEADERS_MESSAGE, table));
    g_variant_unref (table);
}

GVariant *
uc_site_wk_headers_user_data (UcSiteWkHeaders *headers)
{
    return uc_headers_table_serialize (headers->merged, headers->generation);
}
