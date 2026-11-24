/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --process-test of ucwebview-test (library 0.10.4), see ucwebview-test-process.h. One libsoup server inside the tool
 * listens on 127.0.0.1 and 127.0.0.2 (two sites: WebKit gives each its own web process); nothing goes to the network.
 */
#include "ucwebview-test-process.h"

#include "uc_cache_model.h"
#include "uc_webview.h"

#include <dirent.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

/* ---- options ----------------------------------------------------------------------------------------------- */

static gboolean opt_process_test;
static int opt_process_wait = -1;

const GOptionEntry process_test_entries[] = {
    { "process-test", 0, 0, G_OPTION_ARG_NONE, &opt_process_test,
      "Check the web processes WebKit keeps with the library's cache model (UC_WEBVIEW_CACHE_MODEL, library 0.10.4) "
      "after the last view is freed, and the back/forward cache of a view across two sites (pages on 127.0.0.1 and "
      "127.0.0.2, no network needed)", NULL },
    { "process-wait", 0, 0, G_OPTION_ARG_INT, &opt_process_wait,
      "Seconds --process-test waits for web processes to end after a view is freed (default 10, 1 to 600)", "S" },
    { NULL }
};

#define PROCESS_WAIT_DEFAULT 10
#define WEB_PROCESS_NAME     "WPEWebProcess"

gboolean
process_test_mode (void)
{
    return opt_process_test;
}

gboolean
process_test_check_options (void)
{
    if (!opt_process_test) {
        if (opt_process_wait != -1) {
            g_printerr ("ucwebview-test: --process-wait goes with --process-test\n");
            return FALSE;
        }
        return TRUE;
    }
    if (opt_process_wait == -1) {
        opt_process_wait = PROCESS_WAIT_DEFAULT;
    } else if (opt_process_wait < 1 || opt_process_wait > 600) {
        g_printerr ("ucwebview-test: --process-wait takes 1 to 600 seconds\n");
        return FALSE;
    }
    return TRUE;
}

int
process_test_default_timeout (void)
{
    /* six loads and two waits for processes to end */
    return 60 + 2 * (opt_process_wait > 0 ? opt_process_wait : PROCESS_WAIT_DEFAULT);
}

/* ---- state ------------------------------------------------------------------------------------------------- */

static struct {
    uc_webview *wv;
    void *held;
    char *title;
    gboolean terminated;
    gboolean timed_out;
    gint64 deadline;
    SoupServer *server;
    char *site_a;           /* http://127.0.0.1:P */
    char *site_b;           /* http://127.0.0.2:Q */
    UcCacheModel model;
    int pass, fail;
} st;

static void G_GNUC_PRINTF (3, 4)
check (gboolean ok, const char *name, const char *format, ...)
{
    va_list args;
    char *why;

    va_start (args, format);
    why = g_strdup_vprintf (format, args);
    va_end (args);
    g_print ("process-test: %s %s: %s\n", ok ? "PASS" : "FAIL", name, why);
    g_free (why);
    if (ok)
        st.pass++;
    else
        st.fail++;
}

static void G_GNUC_PRINTF (1, 2)
info (const char *format, ...)
{
    va_list args;
    char *text;

    va_start (args, format);
    text = g_strdup_vprintf (format, args);
    va_end (args);
    g_print ("process-test: INFO %s\n", text);
    g_free (text);
}

/* ---- web processes ------------------------------------------------------------------------------------------- */

typedef struct {
    int pid, ppid;
    gboolean web;           /* comm is WPEWebProcess and it is not a zombie */
} Proc;

/* The fields of /proc/<pid>/stat: "pid (comm) state ppid ..."; comm may hold blanks and parentheses, so it ends at the
 * last ')'. */
static gboolean
read_proc (int pid, Proc *p)
{
    char path[64], buf[512];
    FILE *f;
    size_t n;
    char *open, *close, state;
    int ppid;

    g_snprintf (path, sizeof path, "/proc/%d/stat", pid);
    f = fopen (path, "re");
    if (f == NULL)
        return FALSE;
    n = fread (buf, 1, sizeof buf - 1, f);
    fclose (f);
    buf[n] = '\0';
    open = strchr (buf, '(');
    close = strrchr (buf, ')');
    if (open == NULL || close == NULL || close < open || sscanf (close + 1, " %c %d", &state, &ppid) != 2)
        return FALSE;
    *close = '\0';
    p->pid = pid;
    p->ppid = ppid;
    p->web = strcmp (open + 1, WEB_PROCESS_NAME) == 0 && state != 'Z';
    return TRUE;
}

/* the parent of pid in procs, 0 when pid is not there */
static int
parent_of (GArray *procs, int pid)
{
    for (guint k = 0; k < procs->len; k++) {
        if (g_array_index (procs, Proc, k).pid == pid)
            return g_array_index (procs, Proc, k).ppid;
    }
    return 0;
}

/* The live WPEWebProcess processes below this process (directly or through a launcher such as bwrap); their pids in
 * pids when it is not NULL. */
static guint
count_web_processes (GString *pids)
{
    GArray *procs = g_array_new (FALSE, FALSE, sizeof (Proc));
    DIR *dir = opendir ("/proc");
    struct dirent *entry;
    guint n = 0;
    int self = (int) getpid ();

    if (dir == NULL)
        return 0;
    while ((entry = readdir (dir)) != NULL) {
        Proc p;
        char *end;
        long pid = strtol (entry->d_name, &end, 10);
        if (*end == '\0' && pid > 0 && read_proc ((int) pid, &p))
            g_array_append_val (procs, p);
    }
    closedir (dir);
    for (guint i = 0; i < procs->len; i++) {
        const Proc *p = &g_array_index (procs, Proc, i);
        int ancestor = p->ppid;
        /* up at most 8 levels to this process */
        for (int depth = 0; p->web && ancestor > 1 && ancestor != self && depth < 8; depth++)
            ancestor = parent_of (procs, ancestor);
        if (p->web && ancestor == self) {
            n++;
            if (pids != NULL)
                g_string_append_printf (pids, "%s%d", pids->len > 0 ? " " : "", p->pid);
        }
    }
    g_array_unref (procs);
    return n;
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

/* the title of a page that has shown: "<page>:load" or "<page>:bfcache" */
static gboolean
title_of (gconstpointer page)
{
    const char *prefix = page;
    return st.title != NULL && g_str_has_prefix (st.title, prefix) && st.title[strlen (prefix)] == ':';
}

static gboolean
processes_at_most (gconstpointer n)
{
    return count_web_processes (NULL) <= GPOINTER_TO_UINT (n);
}

static gboolean
never (gconstpointer data)
{
    (void) data;
    return FALSE;
}

/* ---- callbacks --------------------------------------------------------------------------------------------- */

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
    (void) user;
    if (wv != st.wv)
        return;
    g_free (st.title);
    st.title = g_strdup (title);
}

static void
on_terminated (uc_webview *wv, int reason, void *user)
{
    (void) wv; (void) user;
    g_printerr ("process-test: web process terminated (reason %d)\n", reason);
    st.terminated = TRUE;
}

/* ---- server ------------------------------------------------------------------------------------------------ */

/* The page reports how it was shown; no Cache-Control: a page with no-store may stay out of the back/forward cache. */
static void
serve (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data)
{
    char *html;

    (void) server; (void) query; (void) user_data;
    if (g_strcmp0 (path, "/a") != 0 && g_strcmp0 (path, "/b") != 0) {
        soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }
    html = g_strdup_printf ("<!doctype html><title>%s</title><body>page %s<script>"
                            "addEventListener('pageshow', e => { document.title = '%s:' + "
                            "(e.persisted ? 'bfcache' : 'load'); });</script></body>",
                            path + 1, path + 1, path + 1);
    soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response (msg, "text/html; charset=utf-8", SOUP_MEMORY_TAKE, html, strlen (html));
}

/* A listening socket on ip for the server, the port the kernel's choice; 0 on failure. */
static guint16
listen_on (const char *ip)
{
    GError *error = NULL;
    GInetAddress *inet = g_inet_address_new_from_string (ip);
    GSocketAddress *address = g_inet_socket_address_new (inet, 0);
    GSocket *socket = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error);
    GSocketAddress *bound = NULL;
    guint16 port = 0;

    if (socket != NULL && g_socket_bind (socket, address, TRUE, &error) && g_socket_listen (socket, &error)
        && (bound = g_socket_get_local_address (socket, &error)) != NULL
        && soup_server_listen_socket (st.server, socket, 0, &error))
        port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));
    else
        g_printerr ("process-test: cannot listen on %s: %s\n", ip, error != NULL ? error->message : "unknown error");
    g_clear_error (&error);
    g_clear_object (&bound);
    g_clear_object (&socket);
    g_object_unref (address);
    g_object_unref (inet);
    return port;
}

static gboolean
server_start (void)
{
    guint16 a, b;

    st.server = soup_server_new ("server-header", "ucwebview-test", NULL);
    soup_server_add_handler (st.server, NULL, serve, NULL, NULL);
    a = listen_on ("127.0.0.1");
    b = a != 0 ? listen_on ("127.0.0.2") : 0;
    if (b == 0)
        return FALSE;
    st.site_a = g_strdup_printf ("http://127.0.0.1:%u", a);
    st.site_b = g_strdup_printf ("http://127.0.0.2:%u", b);
    return TRUE;
}

/* ---- steps ------------------------------------------------------------------------------------------------- */

static gboolean
view_new (int width, int height, double scale)
{
    uc_webview_callbacks cb;

    memset (&cb, 0, sizeof cb);
    cb.frame_ready = on_frame;
    cb.title_changed = on_title;
    cb.web_process_terminated = on_terminated;
    st.wv = uc_webview_new_sized (width, height, &cb, sizeof cb, NULL);
    if (st.wv == NULL) {
        g_printerr ("process-test: uc_webview_new_sized() failed\n");
        return FALSE;
    }
    uc_webview_set_scale (st.wv, scale);
    return TRUE;
}

static void
view_free (void)
{
    if (st.held != NULL)
        uc_webview_frame_release (st.wv, st.held);
    st.held = NULL;
    uc_webview_free (st.wv);
    st.wv = NULL;
    g_clear_pointer (&st.title, g_free);
}

/* loads site + path and waits for the page's title; returns the title's second part ("load", "bfcache") or NULL */
static const char *
show (const char *site, const char *page)
{
    char *uri = g_strdup_printf ("%s/%s", site, page);

    g_clear_pointer (&st.title, g_free);
    uc_webview_load_uri (st.wv, uri);
    g_free (uri);
    if (!spin_until (title_of, page, 15000))
        return NULL;
    return st.title + strlen (page) + 1;
}

/* goes back or forward and waits for the page's title */
static const char *
travel (gboolean back, const char *page)
{
    g_clear_pointer (&st.title, g_free);
    if (back)
        uc_webview_go_back (st.wv);
    else
        uc_webview_go_forward (st.wv);
    if (!spin_until (title_of, page, 15000))
        return NULL;
    /* a page from the back/forward cache comes with the title it had ("a:load"), its pageshow handler sets the new one
     * a moment later */
    spin_until (never, NULL, 1000);
    return title_of (page) ? st.title + strlen (page) + 1 : NULL;
}

/* the web processes after the view is freed: waits until at most `allowed` are left or --process-wait passed */
static guint
after_free (const char *step, guint allowed, gint64 *waited_ms)
{
    gint64 start = g_get_monotonic_time ();
    GString *pids = g_string_new (NULL);
    guint n;

    view_free ();
    spin_until (processes_at_most, GUINT_TO_POINTER (allowed), (guint) opt_process_wait * 1000);
    /* what WebKit keeps a moment longer */
    spin_until (never, NULL, 500);
    *waited_ms = (g_get_monotonic_time () - start) / 1000;
    n = count_web_processes (pids);
    info ("%s: %u web process(es) %" G_GINT64_FORMAT " ms after uc_webview_free()%s%s", step, n, *waited_ms,
          n > 0 ? ", pids " : "", pids->str);
    g_string_free (pids, TRUE);
    return n;
}

static void
step_close (int width, int height, double scale, gboolean cache_on)
{
    const char *how;
    guint shown, left;
    gint64 waited;

    if (!view_new (width, height, scale))
        return;
    how = show (st.site_a, "a");
    shown = count_web_processes (NULL);
    info ("close: %u web process(es) while page a shows", shown);
    check (how != NULL && shown >= 1, "close-shown", "page a %s, %u web process(es)", how != NULL ? how : "not shown",
           shown);
    left = after_free ("close", cache_on ? 1 : 0, &waited);
    if (cache_on)
        check (left == 1, "close", "%u web process(es) left with web-browser: WebKit's web process cache keeps the "
               "closed page's process (expected 1)", left);
    else if (st.model == UC_CACHE_MODEL_WEB_BROWSER)
        check (left == 0, "close", "%u web process(es) left with web-browser below 2 GB of RAM: no web process cache "
               "(expected 0)", left);
    else
        check (left == 0, "close", "%u web process(es) left with %s within %d s (expected 0)", left,
               uc_cache_model_name (st.model), opt_process_wait);
}

static void
step_back_forward (int width, int height, double scale)
{
    gboolean bfcache = st.model != UC_CACHE_MODEL_VIEWER;
    const char *expect = bfcache ? "bfcache" : "load";
    const char *how;
    gint64 waited;
    guint left;

    if (!view_new (width, height, scale))
        return;
    how = show (st.site_a, "a");
    check (g_strcmp0 (how, "load") == 0, "back-forward-a", "page a on %s: %s", st.site_a, how != NULL ? how : "not shown");
    info ("back-forward: %u web process(es) with page a", count_web_processes (NULL));
    how = show (st.site_b, "b");
    check (g_strcmp0 (how, "load") == 0, "back-forward-b", "page b on %s: %s", st.site_b, how != NULL ? how : "not shown");
    /* the spare process WebKit starts after the main frame's load (WebPageProxy.cpp:9232-9233) */
    spin_until (never, NULL, 1000);
    info ("back-forward: %u web process(es) with page b (b, a spare, and a's suspended page unless viewer)",
          count_web_processes (NULL));
    how = travel (TRUE, "a");
    check (g_strcmp0 (how, expect) == 0, "back", "page a %s (expected %s with %s)", how != NULL ? how : "not shown",
           expect, uc_cache_model_name (st.model));
    how = travel (FALSE, "b");
    check (g_strcmp0 (how, expect) == 0, "forward", "page b %s (expected %s with %s)", how != NULL ? how : "not shown",
           expect, uc_cache_model_name (st.model));
    spin_until (never, NULL, 1000);
    info ("back-forward: %u web process(es) after back and forward", count_web_processes (NULL));

    /* web-browser: the cache keeps a's and b's processes, besides the spare */
    left = after_free ("swap-close", st.model == UC_CACHE_MODEL_WEB_BROWSER ? 2 : 1, &waited);
    if (st.model != UC_CACHE_MODEL_WEB_BROWSER)
        check (left <= 1, "swap-close", "%u web process(es) left with %s (expected at most 1: WebKit's spare process "
               "after a process swap)", left, uc_cache_model_name (st.model));
}

int
process_test_run (int width, int height, double scale, int timeout_s)
{
    struct sysinfo si;
    guint64 ram = 0;
    gboolean cache_on;
    guint ticker, before;
    int code;

    st.deadline = g_get_monotonic_time () + (gint64) timeout_s * G_USEC_PER_SEC;
    st.model = uc_cache_model_parse (g_getenv (UC_CACHE_MODEL_ENV), NULL);
    /* WebKit's ramSize() on Linux (AvailableMemory.cpp:152-155) and its threshold of the web process cache
     * (WebProcessCache.cpp:346-352) */
    if (sysinfo (&si) == 0)
        ram = (guint64) si.totalram * si.mem_unit;
    cache_on = st.model == UC_CACHE_MODEL_WEB_BROWSER && ram >= 2ull * 1024 * 1024 * 1024;
    if (!server_start ())
        return 2;
    g_print ("process-test: cache model %s, RAM %" G_GUINT64_FORMAT " MiB (web process cache %s), pages on %s and %s\n",
             uc_cache_model_name (st.model), ram / (1024 * 1024), cache_on ? "on" : "off", st.site_a, st.site_b);
    ticker = g_timeout_add (20, tick, NULL);
    before = count_web_processes (NULL);
    if (before != 0)
        info ("%u web process(es) before the first view", before);

    step_close (width, height, scale, cache_on);
    if (!st.terminated && !st.timed_out)
        step_back_forward (width, height, scale);
    if (st.wv != NULL)
        view_free ();

    g_source_remove (ticker);
    if (st.timed_out) {
        g_print ("process-test: %d passed, %d failed: TIMEOUT\n", st.pass, st.fail);
        code = 2;
    } else if (st.terminated) {
        g_print ("process-test: %d passed, %d failed: FAILED (web process terminated)\n", st.pass, st.fail);
        code = 1;
    } else {
        g_print ("process-test: %d passed, %d failed: %s\n", st.pass, st.fail, st.fail == 0 ? "ok" : "FAILED");
        code = st.fail == 0 ? 0 : 1;
    }
    soup_server_disconnect (st.server);
    g_clear_object (&st.server);
    g_clear_pointer (&st.site_a, g_free);
    g_clear_pointer (&st.site_b, g_free);
    return code;
}
