/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --load-fail-test of ucwebview-test (library 0.10.0), see ucwebview-test-loadfail.h. One HTTP server inside the tool
 * (libsoup on 127.0.0.1) serves the pages that must not fail, and with --load-fail-tls-cert an HTTPS server the one
 * that fails on its certificate. The certificate file is the one of --site-test (a PEM certificate and its key for
 * 127.0.0.1, ucwebview-test-site.c). Nothing goes to the network: the DNS step asks for a name under .invalid
 * (RFC 6761: never resolves), and the timeout step sends SYNs to 240.0.0.1 (reserved, never answers).
 */
#include "ucwebview-test-loadfail.h"

#include "uc_webview.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- options ----------------------------------------------------------------------------------------------- */

static gboolean opt_load_fail_test;
static char *opt_load_fail_tls_cert;
static char *opt_load_fail_steps;
static int opt_load_fail_tls_hosts = -1;

const GOptionEntry load_fail_test_entries[] = {
    { "load-fail-test", 0, 0, G_OPTION_ARG_NONE, &opt_load_fail_test,
      "Check load_failed_detail() (library 0.10.0): the reason of failed loads (DNS, refused, unreachable, certificate "
      "with --load-fail-tls-cert, scheme allowlist), one detail right before each FAILED, none for loads without FAILED "
      "(no network needed)", NULL },
    { "load-fail-tls-cert", 0, 0, G_OPTION_ARG_FILENAME, &opt_load_fail_tls_cert,
      "PEM file with a self-signed certificate and its key for 127.0.0.1 (the one of --site-tls-cert), for the "
      "certificate steps of --load-fail-test", "FILE" },
    { "load-fail-steps", 0, 0, G_OPTION_ARG_STRING, &opt_load_fail_steps,
      "The --load-fail-test steps, comma separated (default: all): dns, refused, unreachable, timeout, tls, scheme, "
      "cancelled, attachment, http-error, old-host, tls-race", "LIST" },
    { "load-fail-tls-hosts", 0, 0, G_OPTION_ARG_INT, &opt_load_fail_tls_hosts,
      "Loads of the tls-race step (default 24, 2 to 48): HTTPS loads of new hosts 127.0.1.1, 127.0.1.2, ... whose "
      "certificate WebKit has not seen, half of them with an exception, timed against WebKit's preconnect (library "
      "0.10.3)", "N" },
    { NULL }
};

static const char *const all_steps[] = { "dns", "refused", "unreachable", "timeout", "tls", "scheme", "cancelled",
                                         "attachment", "http-error", "old-host", "tls-race" };

#define TLS_RACE_HOSTS_DEFAULT 24
#define TLS_RACE_HOSTS_MAX     48

gboolean
load_fail_test_mode (void)
{
    return opt_load_fail_test;
}

gboolean
load_fail_test_check_options (void)
{
    if (!opt_load_fail_test) {
        if (opt_load_fail_tls_cert != NULL || opt_load_fail_steps != NULL || opt_load_fail_tls_hosts != -1) {
            g_printerr ("ucwebview-test: --load-fail-tls-cert, --load-fail-steps and --load-fail-tls-hosts go with "
                        "--load-fail-test\n");
            return FALSE;
        }
        return TRUE;
    }
    if (opt_load_fail_tls_hosts == -1) {
        opt_load_fail_tls_hosts = TLS_RACE_HOSTS_DEFAULT;
    } else if (opt_load_fail_tls_hosts < 2 || opt_load_fail_tls_hosts > TLS_RACE_HOSTS_MAX) {
        g_printerr ("ucwebview-test: --load-fail-tls-hosts takes 2 to %d\n", TLS_RACE_HOSTS_MAX);
        return FALSE;
    }
    if (opt_load_fail_steps != NULL) {
        char **names = g_strsplit (opt_load_fail_steps, ",", -1);
        for (char **n = names; *n != NULL; n++) {
            gboolean known = FALSE;
            for (gsize i = 0; i < G_N_ELEMENTS (all_steps); i++)
                known = known || g_strcmp0 (g_strstrip (*n), all_steps[i]) == 0;
            if (!known) {
                g_printerr ("ucwebview-test: unknown --load-fail-steps entry \"%s\"\n", *n);
                g_strfreev (names);
                return FALSE;
            }
        }
        g_strfreev (names);
    }
    return TRUE;
}

int
load_fail_test_default_timeout (void)
{
    /* ten loads, the timeout step about 10 s; 200 s for a kernel connect timeout instead (about 130 s) */
    return 360;
}

static gboolean
step_wanted (const char *name)
{
    gboolean wanted = FALSE;
    char **names;

    if (opt_load_fail_steps == NULL)
        return TRUE;
    names = g_strsplit (opt_load_fail_steps, ",", -1);
    for (char **n = names; *n != NULL; n++)
        wanted = wanted || g_strcmp0 (g_strstrip (*n), name) == 0;
    g_strfreev (names);
    return wanted;
}

/* ---- state ------------------------------------------------------------------------------------------------- */

/* one callback of a view: a load event or a detail */
typedef struct {
    const char *kind;       /* "started", "redirected", "committed", "finished", "failed", "detail" */
    uc_webview *wv;
    char *uri;
    int reason;
    char *domain;
    int code;
    unsigned tls_errors;
    char *message;
} Event;

static struct {
    uc_webview *wv;
    void *held;
    GPtrArray *events;      /* Event*, of every view */
    char *title;
    gboolean terminated;
    SoupServer *server, *tls_server;
    char *site;             /* http://127.0.0.1:P */
    char *tls_site;         /* https://127.0.0.1:Q, NULL without --load-fail-tls-cert */
    char *tls_cert_sha256;
    guint16 race_ports[TLS_RACE_HOSTS_MAX];       /* the HTTPS listeners of 127.0.1.1, 127.0.1.2, ... (tls-race) */
    guint16 race_http_ports[TLS_RACE_HOSTS_MAX];  /* and the HTTP ones */
    guint n_race;
    SoupServerMessage *paused;  /* the /slow request, answered at the end */
    int pass, fail, skip;
    gint64 deadline;
    gboolean timed_out;
    int width, height;
    double scale;
} st;

static void
event_free (gpointer data)
{
    Event *e = data;
    g_free (e->uri);
    g_free (e->domain);
    g_free (e->message);
    g_free (e);
}

static void G_GNUC_PRINTF (3, 4)
check (gboolean ok, const char *name, const char *format, ...)
{
    va_list args;
    char *why;

    va_start (args, format);
    why = g_strdup_vprintf (format, args);
    va_end (args);
    g_print ("load-fail-test: %s %s: %s\n", ok ? "PASS" : "FAIL", name, why);
    g_free (why);
    if (ok)
        st.pass++;
    else
        st.fail++;
}

static void
skip (const char *name, const char *why)
{
    g_print ("load-fail-test: SKIP %s: %s\n", name, why);
    st.skip++;
}

static const char *
reason_name (int reason)
{
    static const char *const names[] = { "OTHER", "HOST_NOT_FOUND", "NO_NETWORK", "CONNECTION_REFUSED", "TIMEOUT",
                                         "CERTIFICATE", "SCHEME_REFUSED" };
    return reason >= 0 && reason < (int) G_N_ELEMENTS (names) ? names[reason] : "?";
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

static gboolean
never (gconstpointer data)
{
    (void) data;
    return FALSE;
}

static guint
count (const char *kind)
{
    guint n = 0;
    for (guint i = 0; i < st.events->len; i++)
        n += g_strcmp0 (((Event *) g_ptr_array_index (st.events, i))->kind, kind) == 0;
    return n;
}

static gboolean
failed_and_finished (gconstpointer data)
{
    (void) data;
    return count ("failed") >= 1 && count ("finished") >= 1;
}

static gboolean
failed_n (gconstpointer n)
{
    return count ("failed") >= GPOINTER_TO_UINT (n);
}

static gboolean
finished_n (gconstpointer n)
{
    return count ("finished") >= GPOINTER_TO_UINT (n);
}

static gboolean
started (gconstpointer data)
{
    (void) data;
    return count ("started") >= 1 && st.paused != NULL;
}

static gboolean
title_is (gconstpointer title)
{
    return g_strcmp0 (st.title, title) == 0;
}

/* What the previous load still sends (WebKit shows an error page after a failed load: its STARTED, COMMITTED and
 * FINISHED come after the failure's FINISHED) must not count for the next one. */
static void
settle (void)
{
    spin_until (never, NULL, 300);
}

static void
begin (void)
{
    settle ();
    g_ptr_array_set_size (st.events, 0);
    g_clear_pointer (&st.title, g_free);
}

/* the events in one line, for the check's text */
static char *
describe (void)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < st.events->len; i++) {
        const Event *e = g_ptr_array_index (st.events, i);
        if (s->len > 0)
            g_string_append (s, ", ");
        if (g_strcmp0 (e->kind, "detail") == 0)
            g_string_append_printf (s, "detail(%s, \"%s\" %d, tls 0x%x, \"%s\")", reason_name (e->reason), e->domain,
                                    e->code, e->tls_errors, e->message);
        else
            g_string_append (s, e->kind);
    }
    if (s->len == 0)
        g_string_append (s, "no event");
    return g_string_free (s, FALSE);
}

/*
 * One failure in the events: exactly one detail and one FAILED, the FAILED the next event after the detail, both with
 * the same uri (expect_uri when not NULL), the detail with reason and domain (code when not -1, tls_bit when not 0).
 */
static void
check_failure (const char *name, const char *expect_uri, int reason, const char *domain, int code, unsigned tls_bit)
{
    const Event *detail = NULL, *failed = NULL;
    guint detail_at = 0, failed_at = 0;
    char *seen = describe ();
    gboolean ok;

    for (guint i = 0; i < st.events->len; i++) {
        const Event *e = g_ptr_array_index (st.events, i);
        if (g_strcmp0 (e->kind, "detail") == 0 && detail == NULL) {
            detail = e;
            detail_at = i;
        } else if (g_strcmp0 (e->kind, "failed") == 0 && failed == NULL) {
            failed = e;
            failed_at = i;
        }
    }
    check (count ("detail") == 1 && count ("failed") == 1 && detail != NULL && failed != NULL
               && failed_at == detail_at + 1,
           name, "one detail right before one FAILED: %s", seen);
    if (detail == NULL || failed == NULL) {
        g_free (seen);
        return;
    }
    ok = g_strcmp0 (detail->uri, failed->uri) == 0 && (expect_uri == NULL || g_strcmp0 (detail->uri, expect_uri) == 0);
    {
        char *check_name = g_strconcat (name, "-uri", NULL);
        check (ok, check_name, "detail %s, FAILED %s", detail->uri, failed->uri);
        g_free (check_name);
    }
    ok = detail->reason == reason && g_strcmp0 (detail->domain, domain) == 0 && (code == -1 || detail->code == code)
         && (tls_bit == 0 ? detail->tls_errors == 0 : (detail->tls_errors & tls_bit) != 0);
    {
        char *check_name = g_strconcat (name, "-reason", NULL);
        char *want_code = code == -1 ? g_strdup ("any code") : g_strdup_printf ("%d", code);
        check (ok, check_name, "%s (%s %d, tls 0x%x, \"%s\"), expected %s (%s %s%s)", reason_name (detail->reason),
               detail->domain, detail->code, detail->tls_errors, detail->message, reason_name (reason), domain,
               want_code, tls_bit != 0 ? ", tls bit" : "");
        g_free (want_code);
        g_free (check_name);
    }
    g_free (seen);
}

/* no detail and no FAILED in the events */
static void
check_no_failure (const char *name, gboolean other_ok, const char *what)
{
    char *seen = describe ();
    check (other_ok && count ("detail") == 0 && count ("failed") == 0, name, "%s, no detail, no FAILED: %s", what, seen);
    g_free (seen);
}

/* ---- library callbacks ------------------------------------------------------------------------------------- */

/* user: the view's slot for its held frame */
static void
on_frame (uc_webview *wv, const uc_webview_frame *frame, void *user)
{
    void **held = user;
    uc_webview_frame_rendered (wv, frame->handle);
    if (*held != NULL)
        uc_webview_frame_release (wv, *held);
    *held = frame->handle;
}

static void
on_title (uc_webview *wv, const char *title, void *user)
{
    (void) user;
    if (wv != st.wv)
        return;
    g_free (st.title);
    st.title = g_strdup (title);
}

static void
on_load (uc_webview *wv, int event, const char *uri, void *user)
{
    static const char *const names[] = { "started", "redirected", "committed", "finished", "failed" };
    Event *e;

    (void) user;
    if (event < 0 || event >= (int) G_N_ELEMENTS (names))
        return;
    e = g_new0 (Event, 1);
    e->kind = names[event];
    e->wv = wv;
    e->uri = g_strdup (uri);
    g_ptr_array_add (st.events, e);
}

static void
on_detail (uc_webview *wv, const char *uri, int reason, const char *error_domain, int error_code, unsigned tls_errors,
           const char *message, void *user)
{
    Event *e = g_new0 (Event, 1);

    (void) user;
    e->kind = "detail";
    e->wv = wv;
    e->uri = g_strdup (uri);
    e->reason = reason;
    e->domain = g_strdup (error_domain);
    e->code = error_code;
    e->tls_errors = tls_errors;
    e->message = g_strdup (message);
    g_ptr_array_add (st.events, e);
    if (error_domain == NULL || message == NULL)
        check (FALSE, "detail-strings", "error_domain %s, message %s: never NULL", error_domain ? "set" : "NULL",
               message ? "set" : "NULL");
}

static void
on_terminated (uc_webview *wv, int reason, void *user)
{
    (void) wv; (void) user;
    g_printerr ("load-fail-test: web process terminated (reason %d)\n", reason);
    st.terminated = TRUE;
}

/* ---- server ------------------------------------------------------------------------------------------------ */

static void
respond_html (SoupServerMessage *msg, guint status, const char *html)
{
    soup_message_headers_replace (soup_server_message_get_response_headers (msg), "Cache-Control", "no-store");
    soup_server_message_set_status (msg, status, NULL);
    soup_server_message_set_response (msg, "text/html; charset=utf-8", SOUP_MEMORY_COPY, html, strlen (html));
}

static void
serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data)
{
    (void) server; (void) query; (void) user_data;
    if (g_strcmp0 (path, "/ok") == 0) {
        respond_html (msg, SOUP_STATUS_OK, "<!doctype html><title>done:ok</title><body>ok</body>");
    } else if (g_strcmp0 (path, "/tls") == 0) {
        respond_html (msg, SOUP_STATUS_OK, "<!doctype html><title>done:tls</title><body>tls</body>");
    } else if (g_strcmp0 (path, "/slow") == 0) {
        /* never answered while the test runs: the load is replaced by the next one */
        if (st.paused == NULL) {
            st.paused = g_object_ref (msg);
            soup_server_message_pause (msg);
        } else {
            respond_html (msg, SOUP_STATUS_OK, "<!doctype html><title>done:slow</title>");
        }
    } else if (g_strcmp0 (path, "/attachment") == 0) {
        static const char text[] = "load-fail-test attachment\n";
        soup_message_headers_replace (soup_server_message_get_response_headers (msg), "Content-Disposition",
                                      "attachment; filename=\"uc.txt\"");
        soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response (msg, "text/plain", SOUP_MEMORY_COPY, text, sizeof text - 1);
    } else {
        respond_html (msg, SOUP_STATUS_NOT_FOUND, "<!doctype html><title>done:404</title><body>not found</body>");
    }
}

static guint16
first_port (SoupServer *server)
{
    GSList *uris = soup_server_get_uris (server);
    guint16 port = uris != NULL ? (guint16) g_uri_get_port (uris->data) : 0;
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    return port;
}

/* A listening socket on ip for server (plain http), the port the kernel's choice; 0 on failure. */
static guint16
listen_on (SoupServer *server, const char *ip)
{
    GError *error = NULL;
    GInetAddress *inet = g_inet_address_new_from_string (ip);
    GSocketAddress *address = g_inet_socket_address_new (inet, 0);
    GSocket *socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error);
    GSocketAddress *bound = NULL;
    guint16 port = 0;

    if (socket != NULL && g_socket_bind (socket, address, TRUE, &error) && g_socket_listen (socket, &error)
        && (bound = g_socket_get_local_address (socket, &error)) != NULL
        && soup_server_listen_socket (server, socket, 0, &error))
        port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));
    else
        g_printerr ("load-fail-test: cannot listen on %s: %s\n", ip, error != NULL ? error->message : "unknown error");
    g_clear_error (&error);
    g_clear_object (&bound);
    g_clear_object (&socket);
    g_object_unref (address);
    g_object_unref (inet);
    return port;
}

/* The HTTPS server of tls-race runs in a thread of its own with its own main context: the tool holds its main thread
 * (the UI process' thread, which also runs st.server) after a load call, and that must delay only the web process'
 * request, not the TLS handshake of WebKit's preconnect. It accepts the TCP connections itself, to count them (one
 * per load: the main request took over the preconnect's connection; two: it came after the preconnect had failed and
 * opened its own), and hands each to a SoupServer as a TLS server stream. */
static struct {
    GThread *thread;
    GMainContext *context;
    GMainLoop *loop;
    GTlsCertificate *cert;
    SoupServer *server;
    gint connections;       /* atomic */
    GMutex lock;
    GCond cond;
    gboolean ready, ok;
} race;

static void
race_serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data)
{
    (void) server; (void) query; (void) user_data;
    if (g_strcmp0 (path, "/tls") == 0)
        respond_html (msg, SOUP_STATUS_OK, "<!doctype html><title>done:tls</title><body>tls</body>");
    else
        respond_html (msg, SOUP_STATUS_NOT_FOUND, "<!doctype html><title>done:404</title><body>not found</body>");
}

static gboolean
race_incoming (GSocketService *service, GSocketConnection *connection, GObject *source, gpointer user_data)
{
    GError *error = NULL;
    GIOStream *tls;

    (void) service; (void) source; (void) user_data;
    g_atomic_int_inc (&race.connections);
    tls = g_tls_server_connection_new (G_IO_STREAM (connection), race.cert, &error);
    if (tls != NULL) {
        GSocketAddress *local = g_socket_connection_get_local_address (connection, NULL);
        GSocketAddress *remote = g_socket_connection_get_remote_address (connection, NULL);
        /* the handshake runs with the server's first read */
        soup_server_accept_iostream (race.server, tls, local, remote, &error);
        g_clear_object (&local);
        g_clear_object (&remote);
        g_object_unref (tls);
    }
    if (error != NULL) {
        g_printerr ("load-fail-test: tls-race: %s\n", error->message);
        g_clear_error (&error);
    }
    return TRUE;
}

static gpointer
race_thread (gpointer data)
{
    GSocketService *service;
    gboolean ok = TRUE;

    (void) data;
    g_main_context_push_thread_default (race.context);
    race.server = soup_server_new ("server-header", "ucwebview-test", NULL);
    soup_server_add_handler (race.server, NULL, race_serve, NULL, NULL);
    service = g_socket_service_new ();
    for (guint i = 0; i < (guint) opt_load_fail_tls_hosts && ok; i++) {
        GError *error = NULL;
        char ip[32];
        GInetAddress *inet;
        GSocketAddress *address, *effective = NULL;

        g_snprintf (ip, sizeof ip, "127.0.1.%u", i + 1);
        inet = g_inet_address_new_from_string (ip);
        address = g_inet_socket_address_new (inet, 0);
        ok = g_socket_listener_add_address (G_SOCKET_LISTENER (service), address, G_SOCKET_TYPE_STREAM,
                                            G_SOCKET_PROTOCOL_TCP, NULL, &effective, &error);
        if (ok) {
            st.race_ports[i] = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
        } else {
            g_printerr ("load-fail-test: cannot listen for HTTPS on %s: %s\n", ip, error->message);
            g_clear_error (&error);
        }
        g_clear_object (&effective);
        g_object_unref (address);
        g_object_unref (inet);
    }
    if (ok) {
        g_signal_connect (service, "incoming", G_CALLBACK (race_incoming), NULL);
        g_socket_service_start (service);
    }
    g_mutex_lock (&race.lock);
    race.ready = TRUE;
    race.ok = ok;
    g_cond_signal (&race.cond);
    g_mutex_unlock (&race.lock);
    if (ok)
        g_main_loop_run (race.loop);
    g_socket_service_stop (service);
    g_socket_listener_close (G_SOCKET_LISTENER (service));
    g_object_unref (service);
    soup_server_disconnect (race.server);
    g_clear_object (&race.server);
    g_main_context_pop_thread_default (race.context);
    return NULL;
}

/* tls-race: plain http on 127.0.1.1, 127.0.1.2, ... (all of 127.0.0.0/8 is loopback on Linux) from st.server, the
 * pages before the HTTPS loads of the same hosts, so that WebKit does not swap the web process for them; HTTPS from
 * the thread above. The certificate is not for these addresses: WebKit sees BAD_IDENTITY next to UNKNOWN_CA. */
static gboolean
race_listen (GTlsCertificate *cert)
{
    for (guint i = 0; i < (guint) opt_load_fail_tls_hosts; i++) {
        char ip[32];

        g_snprintf (ip, sizeof ip, "127.0.1.%u", i + 1);
        st.race_http_ports[i] = listen_on (st.server, ip);
        if (st.race_http_ports[i] == 0)
            return FALSE;
    }
    g_mutex_init (&race.lock);
    g_cond_init (&race.cond);
    race.context = g_main_context_new ();
    race.loop = g_main_loop_new (race.context, FALSE);
    race.cert = g_object_ref (cert);
    race.thread = g_thread_new ("tls-race", race_thread, NULL);
    g_mutex_lock (&race.lock);
    while (!race.ready)
        g_cond_wait (&race.cond, &race.lock);
    g_mutex_unlock (&race.lock);
    if (race.ok)
        st.n_race = (guint) opt_load_fail_tls_hosts;
    return race.ok;
}

static gboolean
race_quit (gpointer data)
{
    (void) data;
    g_main_loop_quit (race.loop);
    return G_SOURCE_REMOVE;
}

static void
race_stop (void)
{
    if (race.thread == NULL)
        return;
    g_main_context_invoke (race.context, race_quit, NULL);
    g_thread_join (race.thread);
    race.thread = NULL;
    g_main_loop_unref (race.loop);
    g_main_context_unref (race.context);
    g_clear_object (&race.cert);
    g_mutex_clear (&race.lock);
    g_cond_clear (&race.cond);
}

static gboolean
servers_start (void)
{
    GError *error = NULL;
    GInetAddress *loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new (loopback, 0);

    st.server = soup_server_new ("server-header", "ucwebview-test", NULL);
    soup_server_add_handler (st.server, NULL, serve, NULL, NULL);
    if (!soup_server_listen (st.server, address, 0, &error)) {
        g_printerr ("load-fail-test: cannot listen on 127.0.0.1: %s\n", error->message);
        g_clear_error (&error);
        g_object_unref (address);
        g_object_unref (loopback);
        return FALSE;
    }
    st.site = g_strdup_printf ("http://127.0.0.1:%u", first_port (st.server));

    if (opt_load_fail_tls_cert != NULL) {
        GTlsCertificate *cert = g_tls_certificate_new_from_file (opt_load_fail_tls_cert, &error);
        GByteArray *der = NULL;

        if (cert == NULL) {
            g_printerr ("load-fail-test: cannot read --load-fail-tls-cert: %s\n", error->message);
            g_clear_error (&error);
            g_object_unref (address);
            g_object_unref (loopback);
            return FALSE;
        }
        g_object_get (cert, "certificate", &der, NULL);
        st.tls_cert_sha256 = g_compute_checksum_for_data (G_CHECKSUM_SHA256, der->data, der->len);
        g_byte_array_unref (der);
        st.tls_server = soup_server_new ("server-header", "ucwebview-test", "tls-certificate", cert, NULL);
        soup_server_add_handler (st.tls_server, NULL, serve, NULL, NULL);
        if (!soup_server_listen (st.tls_server, address, SOUP_SERVER_LISTEN_HTTPS, &error)
            || (step_wanted ("tls-race") && !race_listen (cert))) {
            if (error != NULL)
                g_printerr ("load-fail-test: cannot listen for HTTPS on 127.0.0.1: %s\n", error->message);
            g_clear_error (&error);
            g_object_unref (cert);
            g_object_unref (address);
            g_object_unref (loopback);
            return FALSE;
        }
        g_object_unref (cert);
        st.tls_site = g_strdup_printf ("https://127.0.0.1:%u", first_port (st.tls_server));
    }
    g_object_unref (address);
    g_object_unref (loopback);
    return TRUE;
}

/* a port on 127.0.0.1 nobody listens on: bound by the kernel's choice, then closed again */
static guint16
closed_port (void)
{
    GError *error = NULL;
    GSocket *socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error);
    GInetAddress *loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new (loopback, 0);
    guint16 port = 0;

    if (socket != NULL && g_socket_bind (socket, address, FALSE, &error)) {
        GSocketAddress *bound = g_socket_get_local_address (socket, &error);
        if (bound != NULL) {
            port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));
            g_object_unref (bound);
        }
    }
    if (error != NULL) {
        g_printerr ("load-fail-test: no free port: %s\n", error->message);
        g_clear_error (&error);
    }
    if (socket != NULL) {
        g_socket_close (socket, NULL);
        g_object_unref (socket);
    }
    g_object_unref (address);
    g_object_unref (loopback);
    return port;
}

/* ---- steps ------------------------------------------------------------------------------------------------- */

/* Loads uri and waits for its FAILED and FINISHED (and the error page WebKit shows after them), at most timeout_ms. */
static void
load_failing (const char *uri, guint timeout_ms)
{
    begin ();
    uc_webview_load_uri (st.wv, uri);
    spin_until (failed_and_finished, NULL, timeout_ms);
    settle ();      /* a second detail or FAILED would show here */
}

static void
step_dns (void)
{
    static const char uri[] = "http://load-fail-test.invalid/";
    /* NOT_FOUND (0) from a DNS server, TEMPORARY_FAILURE (1) without one: both HOST_NOT_FOUND */
    load_failing (uri, 15000);
    check_failure ("dns", uri, UC_WEBVIEW_FAIL_HOST_NOT_FOUND, "g-resolver-error-quark", -1, 0);
}

static void
step_refused (void)
{
    guint16 port = closed_port ();
    char *uri;

    if (port == 0) {
        check (FALSE, "refused", "no free port on 127.0.0.1");
        return;
    }
    uri = g_strdup_printf ("http://127.0.0.1:%u/", port);
    load_failing (uri, 10000);
    check_failure ("refused", uri, UC_WEBVIEW_FAIL_CONNECTION_REFUSED, "g-io-error-quark", G_IO_ERROR_CONNECTION_REFUSED,
                   0);
    g_free (uri);
}

static void
step_unreachable (void)
{
    /* connect() to the limited broadcast address fails with ENETUNREACH on Linux, with any routing table */
    static const char uri[] = "http://255.255.255.255/";
    load_failing (uri, 10000);
    check_failure ("unreachable", uri, UC_WEBVIEW_FAIL_NO_NETWORK, "g-io-error-quark", G_IO_ERROR_NETWORK_UNREACHABLE,
                   0);
}

static void
step_tls (void)
{
    char *uri, *ok_uri;
    int rc;

    if (st.tls_server == NULL) {
        skip ("tls", "no --load-fail-tls-cert");
        return;
    }
    uri = g_strconcat (st.tls_site, "/tls", NULL);
    load_failing (uri, 10000);
    /* a self-signed certificate: WebKit's G_TLS_ERROR_BAD_CERTIFICATE (ResourceErrorSoup.cpp:101-107), flags with
     * UNKNOWN_CA from load-failed-with-tls-errors */
    check_failure ("tls", uri, UC_WEBVIEW_FAIL_CERTIFICATE, "g-tls-error-quark", G_TLS_ERROR_BAD_CERTIFICATE,
                   G_TLS_CERTIFICATE_UNKNOWN_CA);

    /* the same certificate with an exception: the library accepts it and loads again, no failure at all. An http page
     * between the attempts, as --site-test does. The load right after it failed now and then up to library 0.10.2,
     * with "Operation was cancelled" before any certificate check (see step_tls_race()) */
    ok_uri = g_strconcat (st.site, "/ok", NULL);
    begin ();
    uc_webview_load_uri (st.wv, ok_uri);
    spin_until (title_is, "done:ok", 10000);
    rc = uc_webview_global_allow_certificate ("127.0.0.1", st.tls_cert_sha256);
    begin ();
    uc_webview_load_uri (st.wv, uri);
    spin_until (title_is, "done:tls", 10000);
    settle ();
    check_no_failure ("tls-accepted", rc == 0 && title_is ("done:tls"), "with an exception the page shows");
    g_free (ok_uri);
    g_free (uri);
}

static void
step_scheme (void)
{
    static const char ftp[] = "ftp://example.com/file", bare[] = "example.com";
    gboolean ok = TRUE;
    char *seen;

    begin ();
    uc_webview_load_uri (st.wv, ftp);
    uc_webview_load_uri (st.wv, bare);
    /* nothing from inside the calls: the reports come from an idle */
    ok = st.events->len == 0;
    spin_until (failed_n, GUINT_TO_POINTER (2), 5000);
    settle ();
    seen = describe ();
    if (st.events->len != 4) {
        ok = FALSE;
    } else {
        const char *const uris[] = { ftp, ftp, bare, bare };
        for (guint i = 0; i < 4; i++) {
            const Event *e = g_ptr_array_index (st.events, i);
            ok = ok && g_strcmp0 (e->kind, i % 2 == 0 ? "detail" : "failed") == 0 && g_strcmp0 (e->uri, uris[i]) == 0;
            if (i % 2 == 0)
                ok = ok && e->reason == UC_WEBVIEW_FAIL_SCHEME_REFUSED && g_strcmp0 (e->domain, "") == 0 && e->code == 0
                     && e->tls_errors == 0 && g_strcmp0 (e->message, "") == 0;
        }
    }
    check (ok, "scheme", "two refused URIs: detail(SCHEME_REFUSED, \"\" 0, tls 0, \"\") and FAILED each, in order, no "
                         "STARTED: %s", seen);
    g_free (seen);
}

static void
step_cancelled (void)
{
    char *slow = g_strconcat (st.site, "/slow", NULL), *ok_uri = g_strconcat (st.site, "/ok", NULL);
    gboolean ok;

    begin ();
    uc_webview_load_uri (st.wv, slow);
    ok = spin_until (started, NULL, 10000);
    /* a newer load replaces the waiting one: WebKit fails it with WEBKIT_NETWORK_ERROR_CANCELLED */
    uc_webview_load_uri (st.wv, ok_uri);
    ok = spin_until (title_is, "done:ok", 10000) && ok;
    settle ();
    check_no_failure ("cancelled", ok, "a load replaced by a newer one");
    g_free (slow);
    g_free (ok_uri);
}

static void
step_attachment (void)
{
    char *uri = g_strconcat (st.site, "/attachment", NULL);
    gboolean ok;

    begin ();
    uc_webview_load_uri (st.wv, uri);
    ok = spin_until (finished_n, GUINT_TO_POINTER (1), 10000);
    settle ();
    check_no_failure ("attachment", ok, "a refused response (FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)");
    g_free (uri);
}

static void
step_http_error (void)
{
    char *uri = g_strconcat (st.site, "/missing", NULL);
    gboolean ok;

    begin ();
    uc_webview_load_uri (st.wv, uri);
    ok = spin_until (title_is, "done:404", 10000);
    settle ();
    check_no_failure ("http-error", ok, "a 404 page shows");
    g_free (uri);
}

/* A host of a 0.7.0 to 0.9.x header passes its struct up to response_refused: the library must not read, let alone
 * call, a load_failed_detail after it. The struct here has the field set, the size stops before it. */
static void
step_old_host (void)
{
    static const char uri[] = "http://load-fail-test.invalid/old-host";
    void *held = NULL;
    uc_webview_callbacks cb;
    uc_webview *old;
    gboolean ok;
    char *seen;

    memset (&cb, 0, sizeof cb);
    cb.frame_ready = on_frame;
    cb.load_changed = on_load;
    cb.web_process_terminated = on_terminated;
    cb.load_failed_detail = on_detail;
    old = uc_webview_new_sized (st.width, st.height, &cb, offsetof (uc_webview_callbacks, load_failed_detail), &held);
    if (old == NULL) {
        check (FALSE, "old-host", "uc_webview_new_sized() with the 0.9 size failed");
        return;
    }
    uc_webview_set_scale (old, st.scale);
    begin ();
    uc_webview_load_uri (old, uri);
    ok = spin_until (failed_and_finished, NULL, 15000);
    settle ();
    seen = describe ();
    check (ok && count ("failed") == 1 && count ("detail") == 0, "old-host",
           "a 0.9 host's struct: FAILED without a detail: %s", seen);
    g_free (seen);
    if (held != NULL)
        uc_webview_frame_release (old, held);
    uc_webview_free (old);
}

static void
step_timeout (void)
{
    /* 240.0.0.0/4 is reserved: the SYNs go out by the default route and nobody answers. WebKit's preconnect gives up
     * after 10 s and its cancel ends the main request (G_IO_ERROR_CANCELLED, uc_load_error.h); without the preconnect
     * the kernel's connect timeout would (G_IO_ERROR_TIMED_OUT, tcp_syn_retries 6: about 130 s) */
    static const char uri[] = "http://240.0.0.1/";
    g_print ("load-fail-test: timeout step: a host that never answers (about 10 s)\n");
    load_failing (uri, 200000);
    check_failure ("timeout", uri, UC_WEBVIEW_FAIL_TIMEOUT, "g-io-error-quark", -1, 0);
}

static gboolean
failed_and_finished_or_title (gconstpointer title)
{
    return failed_and_finished (NULL) || title_is (title);
}

/*
 * tls-race (library 0.10.3): HTTPS loads of hosts whose certificate WebKit has not seen yet, a new one each time
 * (127.0.1.1, 127.0.1.2, ...). Before a main-frame load the UI process asks the network process to preconnect to the
 * host (WebPageProxy.cpp:2343), and the web process' request comes after it. When the request comes while the
 * preconnect is still connecting, it takes over the connection and sees the certificate itself; when it comes after
 * the preconnect has failed on the certificate, it opens its own. In between, the preconnect has rejected the
 * certificate and cancelled itself, but its connection is still handed over, and the load fails with "Operation was
 * cancelled" without a certificate check (uc_load_error.h). The library loads such a load again.
 *
 * To land in between, the tool holds its main thread (the UI process; the HTTPS server runs in a thread of its own)
 * for a while after each load call, which delays only the web process' request, and moves the hold up after a load
 * whose request took over the preconnect's connection (one connection at the server) and down after one that opened
 * its own (two), with a step that halves at every turn: a staircase around the point where the order flips. Before
 * each HTTPS load an http page of the same host, so that WebKit keeps the web process. Even attempts have an
 * exception for the certificate's fingerprint (the page must show without detail or FAILED), odd ones none (exactly
 * one detail, CERTIFICATE). The summary counts the loads the library repeated after such a cancellation, seen as a
 * third STARTED (an accepted certificate brings a second one, and so does WebKit's error page after a failure).
 */
#define TLS_RACE_STEP_START_US 2000
#define TLS_RACE_STEP_MIN_US   100

static void
step_tls_race (void)
{
    gint64 hold_us = 0, step_us = TLS_RACE_STEP_START_US;
    int last_move = 0;
    guint bad = 0, repeated = 0, taken = 0, own = 0;

    if (st.tls_server == NULL) {
        skip ("tls-race", "no --load-fail-tls-cert");
        return;
    }
    for (guint i = 0; i < st.n_race && !st.terminated && !st.timed_out; i++) {
        char host[32];
        char *uri, *gap;
        gboolean with_exception = i % 2 == 0, ok;
        int before, connections = -1, move;
        guint started;

        g_snprintf (host, sizeof host, "127.0.1.%u", i + 1);
        uri = g_strdup_printf ("https://%s:%u/tls", host, st.race_ports[i]);
        gap = g_strdup_printf ("http://%s:%u/ok", host, st.race_http_ports[i]);
        begin ();
        uc_webview_load_uri (st.wv, gap);
        spin_until (title_is, "done:ok", 10000);
        if (with_exception && uc_webview_global_allow_certificate (host, st.tls_cert_sha256) != 0)
            check (FALSE, "tls-race", "allow_certificate(%s) refused", host);
        begin ();
        before = g_atomic_int_get (&race.connections);
        uc_webview_load_uri (st.wv, uri);
        g_usleep ((gulong) hold_us);
        /* the connections of the first load, before anything the library repeats */
        if (spin_until (finished_n, GUINT_TO_POINTER (1), 15000))
            connections = g_atomic_int_get (&race.connections) - before;
        if (with_exception) {
            spin_until (failed_and_finished_or_title, "done:tls", 15000);
            settle ();
            started = count ("started");
            ok = title_is ("done:tls") && count ("detail") == 0 && count ("failed") == 0;
            repeated += started >= 3;
        } else {
            spin_until (failed_and_finished, NULL, 15000);
            settle ();
            started = count ("started");
            ok = count ("detail") == 1 && count ("failed") == 1;
            for (guint k = 0; ok && k < st.events->len; k++) {
                const Event *e = g_ptr_array_index (st.events, k);
                if (g_strcmp0 (e->kind, "detail") == 0)
                    ok = e->reason == UC_WEBVIEW_FAIL_CERTIFICATE
                         && (e->tls_errors & G_TLS_CERTIFICATE_UNKNOWN_CA) != 0;
            }
            repeated += started >= 3;
        }
        if (!ok) {
            char *seen = describe ();
            bad++;
            g_print ("load-fail-test: tls-race %s (%s, hold %" G_GINT64_FORMAT " us, %d connection(s)): %s\n", host,
                     with_exception ? "exception" : "no exception", hold_us, connections, seen);
            g_free (seen);
        }
        /* the staircase */
        move = connections == 1 ? 1 : connections >= 2 ? -1 : 0;
        taken += connections == 1;
        own += connections >= 2;
        if (move != 0 && last_move != 0 && move != last_move)
            step_us = MAX (step_us / 2, TLS_RACE_STEP_MIN_US);
        if (move != 0)
            last_move = move;
        hold_us = MAX (hold_us + move * step_us, 0);
        g_free (gap);
        g_free (uri);
    }
    check (bad == 0, "tls-race", "%u new hosts, %u with an exception (the page, no detail, no FAILED), %u without (one "
           "detail, CERTIFICATE): %u unexpected; the request took over the preconnect's connection %u time(s), opened "
           "its own %u time(s), last hold %" G_GINT64_FORMAT " us; %u load(s) repeated after a cancellation",
           st.n_race, (st.n_race + 1) / 2, st.n_race / 2, bad, taken, own, hold_us, repeated);
}

int
load_fail_test_run (int width, int height, double scale, int timeout_s)
{
    uc_webview_callbacks cb;
    guint ticker;
    int code;

    st.events = g_ptr_array_new_with_free_func (event_free);
    st.width = width;
    st.height = height;
    st.scale = scale;
    st.deadline = g_get_monotonic_time () + (gint64) timeout_s * G_USEC_PER_SEC;
    if (!servers_start ())
        return 2;
    g_print ("load-fail-test: server %s, %s\n", st.site, st.tls_site != NULL ? st.tls_site : "no HTTPS (no --load-fail-tls-cert)");

    memset (&cb, 0, sizeof cb);
    cb.frame_ready = on_frame;
    cb.load_changed = on_load;
    cb.title_changed = on_title;
    cb.web_process_terminated = on_terminated;
    cb.load_failed_detail = on_detail;
    st.wv = uc_webview_new_sized (width, height, &cb, sizeof cb, &st.held);
    if (st.wv == NULL) {
        g_printerr ("load-fail-test: uc_webview_new_sized() failed\n");
        return 2;
    }
    uc_webview_set_scale (st.wv, scale);
    ticker = g_timeout_add (20, tick, NULL);

#define STEP(name, fn) \
    if (step_wanted (name) && !st.terminated && !st.timed_out) \
        fn ()
    STEP ("dns", step_dns);
    STEP ("refused", step_refused);
    STEP ("unreachable", step_unreachable);
    STEP ("timeout", step_timeout);
    STEP ("tls", step_tls);
    STEP ("scheme", step_scheme);
    STEP ("cancelled", step_cancelled);
    STEP ("attachment", step_attachment);
    STEP ("http-error", step_http_error);
    STEP ("old-host", step_old_host);
    STEP ("tls-race", step_tls_race);
#undef STEP

    g_source_remove (ticker);
    if (st.held != NULL)
        uc_webview_frame_release (st.wv, st.held);
    uc_webview_free (st.wv);
    if (st.timed_out) {
        g_print ("load-fail-test: %d passed, %d failed, %d skipped: TIMEOUT\n", st.pass, st.fail, st.skip);
        code = 2;
    } else if (st.terminated) {
        g_print ("load-fail-test: %d passed, %d failed, %d skipped: FAILED (web process terminated)\n", st.pass, st.fail,
                 st.skip);
        code = 1;
    } else {
        g_print ("load-fail-test: %d passed, %d failed, %d skipped: %s\n", st.pass, st.fail, st.skip,
                 st.fail == 0 ? "ok" : "FAILED");
        code = st.fail == 0 ? 0 : 1;
    }
    if (st.paused != NULL) {
        soup_server_message_set_status (st.paused, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
        soup_server_message_unpause (st.paused);
        g_clear_object (&st.paused);
    }
    race_stop ();
    if (st.tls_server != NULL)
        soup_server_disconnect (st.tls_server);
    soup_server_disconnect (st.server);
    g_clear_object (&st.tls_server);
    g_clear_object (&st.server);
    g_clear_pointer (&st.site, g_free);
    g_clear_pointer (&st.tls_site, g_free);
    g_clear_pointer (&st.tls_cert_sha256, g_free);
    g_clear_pointer (&st.title, g_free);
    g_ptr_array_unref (st.events);
    return code;
}
