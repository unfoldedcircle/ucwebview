/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --site-test of ucwebview-test, see ucwebview-test-site.h.
 *
 * The test is a script: it calls the library like a host, then iterates the main context until the page reported
 * its result in its title ("done:...") or the load finished, and compares what the servers recorded and what the
 * page saw with what the step expects. The servers (libsoup, in this process' main context):
 *   http://127.0.0.1:P   the site under test (origin headers, cookies, user agent, color scheme)
 *   http://localhost:P   another site (cross-site embedding, a form posted to the site, redirects)
 *   https://127.0.0.1:Q, https://localhost:Q, https://127.0.0.2:R  with --site-tls-cert: a self-signed certificate
 *                        for the certificate exceptions (127.0.0.2: the TLS errors policy)
 * Nothing goes to the network. The certificate file holds a PEM certificate and its key, valid for localhost,
 * 127.0.0.1 and 127.0.0.2, e.g. made with
 *   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout key.pem -out cert.pem \
 *     -days 3650 -subj /CN=ucwebview-test -addext subjectAltName=DNS:localhost,IP:127.0.0.1,IP:127.0.0.2
 *   cat cert.pem key.pem > site-test.pem
 * The tool never prints a header value, cookie value or the token it sets, only whether the server got it.
 */
#include "ucwebview-test-site.h"

#include "uc_site.h"
#include "uc_webview.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- options ----------------------------------------------------------------------------------------------- */

static gboolean opt_site_test;
static char *opt_site_tls_cert;
static gboolean opt_site_expect_persisted;
static char *opt_site_steps;

const GOptionEntry site_test_entries[] = {
    { "site-test", 0, 0, G_OPTION_ARG_NONE, &opt_site_test,
      "Check the site settings of library 0.8.0 against pages of servers inside the tool: user agent, cookies, origin "
      "headers, certificate exceptions (with --site-tls-cert), color scheme, clearing website data; with --data-dir "
      "also that cookies are written to the data directory", NULL },
    { "site-tls-cert", 0, 0, G_OPTION_ARG_FILENAME, &opt_site_tls_cert,
      "PEM file with a self-signed certificate and its key for localhost, 127.0.0.1 and 127.0.0.2 (the TLS steps of "
      "--site-test; without it they are skipped)", "FILE" },
    { "site-expect-persisted", 0, 0, G_OPTION_ARG_NONE, &opt_site_expect_persisted,
      "With --site-test and --data-dir: fail unless the persistent cookie of an earlier run with the same directory "
      "is there", NULL },
    { "site-steps", 0, 0, G_OPTION_ARG_STRING, &opt_site_steps,
      "The --site-test steps, comma separated (default: all): ua, cookies, headers, tls, scheme, clear", "LIST" },
    { NULL }
};

static const char *const all_steps[] = { "ua", "cookies", "headers", "tls", "scheme", "clear" };

gboolean
site_test_mode (void)
{
    return opt_site_test;
}

gboolean
site_test_check_options (void)
{
    if (!opt_site_test) {
        if (opt_site_tls_cert != NULL || opt_site_expect_persisted || opt_site_steps != NULL) {
            g_printerr ("ucwebview-test: --site-tls-cert, --site-expect-persisted and --site-steps go with --site-test\n");
            return FALSE;
        }
        return TRUE;
    }
    if (opt_site_steps != NULL) {
        char **names = g_strsplit (opt_site_steps, ",", -1);
        for (char **n = names; *n != NULL; n++) {
            gboolean known = FALSE;
            for (gsize i = 0; i < G_N_ELEMENTS (all_steps); i++)
                known = known || g_strcmp0 (g_strstrip (*n), all_steps[i]) == 0;
            if (!known) {
                g_printerr ("ucwebview-test: unknown --site-steps entry \"%s\"\n", *n);
                g_strfreev (names);
                return FALSE;
            }
        }
        g_strfreev (names);
    }
    return TRUE;
}

int
site_test_default_timeout (void)
{
    return 120;
}

static gboolean
step_wanted (const char *name)
{
    gboolean wanted = FALSE;
    char **names;

    if (opt_site_steps == NULL)
        return TRUE;
    names = g_strsplit (opt_site_steps, ",", -1);
    for (char **n = names; *n != NULL; n++)
        wanted = wanted || g_strcmp0 (g_strstrip (*n), name) == 0;
    g_strfreev (names);
    return wanted;
}

/* ---- state ------------------------------------------------------------------------------------------------- */

/* what a server got: one request */
typedef struct {
    char *host;             /* Host header, "127.0.0.1:P" */
    char *path;
    char *method;
    char *authorization, *token, *cookie, *user_agent, *origin;
} Record;

static struct {
    uc_webview *wv;
    void *held;
    GPtrArray *events;      /* char*: "started", "committed", "finished", "failed" */
    char *title;
    gboolean terminated;
    SoupServer *server, *tls_server;
    guint16 port, tls_port, tls_port_other;
    char *site, *other;     /* http://127.0.0.1:P, http://localhost:P */
    char *site_host, *other_host;   /* "127.0.0.1:P", "localhost:P" */
    char *third, *third_host;       /* http://127.0.0.2:R, a third site for redirect chains; NULL without it */
    char *tls_cert_sha256;
    GPtrArray *log;         /* Record* */
    int pass, fail, skip;
    gint64 deadline;
    gboolean timed_out;
    int width, height;
    const char *data_dir;
    guint done_calls;
    int done_status;
} st;

static void
record_free (gpointer data)
{
    Record *r = data;
    g_free (r->host);
    g_free (r->path);
    g_free (r->method);
    g_free (r->authorization);
    g_free (r->token);
    g_free (r->cookie);
    g_free (r->user_agent);
    g_free (r->origin);
    g_free (r);
}

static void G_GNUC_PRINTF (3, 4)
check (gboolean ok, const char *name, const char *format, ...)
{
    va_list args;
    char *why;

    va_start (args, format);
    why = g_strdup_vprintf (format, args);
    va_end (args);
    g_print ("site-test: %s %s: %s\n", ok ? "PASS" : "FAIL", name, why);
    g_free (why);
    if (ok)
        st.pass++;
    else
        st.fail++;
}

static void G_GNUC_PRINTF (2, 3)
skip (const char *name, const char *format, ...)
{
    va_list args;
    char *why;

    va_start (args, format);
    why = g_strdup_vprintf (format, args);
    va_end (args);
    g_print ("site-test: SKIP %s: %s\n", name, why);
    g_free (why);
    st.skip++;
}

/* ---- the main context -------------------------------------------------------------------------------------- */

typedef gboolean (*Predicate) (gconstpointer data);

static gboolean
tick (gpointer data)
{
    (void) data;
    return G_SOURCE_CONTINUE;
}

/* Iterates the main context until pred holds, at most timeout_ms (and never past the run's deadline). */
static gboolean
spin_until (Predicate pred, gconstpointer data, guint timeout_ms)
{
    gint64 end = g_get_monotonic_time () + (gint64) timeout_ms * 1000;

    while (!pred (data)) {
        gint64 now = g_get_monotonic_time ();
        if (now >= st.deadline) {
            st.timed_out = TRUE;
            return FALSE;
        }
        if (now >= end || st.terminated)
            return FALSE;
        g_main_context_iteration (NULL, TRUE);
    }
    return TRUE;
}

static guint count_events (const char *name);

static gboolean
title_has_prefix (gconstpointer prefix)
{
    return st.title != NULL && g_str_has_prefix (st.title, prefix);
}

/* the new document committed and reported (not a late title of the previous one) */
static gboolean
committed_with_title (gconstpointer prefix)
{
    return count_events ("committed") > 0 && title_has_prefix (prefix);
}

static gboolean
title_is (gconstpointer title)
{
    return g_strcmp0 (st.title, title) == 0;
}

static guint
count_events (const char *name)
{
    guint n = 0;
    for (guint i = 0; i < st.events->len; i++)
        n += g_strcmp0 (g_ptr_array_index (st.events, i), name) == 0;
    return n;
}

static gboolean
finished_n (gconstpointer n)
{
    return count_events ("finished") >= GPOINTER_TO_UINT (n);
}

static gboolean
done_called (gconstpointer n)
{
    return st.done_calls >= GPOINTER_TO_UINT (n);
}

static gboolean
never (gconstpointer data)
{
    (void) data;
    return FALSE;
}

/* What the previous load still sends (WebKit loads an error page after a failed load: its commit and FINISHED come
 * after the failure's FINISHED) must not count for the next one. */
static void
settle (void)
{
    spin_until (never, NULL, 300);
}

/* Loads uri and waits for the page's "done:" title (pages of the servers) or, with wait_title NULL, for FINISHED. */
static gboolean
load (const char *uri, const char *wait_title)
{
    settle ();
    g_ptr_array_set_size (st.events, 0);
    g_clear_pointer (&st.title, g_free);
    uc_webview_load_uri (st.wv, uri);
    if (wait_title != NULL)
        return spin_until (committed_with_title, wait_title, 10000);
    return spin_until (finished_n, GUINT_TO_POINTER (1), 10000);
}

/* the last request a server got for host ("127.0.0.1:P") and path */
static const Record *
find (const char *host, const char *path)
{
    for (guint i = st.log->len; i > 0; i--) {
        const Record *r = g_ptr_array_index (st.log, i - 1);
        if (g_strcmp0 (r->host, host) == 0 && g_strcmp0 (r->path, path) == 0)
            return r;
    }
    return NULL;
}

/* "name=" in a Cookie header or document.cookie */
static gboolean
has_cookie (const char *cookies, const char *name)
{
    gboolean found = FALSE;
    char **parts;

    if (cookies == NULL)
        return FALSE;
    parts = g_strsplit (cookies, ";", -1);
    for (char **p = parts; *p != NULL && !found; p++) {
        const char *c = g_strstrip (*p);
        found = g_str_has_prefix (c, name) && c[strlen (name)] == '=';
    }
    g_strfreev (parts);
    return found;
}

/* Whether a cross-site main-frame navigation moves to another web process: always on WPE (WebKitWebContext.cpp:442-446);
 * in the WebKitGTK harness unless UC_HARNESS_PSON=0 (tests/webkitgtk-host.c). */
static gboolean
process_swaps (void)
{
    return g_strcmp0 (g_getenv ("UC_HARNESS_PSON"), "0") != 0;
}

/* ---- library callbacks ------------------------------------------------------------------------------------- */

static void
on_frame (uc_webview *wv, const uc_webview_frame *frame, void *user)
{
    (void) user;
    uc_webview_frame_rendered (wv, frame->handle);
    if (st.held != NULL)
        uc_webview_frame_release (wv, st.held);
    st.held = frame->handle;
}

static void
on_title (uc_webview *wv, const char *title, void *user)
{
    (void) wv; (void) user;
    g_free (st.title);
    st.title = g_strdup (title);
}

static void
on_load (uc_webview *wv, int event, const char *uri, void *user)
{
    static const char *const names[] = { "started", "redirected", "committed", "finished", "failed" };
    (void) wv; (void) uri; (void) user;
    if (event >= 0 && event < (int) G_N_ELEMENTS (names))
        g_ptr_array_add (st.events, (gpointer) names[event]);
}

static void
on_terminated (uc_webview *wv, int reason, void *user)
{
    (void) wv; (void) user;
    g_printerr ("site-test: web process terminated (reason %d)\n", reason);
    st.terminated = TRUE;
}

static void
on_done (int status, void *user)
{
    (void) user;
    st.done_calls++;
    st.done_status = status;
}

/* ---- servers ----------------------------------------------------------------------------------------------- */

static const char gif[] = "GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00\x00\x00\x00,"
                          "\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;";

static void
respond (SoupServerMessage *msg, const char *type, const char *body, gsize length)
{
    soup_message_headers_replace (soup_server_message_get_response_headers (msg), "Cache-Control", "no-store");
    soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response (msg, type, SOUP_MEMORY_COPY, body, length);
}

static void
respond_html (SoupServerMessage *msg, const char *html)
{
    respond (msg, "text/html; charset=utf-8", html, strlen (html));
}

/* a page that reports "done:<text>" */
static char *
done_page (const char *text)
{
    return g_strdup_printf ("<!doctype html><title>done:%s</title><body>%s</body>", text, text);
}

static void
serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data)
{
    SoupMessageHeaders *req = soup_server_message_get_request_headers (msg);
    Record *r = g_new0 (Record, 1);
    char *html = NULL;

    (void) server; (void) user_data;
    r->host = g_strdup (soup_message_headers_get_one (req, "Host"));
    r->path = g_strdup (path);
    r->method = g_strdup (soup_server_message_get_method (msg));
    r->authorization = g_strdup (soup_message_headers_get_one (req, "Authorization"));
    r->token = g_strdup (soup_message_headers_get_one (req, "X-Uc-Token"));
    r->cookie = g_strdup (soup_message_headers_get_one (req, "Cookie"));
    r->user_agent = g_strdup (soup_message_headers_get_one (req, "User-Agent"));
    r->origin = g_strdup (soup_message_headers_get_one (req, "Origin"));
    g_ptr_array_add (st.log, r);

    if (g_strcmp0 (path, "/ua") == 0) {
        respond_html (msg, "<!doctype html><title>ua</title><script>document.title='done:'+navigator.userAgent</script>");
    } else if (g_str_has_prefix (path, "/cookies")) {
        respond_html (msg, "<!doctype html><title>cookies</title><script>document.title='done:'+document.cookie</script>");
    } else if (g_strcmp0 (path, "/scheme") == 0) {
        respond_html (msg, "<!doctype html><title>scheme</title><script>var q=matchMedia('(prefers-color-scheme: dark)');"
                           "function u(){document.title='done:'+(q.matches?'dark':'light');}u();"
                           "q.addEventListener('change',u);</script>");
    } else if (g_strcmp0 (path, "/tls") == 0) {
        html = done_page ("tls");
    } else if (g_strcmp0 (path, "/headers") == 0) {
        /* the page, a script, an image, a fetch of its own origin, a no-cors fetch to the other site, and a frame of
         * the other site that fetches from this origin; done when all of them are through */
        html = g_strdup_printf (
            "<!doctype html><title>headers</title><body><img id=i src='/headers/img.gif'>"
            "<script src='/headers/app.js'></script>"
            "<iframe id=f src='%s/headers/frame'></iframe><script>"
            "var img=new Promise(function(r){var i=document.getElementById('i');if(i.complete)r();else{i.onload=r;i.onerror=r;}});"
            "var frame=new Promise(function(r){addEventListener('message',function(e){if(e.data==='frame-done')r();});});"
            "Promise.all([img,frame,fetch('/headers/api').then(function(x){return x.text();}),"
            "fetch('%s/headers/xorigin',{mode:'no-cors'}).catch(function(){})])"
            ".then(function(){document.title='done:headers';});</script></body>",
            st.other, st.other);
    } else if (g_strcmp0 (path, "/headers/frame") == 0) {
        html = g_strdup_printf ("<!doctype html><script>Promise.all(["
                                "fetch('%s/headers/from-frame',{mode:'no-cors'}).catch(function(){}),"
                                "fetch('%s/headers/from-frame-cors').catch(function(){})])"
                                ".then(function(){parent.postMessage('frame-done','*');});</script>", st.site, st.site);
    } else if (g_strcmp0 (path, "/headers/app.js") == 0) {
        respond (msg, "text/javascript", "window.appLoaded=1;", strlen ("window.appLoaded=1;"));
    } else if (g_str_has_suffix (path, ".gif")) {
        respond (msg, "image/gif", gif, sizeof gif - 1);
    } else if (g_strcmp0 (path, "/headers/api") == 0 || g_strcmp0 (path, "/headers/xorigin") == 0
               || g_strcmp0 (path, "/headers/from-frame") == 0 || g_strcmp0 (path, "/headers/own-api") == 0
               || g_strcmp0 (path, "/headers/own-copy") == 0) {
        respond (msg, "text/plain", "ok", 2);
    } else if (g_strcmp0 (path, "/headers/from-frame-cors") == 0) {
        soup_message_headers_replace (soup_server_message_get_response_headers (msg), "Access-Control-Allow-Origin", "*");
        respond (msg, "text/plain", "ok", 2);
    } else if (g_strcmp0 (path, "/headers/redirect") == 0 && query != NULL && g_hash_table_lookup (query, "to") != NULL) {
        soup_server_message_set_redirect (msg, SOUP_STATUS_FOUND, g_hash_table_lookup (query, "to"));
    } else if (g_strcmp0 (path, "/headers/subchain") == 0) {
        /* an image of the origin's page whose request is redirected to another site and on to a third */
        char *end = g_strconcat (st.third != NULL ? st.third : st.other, "/headers/sub-end.gif", NULL);
        char *end_escaped = g_uri_escape_string (end, NULL, FALSE);
        char *hop = g_strdup_printf ("%s/headers/redirect?to=%s", st.other, end_escaped);
        char *hop_escaped = g_uri_escape_string (hop, NULL, FALSE);
        html = g_strdup_printf ("<!doctype html><title>subchain</title><img id=i src='/headers/redirect?to=%s'><script>"
                                "var i=document.getElementById('i');function d(){document.title='done:subchain';}"
                                "if(i.complete)d();else{i.onload=d;i.onerror=d;}</script>", hop_escaped);
        g_free (end);
        g_free (end_escaped);
        g_free (hop);
        g_free (hop_escaped);
    } else if (g_strcmp0 (path, "/headers/own") == 0) {
        /* a page of another site that sends a header of a configured name itself: once with its own value, once with
         * exactly the configured one */
        html = g_strdup ("<!doctype html><title>own</title><script>Promise.all(["
                         "fetch('/headers/own-api',{headers:{'X-Uc-Token':'own-value'}}).catch(function(){}),"
                         "fetch('/headers/own-copy',{headers:{'X-Uc-Token':'t1'}}).catch(function(){})])"
                         ".then(function(){document.title='done:own';});</script>");
    } else if (g_strcmp0 (path, "/headers/selfimg") == 0 || g_strcmp0 (path, "/headers/selfframe") == 0) {
        /* another site's page embeds ITS OWN URL as an image or a frame, and its server answers that second request
         * (not the document: Accept without text/html, or the frame) with a redirect into the origin. Up to 0.8.2 the
         * hop counted as the page's main resource (its redirecting response had the page URI) and got the headers. */
        const char *accept = soup_message_headers_get_one (req, "Accept");
        gboolean document = accept != NULL && strstr (accept, "text/html") != NULL;
        gboolean frame = g_strcmp0 (path, "/headers/selfframe") == 0;
        /* the tool's own load has no Referer; the page's image and frame have the page as Referer */
        gboolean top = document && soup_message_headers_get_one (req, "Referer") == NULL;
        if (top && !frame) {
            html = g_strdup_printf ("<!doctype html><title>selfimg</title><img id=i src='%s/headers/selfimg'><script>"
                                    "var i=document.getElementById('i');function d(){document.title='done:selfimg';}"
                                    "if(i.complete)d();else{i.onload=d;i.onerror=d;}</script>", st.other);
        } else if (top && frame) {
            html = g_strdup_printf ("<!doctype html><title>selfframe</title><iframe id=f src='%s/headers/selfframe'>"
                                    "</iframe><script>var f=document.getElementById('f');"
                                    "function d(){document.title='done:selfframe';}f.onload=d;f.onerror=d;"
                                    "setTimeout(d,3000);</script>", st.other);
        } else {
            char *to = g_strdup_printf ("%s/headers/%s-landed", st.site, frame ? "selfframe" : "selfimg");
            soup_server_message_set_redirect (msg, SOUP_STATUS_FOUND, to);
            g_free (to);
        }
    } else if (g_strcmp0 (path, "/headers/embed") == 0) {
        html = g_strdup_printf ("<!doctype html><title>embed</title><img id=i src='%s/headers/embedded.gif'><script>"
                                "var i=document.getElementById('i');function d(){document.title='done:embed';}"
                                "if(i.complete)d();else{i.onload=d;i.onerror=d;}</script>", st.site);
    } else if (g_strcmp0 (path, "/headers/form-cross") == 0 || g_strcmp0 (path, "/headers/form-same") == 0) {
        gboolean cross = g_strcmp0 (path, "/headers/form-cross") == 0;
        html = g_strdup_printf ("<!doctype html><title>form</title><form method=post action='%s/headers/%s'>"
                                "<input name=a value=1></form><script>document.forms[0].submit();</script>",
                                st.site, cross ? "post-cross" : "post-same");
    } else if (g_str_has_prefix (path, "/headers/")) {
        html = done_page (path + strlen ("/headers/"));
    } else {
        soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
    }
    if (html != NULL) {
        respond_html (msg, html);
        g_free (html);
    }
}

static gboolean
servers_start (void)
{
    GError *error = NULL;
    GSList *uris;

    st.server = soup_server_new ("server-header", "ucwebview-test", NULL);
    soup_server_add_handler (st.server, NULL, serve, NULL, NULL);
    /* both loopback families: localhost may resolve to ::1 first */
    if (!soup_server_listen_local (st.server, 0, 0, &error)) {
        g_clear_error (&error);
        if (!soup_server_listen_local (st.server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
            g_printerr ("site-test: cannot listen on 127.0.0.1: %s\n", error->message);
            g_clear_error (&error);
            return FALSE;
        }
    }
    uris = soup_server_get_uris (st.server);
    st.port = (guint16) g_uri_get_port (uris->data);
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    st.site = g_strdup_printf ("http://127.0.0.1:%u", st.port);
    st.other = g_strdup_printf ("http://localhost:%u", st.port);
    st.site_host = g_strdup_printf ("127.0.0.1:%u", st.port);
    st.other_host = g_strdup_printf ("localhost:%u", st.port);
    {
        /* a third site for redirect chains: Linux answers on all of 127.0.0.0/8 */
        GInetAddress *ip = g_inet_address_new_from_string ("127.0.0.2");
        GSocketAddress *address = g_inet_socket_address_new (ip, 0);
        if (soup_server_listen (st.server, address, 0, &error)) {
            uris = soup_server_get_uris (st.server);
            for (GSList *l = uris; l != NULL; l = l->next)
                if (g_strcmp0 (g_uri_get_host (l->data), "127.0.0.2") == 0) {
                    st.third = g_strdup_printf ("http://127.0.0.2:%d", g_uri_get_port (l->data));
                    st.third_host = g_strdup_printf ("127.0.0.2:%d", g_uri_get_port (l->data));
                }
            g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
        } else {
            g_printerr ("site-test: no third site on 127.0.0.2: %s\n", error->message);
            g_clear_error (&error);
        }
        g_object_unref (address);
        g_object_unref (ip);
    }

    if (opt_site_tls_cert != NULL) {
        GTlsCertificate *cert = g_tls_certificate_new_from_file (opt_site_tls_cert, &error);
        GInetAddress *other_ip;
        GSocketAddress *address;
        GByteArray *der = NULL;

        if (cert == NULL) {
            g_printerr ("site-test: cannot read --site-tls-cert: %s\n", error->message);
            g_clear_error (&error);
            return FALSE;
        }
        g_object_get (cert, "certificate", &der, NULL);
        st.tls_cert_sha256 = g_compute_checksum_for_data (G_CHECKSUM_SHA256, der->data, der->len);
        g_byte_array_unref (der);
        st.tls_server = soup_server_new ("server-header", "ucwebview-test", "tls-certificate", cert, NULL);
        g_object_unref (cert);
        soup_server_add_handler (st.tls_server, NULL, serve, NULL, NULL);
        if (!soup_server_listen_local (st.tls_server, 0, SOUP_SERVER_LISTEN_HTTPS, &error)) {
            g_clear_error (&error);
            if (!soup_server_listen_local (st.tls_server, 0, SOUP_SERVER_LISTEN_HTTPS | SOUP_SERVER_LISTEN_IPV4_ONLY,
                                           &error)) {
                g_printerr ("site-test: cannot listen for HTTPS: %s\n", error->message);
                g_clear_error (&error);
                return FALSE;
            }
        }
        /* a third host for the policy: Linux answers on all of 127.0.0.0/8 */
        other_ip = g_inet_address_new_from_string ("127.0.0.2");
        address = g_inet_socket_address_new (other_ip, 0);
        if (!soup_server_listen (st.tls_server, address, SOUP_SERVER_LISTEN_HTTPS, &error)) {
            g_printerr ("site-test: cannot listen on 127.0.0.2: %s\n", error->message);
            g_clear_error (&error);
        }
        g_object_unref (address);
        g_object_unref (other_ip);
        uris = soup_server_get_uris (st.tls_server);
        for (GSList *l = uris; l != NULL; l = l->next) {
            if (g_strcmp0 (g_uri_get_host (l->data), "127.0.0.2") == 0)
                st.tls_port_other = (guint16) g_uri_get_port (l->data);
            else if (st.tls_port == 0)
                st.tls_port = (guint16) g_uri_get_port (l->data);
        }
        g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    }
    return TRUE;
}

/* ---- steps ------------------------------------------------------------------------------------------------- */

static const char custom_ua[] = "ucwebview-test/0.8 (site-test)";

static void
step_ua (void)
{
    char *url = g_strconcat (st.site, "/ua", NULL);
    char *default_ua = NULL;
    const Record *r;

    if (load (url, "done:") && (r = find (st.site_host, "/ua")) != NULL) {
        /* library 0.8.2: WebKit's user agent with the Remote's token, "UCRemote/<UC_OS_VERSION>" (the firmware's
         * /etc/uc-release, or the environment the remote-ui units load from it), "UCRemote" without a version */
        const char *source = NULL;
        char *version = uc_site_firmware_version (UC_SITE_RELEASE_FILE, g_getenv ("UC_OS_VERSION"), &source);
        char *token = version != NULL ? g_strconcat (" " UC_SITE_UA_PRODUCT "/", version, NULL)
                                      : g_strdup (" " UC_SITE_UA_PRODUCT);
        gboolean ok;

        default_ua = g_strdup (r->user_agent);
        g_print ("site-test: the default user agent is \"%s\"\n", default_ua != NULL ? default_ua : "");
        ok = default_ua != NULL && strstr (default_ua, "AppleWebKit/") != NULL && strstr (default_ua, "iPhone") == NULL
             && g_strcmp0 (st.title + 5, default_ua) == 0;
        check (ok, "ua-default", "the server and navigator.userAgent got WebKit's user agent");
        ok = default_ua != NULL && g_str_has_suffix (default_ua, token);
        check (ok, "ua-default-remote", "it ends with the Remote's token \"%s\" (%s)", token + 1, source);
        g_free (version);
        g_free (token);
    } else {
        check (FALSE, "ua-default", "the page did not load");
    }
    {
        static const struct { const char *name; int preset; const char *custom, *expect; } presets[] = {
            { "ua-mobile", UC_WEBVIEW_USER_AGENT_MOBILE, NULL, "iPhone" },
            { "ua-desktop", UC_WEBVIEW_USER_AGENT_DESKTOP, "ignored", "Macintosh" },
            { "ua-custom", UC_WEBVIEW_USER_AGENT_CUSTOM, custom_ua, custom_ua },
        };
        for (gsize i = 0; i < G_N_ELEMENTS (presets); i++) {
            int rc = uc_webview_set_user_agent (st.wv, presets[i].preset, presets[i].custom);
            gboolean loaded = rc == 0 && load (url, "done:"), ok;
            r = loaded ? find (st.site_host, "/ua") : NULL;
            ok = r != NULL && r->user_agent != NULL && strstr (r->user_agent, presets[i].expect) != NULL
                 && strstr (st.title, presets[i].expect) != NULL && strstr (r->user_agent, UC_SITE_UA_PRODUCT) == NULL;
            check (ok, presets[i].name, "set_user_agent() %d, the server and the page got a user agent with \"%s\" and "
                   "without the Remote's token", rc, presets[i].expect);
        }
    }
    {
        int rc = uc_webview_set_user_agent (st.wv, UC_WEBVIEW_USER_AGENT_DEFAULT, NULL);
        gboolean ok;
        r = rc == 0 && load (url, "done:") ? find (st.site_host, "/ua") : NULL;
        ok = r != NULL && default_ua != NULL && g_strcmp0 (r->user_agent, default_ua) == 0;
        check (ok, "ua-back-to-default", "set_user_agent(DEFAULT) %d, WebKit's user agent again", rc);
    }
    {
        gboolean ok = uc_webview_set_user_agent (st.wv, 4, NULL) == -1
                      && uc_webview_set_user_agent (st.wv, UC_WEBVIEW_USER_AGENT_CUSTOM, NULL) == -1
                      && uc_webview_set_user_agent (st.wv, UC_WEBVIEW_USER_AGENT_CUSTOM, "a\r\nX-Evil: 1") == -1
                      && uc_webview_set_user_agent (NULL, UC_WEBVIEW_USER_AGENT_MOBILE, NULL) == -1;
        check (ok, "ua-refused", "an unknown preset, CUSTOM without or with an invalid text and no view return -1");
    }
    g_free (default_ua);
    g_free (url);
}

/* sets cookies and waits for the done callback; returns the call's result, *status the callback's */
static int
set_cookies (const char *url, const uc_webview_cookie *cookies, size_t count, int *status)
{
    guint before = st.done_calls;
    int rc = uc_webview_global_set_cookies (url, cookies, count, on_done, NULL);

    *status = -1;
    if (rc == 0 && spin_until (done_called, GUINT_TO_POINTER (before + 1), 5000))
        *status = st.done_status;
    return rc;
}

/* the cookie file of the data directory contains name (SQLite keeps the text as it is; a WAL file too) */
static gboolean
cookie_file_has (const char *name)
{
    static const char *const suffixes[] = { "", "-wal", "-journal" };
    gboolean found = FALSE;

    for (gsize i = 0; i < G_N_ELEMENTS (suffixes) && !found; i++) {
        char *path = g_strdup_printf ("%s/cookies.sqlite%s", st.data_dir, suffixes[i]);
        char *data = NULL;
        gsize length = 0;
        if (g_file_get_contents (path, &data, &length, NULL))
            found = memmem (data, length, name, strlen (name)) != NULL;
        g_free (data);
        g_free (path);
    }
    return found;
}

static void
step_cookies (void)
{
    char *base = g_strconcat (st.site, "/", NULL);
    char *page = g_strconcat (st.site, "/cookies", NULL);
    char *sub = g_strconcat (st.site, "/cookies/sub/x", NULL);
    gint64 tomorrow = g_get_real_time () / G_USEC_PER_SEC + 24 * 3600;
    const Record *r;
    int status, rc;

    if (st.data_dir != NULL) {
        gboolean there = load (page, "done:") && (r = find (st.site_host, "/cookies")) != NULL
                         && has_cookie (r->cookie, "uc_keep");
        g_print ("site-test: the persistent cookie of an earlier run with this data directory is %s\n",
                 there ? "there" : "not there");
        if (opt_site_expect_persisted)
            check (there, "cookies-persisted", "the cookie uc_keep set by the previous run came back from %s/cookies.sqlite",
                   st.data_dir);
    } else if (opt_site_expect_persisted) {
        check (FALSE, "cookies-persisted", "--site-expect-persisted needs --data-dir");
    }
    {
        const uc_webview_cookie cookies[] = {
            { "uc_session", "s1", NULL, NULL, 0, 1, -1 },          /* host-only, httpOnly, session */
            { "uc_keep", "k1", NULL, NULL, 0, 0, tomorrow },       /* persistent */
            { "uc_path", "p1", NULL, "/cookies/sub", 0, 0, -1 },   /* only below /cookies/sub */
            { "uc_secure", "x1", NULL, NULL, 1, 0, -1 },           /* https only: not on this http page */
        };
        rc = set_cookies (base, cookies, G_N_ELEMENTS (cookies), &status);
        check (rc == 0 && status == UC_WEBVIEW_DONE_OK, "cookies-set", "set_cookies() %d, done callback %d", rc, status);
    }
    if (load (page, "done:") && (r = find (st.site_host, "/cookies")) != NULL) {
        check (has_cookie (r->cookie, "uc_session") && has_cookie (r->cookie, "uc_keep") && !has_cookie (r->cookie, "uc_path")
               && !has_cookie (r->cookie, "uc_secure"),
               "cookies-sent", "the server got uc_session and uc_keep, not uc_path (another path) or uc_secure (http)");
        check (has_cookie (st.title + 5, "uc_keep") && !has_cookie (st.title + 5, "uc_session"), "cookies-http-only",
               "document.cookie has uc_keep but not the httpOnly uc_session");
    } else {
        check (FALSE, "cookies-sent", "the page did not load");
    }
    check (load (sub, "done:") && (r = find (st.site_host, "/cookies/sub/x")) != NULL && has_cookie (r->cookie, "uc_path"),
           "cookies-path", "a page below /cookies/sub got uc_path");
    {
        /* a whole call is refused for one bad cookie: none of it is set */
        const uc_webview_cookie bad[] = {
            { "uc_never", "n1", NULL, NULL, 0, 0, -1 },
            { "uc_bad", "b1", "example.com", NULL, 0, 0, -1 },     /* not the URL's domain */
        };
        const uc_webview_cookie suffix[] = { { "uc_suffix", "1", "com", NULL, 0, 0, -1 } };
        int rc_bad = uc_webview_global_set_cookies (base, bad, G_N_ELEMENTS (bad), on_done, NULL);
        int rc_value = uc_webview_global_set_cookies (base, (const uc_webview_cookie[]) { { "a", "1;b", NULL, NULL, 0, 0, -1 } },
                                                      1, on_done, NULL);
        int rc_url = uc_webview_global_set_cookies ("ftp://127.0.0.1/", bad, 1, on_done, NULL);
        int rc_suffix = uc_webview_global_set_cookies ("http://example.com/", suffix, 1, on_done, NULL);
        gboolean loaded = load (page, "done:");
        r = loaded ? find (st.site_host, "/cookies") : NULL;
        check (rc_bad == -1 && rc_value == -1 && rc_url == -1 && rc_suffix == -1 && r != NULL
               && !has_cookie (r->cookie, "uc_never"),
               "cookies-refused", "a foreign domain, a value with ';', an ftp URL and a public suffix return -1, nothing "
               "of the refused call is set");
    }
    {
        const uc_webview_cookie del[] = { { "uc_session", "", NULL, NULL, 0, 1, 0 } };   /* expired: deleted */
        rc = set_cookies (base, del, 1, &status);
        check (rc == 0 && status == UC_WEBVIEW_DONE_OK && load (page, "done:")
               && (r = find (st.site_host, "/cookies")) != NULL && !has_cookie (r->cookie, "uc_session")
               && has_cookie (r->cookie, "uc_keep"),
               "cookies-delete", "a cookie with an expiry time in the past deletes uc_session, uc_keep stays");
    }
    rc = set_cookies (base, NULL, 0, &status);
    check (rc == 0 && status == UC_WEBVIEW_DONE_OK, "cookies-none", "count 0: %d, done callback %d", rc, status);
    if (st.data_dir != NULL)
        check (cookie_file_has ("uc_keep") && !cookie_file_has ("uc_session"), "cookies-written",
               "%s/cookies.sqlite has the persistent uc_keep, not the session cookie", st.data_dir);
    else
        skip ("cookies-written", "no --data-dir: an ephemeral session writes nothing");
    g_free (base);
    g_free (page);
    g_free (sub);
}

static gboolean
headers_on (const Record *r)
{
    return r != NULL && g_strcmp0 (r->authorization, "Bearer site-test-token") == 0 && g_strcmp0 (r->token, "t1") == 0;
}

static gboolean
headers_off (const Record *r)
{
    return r != NULL && r->authorization == NULL && r->token == NULL;
}

static const char *
describe (const Record *r)
{
    if (r == NULL)
        return "no request";
    if (headers_on (r))
        return "both headers";
    if (headers_off (r))
        return "no header";
    return "a wrong or partial set of headers";
}

/* loads uri until its "done:<what>" title and reports what the server got for host and path */
static gboolean
headers_check (const char *name, const char *uri, const char *what, const char *host, const char *path, gboolean expect,
               const char *description)
{
    char *title = g_strconcat ("done:", what, NULL);
    gboolean loaded = load (uri, title);
    const Record *r = find (host, path);
    gboolean ok = loaded && (expect ? headers_on (r) : headers_off (r));

    check (ok, name, "%s: %s, expected %s%s", description, describe (r), expect ? "both headers" : "no header",
           loaded ? "" : " (the page did not report done)");
    g_free (title);
    return ok;
}

static void
step_headers (void)
{
    static const char *const names[] = { "Authorization", "X-Uc-Token" };
    static const char *const values[] = { "Bearer site-test-token", "t1" };
    char *url;
    int rc;

    rc = uc_webview_set_origin_headers (st.wv, st.site, names, values, 2);
    if (rc != 0) {
        check (FALSE, "headers-set", "set_origin_headers() returned %d (is the web process extension installed?)", rc);
        return;
    }
    url = g_strconcat (st.site, "/headers", NULL);
    if (headers_check ("headers-page", url, "headers", st.site_host, "/headers", TRUE, "the origin's page")) {
        static const char *const on[] = { "/headers/app.js", "/headers/img.gif", "/headers/api" };
        const Record *r;
        for (gsize i = 0; i < G_N_ELEMENTS (on); i++) {
            char *name = g_strdup_printf ("headers-%s", on[i] + strlen ("/headers/"));
            r = find (st.site_host, on[i]);
            check (headers_on (r), name, "%s of the origin's page: %s", on[i], describe (r));
            g_free (name);
        }
        r = find (st.other_host, "/headers/xorigin");
        check (headers_off (r), "headers-cross-origin-fetch", "a fetch of the page to another origin: %s", describe (r));
        /* a CORS fetch carries Origin: the frame's site, not the origin's */
        r = find (st.site_host, "/headers/from-frame-cors");
        check (headers_off (r), "headers-frame-cors", "a CORS fetch of a frame of another site to the origin: %s (Origin %s)",
               describe (r), r != NULL && r->origin != NULL ? "set" : "not set");
        /* documented limit: a no-cors GET of such a frame has no Origin and the top-level document is the origin's */
        r = find (st.site_host, "/headers/from-frame");
        check (headers_on (r), "headers-frame-no-cors", "a no-cors GET of a frame of another site, inside the origin's "
               "page, to the origin: %s (documented: without an Origin header it counts as the page's own)", describe (r));
    }
    g_free (url);

    /* a redirect away from the origin: what WebKit carries over is removed */
    url = g_strdup_printf ("%s/headers/redirect?to=%s/headers/landed", st.site, st.other);
    if (headers_check ("headers-redirect-away", url, "landed", st.other_host, "/headers/landed", FALSE,
                       "the other site after the origin's redirect")) {
        const Record *r = find (st.site_host, "/headers/redirect");
        check (headers_on (r), "headers-redirect-source", "the origin's redirecting response: %s", describe (r));
    }
    g_free (url);

    /* A main-frame redirect of another site into the origin: WPE moves the load to another web process, which starts
     * it with the target as page URI, so the origin's page gets the headers. In one process (the WebKitGTK harness
     * with UC_HARNESS_PSON=0) the hop is judged by the other site's page URI: no headers since 0.8.3, the documented
     * same-process limit. */
    url = g_strdup_printf ("%s/headers/redirect?to=%s/headers/back", st.other, st.site);
    headers_check ("headers-redirect-into", url, "back", st.site_host, "/headers/back", process_swaps (),
                   process_swaps () ? "a main frame redirect of another site to the origin (a process swap)"
                                    : "a main frame redirect of another site to the origin in one process "
                                      "(UC_HARNESS_PSON=0, documented)");
    g_free (url);

    /* an image and a frame of another site's page with the page's own URL, redirected into the origin */
    url = g_strconcat (st.other, "/headers/selfimg", NULL);
    headers_check ("headers-selfimg-redirect-into", url, "selfimg", st.site_host, "/headers/selfimg-landed", FALSE,
                   "an image of another site's page with the page's own URL, redirected into the origin");
    g_free (url);
    url = g_strconcat (st.other, "/headers/selfframe", NULL);
    headers_check ("headers-selfframe-redirect-into", url, "selfframe", st.site_host, "/headers/selfframe-landed",
                   FALSE, "a frame of another site's page with the page's own URL, redirected into the origin");
    g_free (url);

    /* the view's first navigation stays in its first web process (WebProcessPool.cpp:2330-2332): a redirect of
     * another site into the origin there starts the origin's page without the headers (documented) */
    {
        uc_webview *main_view = st.wv;
        uc_webview_callbacks cb = {
            .frame_ready = on_frame, .load_changed = on_load, .title_changed = on_title,
            .web_process_terminated = on_terminated,
        };
        void *held = st.held;

        st.held = NULL;
        st.wv = uc_webview_new_sized (st.width, st.height, &cb, sizeof cb, NULL);
        if (st.wv != NULL) {
            url = g_strdup_printf ("%s/headers/redirect?to=%s/headers/first-back", st.other, st.site);
            headers_check ("headers-first-navigation-redirect-into", url, "first-back", st.site_host,
                           "/headers/first-back", FALSE, "a redirect of another site into the origin as the first "
                           "navigation of a new view (documented: no headers)");
            g_free (url);
            settle ();
            if (st.held != NULL)
                uc_webview_frame_release (st.wv, st.held);
            uc_webview_free (st.wv);
        } else {
            check (FALSE, "headers-first-navigation-redirect-into", "no second view");
        }
        st.wv = main_view;
        st.held = held;
    }

    /* chains of cross-site redirects (library 0.8.1): every hop leaves the web process (a process swap on WPE) */
    if (st.third != NULL) {
        char *end = g_strconcat (st.third, "/headers/chain-end", NULL);
        char *end_escaped = g_uri_escape_string (end, NULL, FALSE);
        char *hop = g_strdup_printf ("%s/headers/redirect?to=%s", st.other, end_escaped);
        char *hop_escaped = g_uri_escape_string (hop, NULL, FALSE);

        url = g_strdup_printf ("%s/headers/redirect?to=%s", st.site, hop_escaped);
        if (headers_check ("headers-chain-end", url, "chain-end", st.third_host, "/headers/chain-end", FALSE,
                           "the third site at the end of origin -> other site -> third site")) {
            const Record *r = find (st.other_host, "/headers/redirect");
            check (headers_off (r), "headers-chain-middle", "the other site in the middle of the chain: %s",
                   describe (r));
        }
        g_free (url);
        g_free (end);
        g_free (end_escaped);
        g_free (hop);
        g_free (hop_escaped);

        /* other site -> origin -> other site: the origin's hop gets them, the way out not. The chain starts from a page
         * of the other site, so that the other site's web process has a committed document: WebKit swaps the process at
         * a redirect only then (WebProcessPool.cpp:2330-2332); from a fresh process the origin's hop stays in it and
         * gets no headers (documented, like headers-first-navigation-redirect-into) */
        {
            char *warm = g_strconcat (st.other, "/headers/warm", NULL);
            load (warm, "done:warm");
            g_free (warm);
        }
        end = g_strconcat (st.other, "/headers/chain-out", NULL);
        end_escaped = g_uri_escape_string (end, NULL, FALSE);
        hop = g_strdup_printf ("%s/headers/redirect?to=%s", st.site, end_escaped);
        hop_escaped = g_uri_escape_string (hop, NULL, FALSE);
        url = g_strdup_printf ("%s/headers/redirect?to=%s", st.other, hop_escaped);
        if (headers_check ("headers-chain-out", url, "chain-out", st.other_host, "/headers/chain-out", FALSE,
                           "the other site after other site -> origin -> other site")) {
            const Record *r = find (st.site_host, "/headers/redirect");
            check (process_swaps () ? headers_on (r) : headers_off (r), "headers-chain-through",
                   "the origin's hop in the middle of the chain: %s (%s)", describe (r),
                   process_swaps () ? "a process swap, expected both headers"
                                    : "one process, UC_HARNESS_PSON=0, expected no header");
        }
        g_free (url);
        g_free (end);
        g_free (end_escaped);
        g_free (hop);
        g_free (hop_escaped);

        /* a subresource of the origin's page redirected to another site and on to a third one */
        url = g_strconcat (st.site, "/headers/subchain", NULL);
        if (headers_check ("headers-subresource-chain-end", url, "subchain", st.third_host, "/headers/sub-end.gif",
                           FALSE, "the third site at the end of a subresource's redirect chain")) {
            const Record *r = find (st.other_host, "/headers/redirect");
            check (headers_off (r), "headers-subresource-chain-middle", "the other site in the middle: %s",
                   describe (r));
            r = find (st.site_host, "/headers/redirect");
            check (headers_on (r), "headers-subresource-chain-start", "the origin's own redirecting request: %s",
                   describe (r));
        }
        g_free (url);
    } else {
        skip ("headers-chains", "no third site on 127.0.0.2");
    }

    /* another site's own header of a configured name: its own value stays; exactly the configured value is taken for
     * a leaked copy and removed (documented) */
    url = g_strconcat (st.other, "/headers/own", NULL);
    if (load (url, "done:own")) {
        const Record *own = find (st.other_host, "/headers/own-api"), *copy = find (st.other_host, "/headers/own-copy");
        check (own != NULL && g_strcmp0 (own->token, "own-value") == 0, "headers-own-value",
               "another site's page sends X-Uc-Token with its own value: %s",
               own == NULL ? "no request" : own->token != NULL ? "kept" : "removed");
        check (copy != NULL && copy->token == NULL, "headers-own-configured-value",
               "another site's page sends X-Uc-Token with exactly the configured value: %s (documented: removed)",
               copy == NULL ? "no request" : copy->token != NULL ? "kept" : "removed");
    } else {
        check (FALSE, "headers-own-value", "the page did not report done");
    }
    g_free (url);

    url = g_strconcat (st.other, "/headers/embed", NULL);
    headers_check ("headers-embedded", url, "embed", st.site_host, "/headers/embedded.gif", FALSE,
                   "an image of the origin in a page of another site");
    g_free (url);

    url = g_strconcat (st.other, "/headers/form-cross", NULL);
    headers_check ("headers-form-other-site", url, "post-cross", st.site_host, "/headers/post-cross", FALSE,
                   "a form another site posts to the origin");
    g_free (url);
    url = g_strconcat (st.site, "/headers/form-same", NULL);
    headers_check ("headers-form-same-origin", url, "post-same", st.site_host, "/headers/post-same", TRUE,
                   "a form the origin posts to itself");
    g_free (url);

    /* refused calls change nothing; count 0 removes */
    {
        static const char *const host[] = { "Host" };
        static const char *const crlf[] = { "a\r\nX-Evil: 1" };
        gboolean ok = uc_webview_set_origin_headers (st.wv, st.site, host, values, 1) == -1
                      && uc_webview_set_origin_headers (st.wv, st.site, names, crlf, 1) == -1
                      && uc_webview_set_origin_headers (st.wv, "ftp://127.0.0.1", names, values, 1) == -1
                      && uc_webview_set_origin_headers (NULL, st.site, names, values, 1) == -1;
        check (ok, "headers-refused", "Host, a value with CR LF, an ftp origin and no view return -1");
    }
    rc = uc_webview_set_origin_headers (st.wv, st.site, NULL, NULL, 0);
    url = g_strconcat (st.site, "/headers/after-remove", NULL);
    if (rc != 0)
        check (FALSE, "headers-removed", "set_origin_headers(count 0) returned %d", rc);
    else
        headers_check ("headers-removed", url, "after-remove", st.site_host, "/headers/after-remove", FALSE,
                       "after count 0");
    g_free (url);
}

/* expect_ok: the page's "done:tls"; else the load failed (FAILED, never the page) */
static gboolean
load_tls (const char *url, gboolean expect_ok)
{
    /* An http page of the site between the attempts. Up to library 0.10.2 an attempt failed now and then with
     * "Operation was cancelled" before any certificate check (2 of 8 full runs on WebKitGTK 2.54.0, 3 of 100 on WPE
     * WebKit 2.54.3 in containers): its request took over the connection of WebKit's preconnect after that had failed
     * on the certificate; the library loads such a load again since 0.10.3 (uc_load_error.h; --load-fail-test's
     * tls-race step provokes it) */
    char *gap = g_strconcat (st.site, "/headers/tls-gap", NULL);
    load (gap, "done:tls-gap");
    g_free (gap);
    settle ();
    g_ptr_array_set_size (st.events, 0);
    g_clear_pointer (&st.title, g_free);
    uc_webview_load_uri (st.wv, url);
    if (expect_ok)
        return spin_until (title_is, "done:tls", 10000);
    if (!spin_until (finished_n, GUINT_TO_POINTER (1), 10000))
        return FALSE;
    settle ();      /* a late success would show here */
    return count_events ("failed") > 0 && g_strcmp0 (st.title, "done:tls") != 0;
}

static void
step_tls (void)
{
    char *site, *localhost, *other;
    char wrong[65];
    gboolean ok;

    if (st.tls_server == NULL) {
        skip ("tls", "no --site-tls-cert");
        return;
    }
    site = g_strdup_printf ("https://127.0.0.1:%u/tls", st.tls_port);
    localhost = g_strdup_printf ("https://localhost:%u/tls", st.tls_port);
    other = g_strdup_printf ("https://127.0.0.2:%u/tls", st.tls_port_other);
    g_print ("site-test: the test certificate's SHA-256 fingerprint is %s\n", st.tls_cert_sha256);

    ok = load_tls (site, FALSE);
    check (ok, "tls-no-exception", "a self-signed certificate without an exception: FAILED, the page never showed");
    g_strlcpy (wrong, st.tls_cert_sha256, sizeof wrong);
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    ok = uc_webview_global_allow_certificate ("127.0.0.1", wrong) == 0 && load_tls (site, FALSE);
    check (ok, "tls-wrong-fingerprint", "an exception for another fingerprint: still FAILED");
    ok = uc_webview_global_allow_certificate ("127.0.0.1", st.tls_cert_sha256) == 0 && load_tls (site, TRUE);
    {
        guint failed = count_events ("failed"), committed = count_events ("committed");
        guint started = count_events ("started"), finished = count_events ("finished");
        check (ok && failed == 0 && committed == 1, "tls-fingerprint",
               "the exception for its fingerprint: the page loads, %u FAILED, %u committed (%u started, %u finished)",
               failed, committed, started, finished);
    }
    ok = uc_webview_global_allow_certificate ("[::1]", NULL) == 0
         && uc_webview_global_allow_certificate ("localhost", NULL) == 0 && load_tls (localhost, TRUE)
         && count_events ("failed") == 0;
    check (ok, "tls-any", "an exception for any certificate of localhost: the page loads");
    if (st.tls_port_other == 0) {
        skip ("tls-policy", "no listener on 127.0.0.2");
    } else {
        gboolean failed = load_tls (other, FALSE);
        int rc = uc_webview_global_set_tls_errors_policy (1);
        gboolean loaded = rc == 0 && load_tls (other, TRUE);
        check (failed && loaded, "tls-policy", "127.0.0.2 without an exception fails (%s), with the policy \"ignore\" "
               "it loads (%s)", failed ? "yes" : "no", loaded ? "yes" : "no");
        uc_webview_global_set_tls_errors_policy (0);
    }
    ok = uc_webview_global_allow_certificate ("127.0.0.1:443", NULL) == -1
         && uc_webview_global_allow_certificate ("https://example.com", NULL) == -1
         && uc_webview_global_allow_certificate ("example.com", "abc") == -1
         && uc_webview_global_allow_certificate (NULL, NULL) == -1;
    check (ok, "tls-refused", "a port, a URL, a short fingerprint and no host return -1");
    g_free (site);
    g_free (localhost);
    g_free (other);
}

static const char *
scheme_seen (void)
{
    return st.title != NULL && g_str_has_prefix (st.title, "done:") ? st.title + 5 : "no page";
}

static void
step_scheme (void)
{
    char *url = g_strconcat (st.site, "/scheme", NULL);
    gboolean ok;
    int rc;

    rc = uc_webview_global_set_color_scheme (UC_WEBVIEW_COLOR_SCHEME_LIGHT);
    ok = rc == 0 && load (url, "done:") && g_strcmp0 (st.title, "done:light") == 0;
    check (ok, "scheme-light", "prefers-color-scheme: %s", scheme_seen ());
    rc = uc_webview_global_set_color_scheme (UC_WEBVIEW_COLOR_SCHEME_DARK);
    ok = rc == 0 && spin_until (title_is, "done:dark", 5000);
    check (ok, "scheme-dark-live", "the shown page follows at once: %s", scheme_seen ());
    ok = load (url, "done:") && g_strcmp0 (st.title, "done:dark") == 0;
    check (ok, "scheme-dark-load", "a new page starts dark: %s", scheme_seen ());
    rc = uc_webview_global_set_color_scheme (UC_WEBVIEW_COLOR_SCHEME_LIGHT);
    ok = rc == 0 && spin_until (title_is, "done:light", 5000);
    check (ok, "scheme-light-again", "back to light: %s", scheme_seen ());
    ok = uc_webview_global_set_color_scheme (2) == -1 && uc_webview_global_set_color_scheme (-1) == -1;
    check (ok, "scheme-refused", "unknown schemes return -1");
    g_free (url);
}

static int
clear (unsigned types, int *status)
{
    guint before = st.done_calls;
    int rc = uc_webview_global_clear_data (types, on_done, NULL);

    *status = -1;
    if (rc == 0 && spin_until (done_called, GUINT_TO_POINTER (before + 1), 15000))
        *status = st.done_status;
    return rc;
}

static void
step_clear (void)
{
    char *base = g_strconcat (st.site, "/", NULL);
    char *page = g_strconcat (st.site, "/cookies", NULL);
    gint64 tomorrow = g_get_real_time () / G_USEC_PER_SEC + 24 * 3600;
    const uc_webview_cookie cookies[] = { { "uc_clear", "c1", NULL, NULL, 0, 0, -1 } };
    const uc_webview_cookie keep[] = { { "uc_keep", "k1", NULL, NULL, 0, 0, tomorrow } };
    const Record *r;
    int rc, status;

    rc = set_cookies (base, cookies, 1, &status);
    check (rc == 0 && status == UC_WEBVIEW_DONE_OK && load (page, "done:") && (r = find (st.site_host, "/cookies")) != NULL
           && has_cookie (r->cookie, "uc_clear"), "clear-setup", "a cookie to clear is there");
    rc = clear (UC_WEBVIEW_DATA_CACHE, &status);
    check (rc == 0 && status == UC_WEBVIEW_DONE_OK && load (page, "done:") && (r = find (st.site_host, "/cookies")) != NULL
           && has_cookie (r->cookie, "uc_clear"), "clear-cache", "clear_data(CACHE) %d, done %d, the cookie stays", rc,
           status);
    rc = clear (UC_WEBVIEW_DATA_ALL, &status);
    check (rc == 0 && status == UC_WEBVIEW_DONE_OK && load (page, "done:") && (r = find (st.site_host, "/cookies")) != NULL
           && r->cookie == NULL, "clear-all", "clear_data(ALL) %d, done %d, no cookie is left", rc, status);
    if (st.data_dir != NULL)
        check (!cookie_file_has ("uc_clear") && !cookie_file_has ("uc_keep"), "clear-all-written",
               "%s/cookies.sqlite has no cookie of the test left", st.data_dir);
    check (uc_webview_global_clear_data (0x2u, on_done, NULL) == -1 && uc_webview_global_clear_data (0, NULL, NULL) == -1,
           "clear-refused", "types other than CACHE and ALL return -1");
    /* for the next run's persistence check */
    if (st.data_dir != NULL) {
        rc = set_cookies (base, keep, 1, &status);
        check (rc == 0 && status == UC_WEBVIEW_DONE_OK && cookie_file_has ("uc_keep"), "clear-keep-again",
               "uc_keep is written again for the next run");
    }
    g_free (base);
    g_free (page);
}

/* ---- run --------------------------------------------------------------------------------------------------- */

int
site_test_run (int width, int height, double scale, const char *data_dir, int timeout_s)
{
    uc_webview_callbacks cb = {
        .frame_ready = on_frame, .load_changed = on_load, .title_changed = on_title,
        .web_process_terminated = on_terminated,
    };
    guint ticker;
    int code;

    st.events = g_ptr_array_new ();
    st.log = g_ptr_array_new_with_free_func (record_free);
    st.width = width;
    st.height = height;
    st.data_dir = data_dir;
    st.deadline = g_get_monotonic_time () + (gint64) timeout_s * G_USEC_PER_SEC;
    if (!servers_start ())
        return 2;
    if (st.tls_server != NULL)
        g_print ("site-test: servers %s and %s, HTTPS on ports %u and %u (127.0.0.2)\n", st.site, st.other, st.tls_port,
                 st.tls_port_other);
    else
        g_print ("site-test: servers %s and %s, no HTTPS (no --site-tls-cert)\n", st.site, st.other);
    st.wv = uc_webview_new_sized (width, height, &cb, sizeof cb, NULL);
    if (st.wv == NULL) {
        g_printerr ("site-test: uc_webview_new_sized() failed\n");
        return 2;
    }
    uc_webview_set_scale (st.wv, scale);
    ticker = g_timeout_add (20, tick, NULL);

    if (step_wanted ("ua") && !st.terminated && !st.timed_out)
        step_ua ();
    if (step_wanted ("cookies") && !st.terminated && !st.timed_out)
        step_cookies ();
    if (step_wanted ("headers") && !st.terminated && !st.timed_out)
        step_headers ();
    if (step_wanted ("tls") && !st.terminated && !st.timed_out)
        step_tls ();
    if (step_wanted ("scheme") && !st.terminated && !st.timed_out)
        step_scheme ();
    if (step_wanted ("clear") && !st.terminated && !st.timed_out)
        step_clear ();

    g_source_remove (ticker);
    if (st.held != NULL)
        uc_webview_frame_release (st.wv, st.held);
    uc_webview_free (st.wv);
    if (st.timed_out) {
        g_print ("site-test: %d passed, %d failed, %d skipped: TIMEOUT\n", st.pass, st.fail, st.skip);
        code = 2;
    } else if (st.terminated) {
        g_print ("site-test: %d passed, %d failed, %d skipped: FAILED (web process terminated)\n", st.pass, st.fail,
                 st.skip);
        code = 1;
    } else {
        g_print ("site-test: %d passed, %d failed, %d skipped: %s\n", st.pass, st.fail, st.skip,
                 st.fail == 0 ? "ok" : "FAILED");
        code = st.fail == 0 ? 0 : 1;
    }
    if (st.tls_server != NULL)
        soup_server_disconnect (st.tls_server);
    soup_server_disconnect (st.server);
    g_clear_object (&st.tls_server);
    g_clear_object (&st.server);
    g_ptr_array_unref (st.events);
    g_ptr_array_unref (st.log);
    return code;
}
