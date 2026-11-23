/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --ime-swap-test of ucwebview-test, see ucwebview-test-ime.h.
 *
 * Why it exists: the on-screen keyboard opened at every tap on any page after visiting the web-configurator
 * (http://localhost/, which focuses its PIN field itself) and navigating to another site, and also when the view got
 * the focus back after remote-ui's login dialog, on a page without any input. ucwebview-test 0.7.4 on a Remote 3
 * showed the cause: after a cross-site navigation the library never reported ime_focus(0) for the PIN field. Per
 * variant the steps are:
 *   1. page A on http://127.0.0.1:<port>/a/<variant>: an input with autofocus; expects ime_focus(1) ("autofocus"),
 *      then ime_focus(0) and ime_focus(1) with the same hints and purpose while the view loses and regains the
 *      focus (300 ms each): the element still has the focus, WebKit's replay of it must reach the host
 *      ("valid-replay");
 *   2. the host loads page B, on http://localhost:<port>/b/<variant> (cross-site) or http://127.0.0.1:<port>/b/...
 *      (same-site); 500 ms after its load finished expects an ime_focus(0) since the navigation and no element
 *      reported focused ("focus-out");
 *   3. uc_webview_set_focused(0), 300 ms later (1), like remote-ui around a dialog; expects no ime_focus(1) within
 *      500 ms ("no-replay");
 *   4. page B focuses a field by a script (no user gesture, like A's autofocus); expects ime_focus(1) within 1.5 s,
 *      every one with the field's purpose, and the page's focusin ("new-focus"). In the variant "cross-site" the
 *      field has the state of A's element, so WebKit's state does not change at all (the case 0.7.4 reports nothing
 *      for); in "cross-site-email" it is an e-mail field;
 *   5. the script blurs it; expects ime_focus(0) within 1 s ("blur").
 */
#include "ucwebview-test-ime.h"

#include "uc_webview.h"

#include <gio/gio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- options ----------------------------------------------------------------------------------------------- */

static gboolean opt_ime_swap_test;
static char *opt_ime_swap_variants;

const GOptionEntry ime_swap_test_entries[] = {
    { "ime-swap-test", 0, 0, G_OPTION_ARG_NONE, &opt_ime_swap_test,
      "Let a page focus an input when it loads, navigate to another site (WebKit swaps the web process) or the same "
      "site and check the ime_focus reports: out at the navigation, no replay when the view's focus comes back, a new "
      "focus with its purpose, its blur (pages served by the tool on 127.0.0.1 and localhost)", NULL },
    { "ime-swap-variants", 0, 0, G_OPTION_ARG_STRING, &opt_ime_swap_variants,
      "The --ime-swap-test variants, comma separated (default: all): cross-site, cross-site-email, same-site", "LIST" },
    { NULL }
};

/* ---- plan -------------------------------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *what;
    gboolean cross_site;    /* page B on localhost instead of 127.0.0.1 */
    const char *field;      /* the element of page B the script focuses */
    unsigned purpose;       /* its UC_WEBVIEW_IME_PURPOSE_* */
} Variant;

static const Variant variants[] = {
    { "cross-site", "to another site (a web process swap); page B focuses a text field with the state of A's",
      TRUE, "same", UC_WEBVIEW_IME_PURPOSE_FREE_FORM },
    { "cross-site-email", "to another site (a web process swap); page B focuses an e-mail field",
      TRUE, "mail", UC_WEBVIEW_IME_PURPOSE_EMAIL },
    { "same-site", "control: to the same site (one web process, WebKit reports the blur at the commit itself)",
      FALSE, "same", UC_WEBVIEW_IME_PURPOSE_FREE_FORM },
};

/* page A: an input the page focuses itself when it loads, like the web-configurator's PIN field */
static const char page_a_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>ime-swap A</title>"
    "<style>body{margin:0;background:#fff}input{position:absolute;left:20px;top:20px;width:200px;height:40px}</style>"
    "</head><body><input id=a type=text autocomplete=off autofocus></body></html>";

/* page B: no autofocus; the tool focuses and blurs a field by script, the page reports it in its title */
static const char page_b_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>ime-swap B</title>"
    "<style>body{margin:0;background:#fff}input{position:absolute;left:20px;width:200px;height:40px}</style>"
    "</head><body><input id=same type=text autocomplete=off style='top:20px'>"
    "<input id=mail type=email autocomplete=off style='top:100px'>"
    "<script>document.addEventListener('focusin',function(e){document.title='focus:'+e.target.id;});"
    "document.addEventListener('focusout',function(e){document.title='blur:'+e.target.id;});</script>"
    "</body></html>";

typedef struct {
    int focused;
    unsigned hints, purpose;
} ImeEvent;

typedef enum { WAIT_NONE, WAIT_A, WAIT_B } Wait;

static struct {
    GMainLoop *loop;
    int width, height;
    double scale;
    GSocketService *service;
    guint16 port;
    GArray *plan;           /* guint: indices into variants */
    guint vi;
    const Variant *variant;
    uc_webview *wv;
    void *held;             /* the frame shown */
    GArray *events;         /* ImeEvent: the variant's ime_focus reports */
    guint mark;             /* events before the running step */
    Wait wait;              /* the page whose load the test waits for */
    char *title;
    gboolean a_focused;
    ImeEvent a_event;       /* A's element as the library reported it */
    int pass, fail, vpass, vfail;
    gboolean finished;
    int exit_code;
} st;

gboolean
ime_swap_test_mode (void)
{
    return opt_ime_swap_test;
}

gboolean
ime_swap_test_check_options (void)
{
    char **names;

    if (!opt_ime_swap_test) {
        if (opt_ime_swap_variants != NULL)
            g_printerr ("ucwebview-test: --ime-swap-variants goes with --ime-swap-test\n");
        return opt_ime_swap_variants == NULL;
    }
    if (st.plan != NULL)
        return TRUE;
    st.plan = g_array_new (FALSE, FALSE, sizeof (guint));
    if (opt_ime_swap_variants == NULL) {
        for (guint i = 0; i < G_N_ELEMENTS (variants); i++)
            g_array_append_val (st.plan, i);
        return TRUE;
    }
    names = g_strsplit (opt_ime_swap_variants, ",", -1);
    for (char **n = names; *n != NULL; n++) {
        guint i;
        for (i = 0; i < G_N_ELEMENTS (variants); i++)
            if (g_strcmp0 (g_strstrip (*n), variants[i].name) == 0)
                break;
        if (i == G_N_ELEMENTS (variants)) {
            g_printerr ("ucwebview-test: unknown --ime-swap-variants entry \"%s\"\n", *n);
            g_strfreev (names);
            return FALSE;
        }
        g_array_append_val (st.plan, i);
    }
    g_strfreev (names);
    if (st.plan->len == 0) {
        g_printerr ("ucwebview-test: --ime-swap-variants is empty\n");
        return FALSE;
    }
    return TRUE;
}

int
ime_swap_test_default_timeout (void)
{
    guint n = st.plan != NULL ? st.plan->len : G_N_ELEMENTS (variants);
    /* two loads and 4 s of steps per variant */
    return 20 + (int) n * 15;
}

static void
finish (int code)
{
    if (st.finished)
        return;
    st.finished = TRUE;
    st.exit_code = code;
    g_main_loop_quit (st.loop);
}

static void G_GNUC_PRINTF (3, 4)
check (gboolean ok, const char *name, const char *format, ...)
{
    va_list args;
    char *why;

    va_start (args, format);
    why = g_strdup_vprintf (format, args);
    va_end (args);
    g_print ("ime-swap-test: %s: %s %s: %s\n", st.variant->name, ok ? "PASS" : "FAIL", name, why);
    g_free (why);
    if (ok) {
        st.pass++;
        st.vpass++;
    } else {
        st.fail++;
        st.vfail++;
    }
}

/* ---- the server ---------------------------------------------------------------------------------------------- */

/* a thread of the service: one request per connection ("Connection: close") */
static gboolean
server_run (GThreadedSocketService *service, GSocketConnection *connection, GObject *source, gpointer user_data)
{
    GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    char request[4096];
    gsize length = 0;
    const char *body = NULL, *status = "404 Not Found";
    char *header;

    (void) service; (void) source; (void) user_data;
    g_socket_set_timeout (g_socket_connection_get_socket (connection), 5);
    while (length < sizeof request - 1) {
        gssize n = g_input_stream_read (in, request + length, sizeof request - 1 - length, NULL, NULL);
        if (n <= 0)
            break;
        length += (gsize) n;
        request[length] = '\0';
        if (strstr (request, "\r\n\r\n") != NULL)
            break;
    }
    request[length] = '\0';
    if (g_str_has_prefix (request, "GET /a/"))
        body = page_a_html;
    else if (g_str_has_prefix (request, "GET /b/"))
        body = page_b_html;
    if (body != NULL)
        status = "200 OK";
    else
        body = "<!doctype html><title>not found</title>";
    header = g_strdup_printf ("HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %"
                              G_GSIZE_FORMAT "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                              status, strlen (body));
    if (g_output_stream_write_all (out, header, strlen (header), NULL, NULL, NULL))
        g_output_stream_write_all (out, body, strlen (body), NULL, NULL, NULL);
    g_free (header);
    g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
    return TRUE;
}

static gboolean
listen_on (GSocketFamily family, guint16 port, guint16 *effective_port, GError **error)
{
    GInetAddress *loopback = g_inet_address_new_loopback (family);
    GSocketAddress *address = g_inet_socket_address_new (loopback, port);
    GSocketAddress *effective = NULL;
    gboolean ok = g_socket_listener_add_address (G_SOCKET_LISTENER (st.service), address, G_SOCKET_TYPE_STREAM,
                                                 G_SOCKET_PROTOCOL_TCP, NULL, &effective, error);
    g_object_unref (address);
    g_object_unref (loopback);
    if (ok && effective_port != NULL)
        *effective_port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
    g_clear_object (&effective);
    return ok;
}

static gboolean
server_start (void)
{
    GError *error = NULL;

    st.service = g_threaded_socket_service_new (4);
    if (!listen_on (G_SOCKET_FAMILY_IPV4, 0, &st.port, &error)) {
        g_printerr ("ime-swap-test: cannot listen on 127.0.0.1: %s\n", error->message);
        g_clear_error (&error);
        return FALSE;
    }
    /* localhost may resolve to ::1 first; without IPv6 the IPv4 address answers */
    if (!listen_on (G_SOCKET_FAMILY_IPV6, st.port, NULL, &error))
        g_clear_error (&error);
    g_signal_connect (st.service, "run", G_CALLBACK (server_run), NULL);
    g_socket_service_start (st.service);
    return TRUE;
}

/* ---- callbacks ----------------------------------------------------------------------------------------------- */

static void
on_frame (uc_webview *wv, const uc_webview_frame *frame, void *user)
{
    (void) user;
    /* shown at once, the previous one released, like a scene graph; nothing is read back */
    uc_webview_frame_rendered (wv, frame->handle);
    if (st.held != NULL)
        uc_webview_frame_release (wv, st.held);
    st.held = frame->handle;
}

static void
on_terminated (uc_webview *wv, int reason, void *user)
{
    (void) wv; (void) user;
    g_printerr ("ime-swap-test: web process terminated (reason %d)\n", reason);
    st.fail++;
    finish (1);
}

static void
on_title (uc_webview *wv, const char *title, void *user)
{
    (void) wv; (void) user;
    g_free (st.title);
    st.title = g_strdup (title);
}

static void
on_ime_focus (uc_webview *wv, int focused, unsigned hints, unsigned purpose, const char *surrounding, int cursor,
              void *user)
{
    ImeEvent event = { focused, hints, purpose };
    (void) wv; (void) surrounding; (void) cursor; (void) user;
    g_print ("ime-swap-test: %s: ime focus=%d hints=0x%x purpose=%u\n", st.variant->name, focused, hints, purpose);
    g_array_append_val (st.events, event);
}

static void step_a_loaded (void);
static void step_b_loaded (void);

static void
on_load (uc_webview *wv, int event, const char *uri, void *user)
{
    (void) wv; (void) user;
    if (event == UC_WEBVIEW_LOAD_COMMITTED)
        g_print ("ime-swap-test: %s: committed %s\n", st.variant->name, uri != NULL ? uri : "(null)");
    if (event == UC_WEBVIEW_LOAD_FAILED) {
        g_printerr ("ime-swap-test: %s: load failed: %s\n", st.variant->name, uri != NULL ? uri : "(null)");
        st.fail++;
        finish (1);
        return;
    }
    if (event != UC_WEBVIEW_LOAD_FINISHED || st.finished)
        return;
    if (st.wait == WAIT_A) {
        st.wait = WAIT_NONE;
        step_a_loaded ();
    } else if (st.wait == WAIT_B) {
        st.wait = WAIT_NONE;
        step_b_loaded ();
    }
}

/* ---- steps -------------------------------------------------------------------------------------------------- */

static void start_variant (void);

static const ImeEvent *
event_at (guint i)
{
    return &g_array_index (st.events, ImeEvent, i);
}

/* the reports since the step began: how many were focus in / out, and the last one (NULL: none) */
static const ImeEvent *
events_since_mark (int *n_in, int *n_out)
{
    *n_in = *n_out = 0;
    for (guint i = st.mark; i < st.events->len; i++) {
        if (event_at (i)->focused)
            (*n_in)++;
        else
            (*n_out)++;
    }
    return st.events->len > st.mark ? event_at (st.events->len - 1) : NULL;
}

static void
mark (void)
{
    st.mark = st.events->len;
}

static char *
page_uri (gboolean localhost, const char *page)
{
    return g_strdup_printf ("http://%s:%u/%s/%s", localhost ? "localhost" : "127.0.0.1", st.port, page,
                            st.variant->name);
}

static gboolean
next_variant (gpointer user_data)
{
    (void) user_data;
    start_variant ();
    return G_SOURCE_REMOVE;
}

static void
end_variant (void)
{
    g_print ("ime-swap-test: %s: %d passed, %d failed\n", st.variant->name, st.vpass, st.vfail);
    if (st.held != NULL)
        uc_webview_frame_release (st.wv, st.held);
    st.held = NULL;
    uc_webview_free (st.wv);
    st.wv = NULL;
    st.vi++;
    g_idle_add (next_variant, NULL);
}

static gboolean
step_blurred (gpointer user_data)
{
    int n_in, n_out;
    const ImeEvent *last = events_since_mark (&n_in, &n_out);
    (void) user_data;
    if (st.finished)
        return G_SOURCE_REMOVE;
    check (n_out > 0 && last != NULL && !last->focused, "blur",
           "%d ime_focus(0), %d ime_focus(1) after the script's blur(), the page's title \"%s\"", n_out, n_in,
           st.title != NULL ? st.title : "");
    end_variant ();
    return G_SOURCE_REMOVE;
}

static gboolean
step_focused (gpointer user_data)
{
    int n_in, n_out, wrong = 0;
    const ImeEvent *last = events_since_mark (&n_in, &n_out);
    char *expected_title = g_strdup_printf ("focus:%s", st.variant->field);
    gboolean page_focused = g_strcmp0 (st.title, expected_title) == 0;
    char *script;

    (void) user_data;
    if (st.finished) {
        g_free (expected_title);
        return G_SOURCE_REMOVE;
    }
    for (guint i = st.mark; i < st.events->len; i++)
        if (event_at (i)->focused && event_at (i)->purpose != st.variant->purpose)
            wrong++;
    if (last != NULL && last->focused && st.a_focused)
        g_print ("ime-swap-test: %s: the field's state %s A's element (hints 0x%x purpose %u)\n", st.variant->name,
                 last->hints == st.a_event.hints && last->purpose == st.a_event.purpose ? "equals" : "differs from",
                 st.a_event.hints, st.a_event.purpose);
    check (page_focused && n_in > 0 && wrong == 0 && last->focused, "new-focus",
           "the page's title \"%s\"; %d ime_focus(1) (%d with another purpose than %u), %d ime_focus(0) after the "
           "script's focus() of #%s", st.title != NULL ? st.title : "", n_in, wrong, st.variant->purpose, n_out,
           st.variant->field);
    g_free (expected_title);

    mark ();
    script = g_strdup_printf ("document.getElementById('%s').blur();", st.variant->field);
    uc_webview_run_javascript (st.wv, script);
    g_free (script);
    g_timeout_add (1000, step_blurred, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
step_refocused (gpointer user_data)
{
    int n_in, n_out;
    char *script;
    (void) user_data;
    if (st.finished)
        return G_SOURCE_REMOVE;
    events_since_mark (&n_in, &n_out);
    check (n_in == 0, "no-replay", "%d ime_focus(1), %d ime_focus(0) while the view lost and regained the focus", n_in,
           n_out);

    mark ();
    script = g_strdup_printf ("document.getElementById('%s').focus();", st.variant->field);
    uc_webview_run_javascript (st.wv, script);
    g_free (script);
    g_timeout_add (1500, step_focused, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
step_refocus (gpointer user_data)
{
    (void) user_data;
    if (st.finished)
        return G_SOURCE_REMOVE;
    uc_webview_set_focused (st.wv, 1);
    g_timeout_add (500, step_refocused, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
step_b_settled (gpointer user_data)
{
    int n_in, n_out;
    const ImeEvent *last = events_since_mark (&n_in, &n_out);
    (void) user_data;
    if (st.finished)
        return G_SOURCE_REMOVE;
    check (n_out > 0 && last != NULL && !last->focused, "focus-out",
           "%d ime_focus(0), %d ime_focus(1) from the navigation to 500 ms after page B loaded%s", n_out, n_in,
           n_out == 0 && st.a_focused ? ": the host still believes A's element has the focus" : "");

    /* like remote-ui around its dialogs: the item loses the active focus and gets it back */
    mark ();
    uc_webview_set_focused (st.wv, 0);
    g_timeout_add (300, step_refocus, NULL);
    return G_SOURCE_REMOVE;
}

static void
step_b_loaded (void)
{
    g_timeout_add (500, step_b_settled, NULL);
}

static gboolean
step_a_replayed (gpointer user_data)
{
    int n_in, n_out;
    const ImeEvent *last = events_since_mark (&n_in, &n_out);
    char *uri;

    (void) user_data;
    if (st.finished)
        return G_SOURCE_REMOVE;
    check (n_out > 0 && last != NULL && last->focused && last->hints == st.a_event.hints
           && last->purpose == st.a_event.purpose, "valid-replay",
           "%d ime_focus(0), %d ime_focus(1) for page A's input while the view lost and regained the focus", n_out,
           n_in);

    mark ();
    st.wait = WAIT_B;
    uri = page_uri (st.variant->cross_site, "b");
    uc_webview_load_uri (st.wv, uri);
    g_free (uri);
    return G_SOURCE_REMOVE;
}

static gboolean
step_a_refocus (gpointer user_data)
{
    (void) user_data;
    if (st.finished)
        return G_SOURCE_REMOVE;
    uc_webview_set_focused (st.wv, 1);
    g_timeout_add (300, step_a_replayed, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
step_a_settled (gpointer user_data)
{
    (void) user_data;
    if (st.finished)
        return G_SOURCE_REMOVE;
    for (guint i = 0; i < st.events->len; i++)
        if (event_at (i)->focused) {
            st.a_focused = TRUE;
            st.a_event = *event_at (i);
        }
    if (st.a_focused)
        check (TRUE, "autofocus", "ime_focus(1) hints 0x%x purpose %u for page A's input", st.a_event.hints,
               st.a_event.purpose);
    else
        check (FALSE, "autofocus", "no ime_focus(1) for page A's input within 700 ms after its load");

    /* the element keeps its focus while the view loses and regains it: WebKit replays it, the host must hear it */
    mark ();
    uc_webview_set_focused (st.wv, 0);
    g_timeout_add (300, step_a_refocus, NULL);
    return G_SOURCE_REMOVE;
}

static void
step_a_loaded (void)
{
    g_timeout_add (700, step_a_settled, NULL);
}

static void
start_variant (void)
{
    uc_webview_callbacks cb;
    char *uri;

    if (st.finished)
        return;
    if (st.vi >= st.plan->len) {
        g_print ("ime-swap-test: %d passed, %d failed: %s\n", st.pass, st.fail, st.fail == 0 ? "ok" : "FAILED");
        finish (st.fail == 0 ? 0 : 1);
        return;
    }
    st.variant = &variants[g_array_index (st.plan, guint, st.vi)];
    st.vpass = st.vfail = 0;
    st.a_focused = FALSE;
    memset (&st.a_event, 0, sizeof st.a_event);
    g_array_set_size (st.events, 0);
    st.mark = 0;
    g_clear_pointer (&st.title, g_free);
    g_print ("ime-swap-test: variant %s: %s\n", st.variant->name, st.variant->what);

    memset (&cb, 0, sizeof cb);
    cb.frame_ready = on_frame;
    cb.load_changed = on_load;
    cb.title_changed = on_title;
    cb.web_process_terminated = on_terminated;
    cb.ime_focus = on_ime_focus;
    st.wv = uc_webview_new_sized (st.width, st.height, &cb, sizeof cb, NULL);
    if (st.wv == NULL) {
        g_printerr ("ime-swap-test: uc_webview_new_sized failed\n");
        finish (2);
        return;
    }
    /* what remote-ui does for a new view (webviewitem.cpp attachWindow) */
    uc_webview_set_scale (st.wv, st.scale);
    uc_webview_set_mapped (st.wv, 1);
    uc_webview_set_focused (st.wv, 1);
    uc_webview_set_background_rgba (st.wv, 1, 1, 1, 1);

    st.wait = WAIT_A;
    uri = page_uri (FALSE, "a");
    uc_webview_load_uri (st.wv, uri);
    g_free (uri);
}

static guint run_timeout_source;

static gboolean
run_timeout (gpointer user_data)
{
    int timeout_s = GPOINTER_TO_INT (user_data);
    run_timeout_source = 0;
    g_printerr ("ime-swap-test: timeout after %d s (%d passed, %d failed so far)\n", timeout_s, st.pass, st.fail);
    finish (2);
    return G_SOURCE_REMOVE;
}

int
ime_swap_test_run (int width, int height, double scale, int timeout_s)
{
    st.width = width;
    st.height = height;
    st.scale = scale;
    if (!ime_swap_test_check_options ())
        return 3;
    if (!server_start ())
        return 2;
    st.loop = g_main_loop_new (NULL, FALSE);
    st.events = g_array_new (FALSE, FALSE, sizeof (ImeEvent));
    g_print ("ime-swap-test: pages on http://127.0.0.1:%u/ and http://localhost:%u/, %u variant(s)\n", st.port,
             st.port, st.plan->len);
    run_timeout_source = g_timeout_add_seconds ((guint) timeout_s, run_timeout, GINT_TO_POINTER (timeout_s));
    g_idle_add (next_variant, NULL);
    g_main_loop_run (st.loop);

    if (run_timeout_source != 0)
        g_source_remove (run_timeout_source);
    run_timeout_source = 0;
    if (st.wv != NULL) {
        if (st.held != NULL)
            uc_webview_frame_release (st.wv, st.held);
        st.held = NULL;
        uc_webview_free (st.wv);
        st.wv = NULL;
    }
    g_socket_service_stop (st.service);
    g_socket_listener_close (G_SOCKET_LISTENER (st.service));
    g_clear_object (&st.service);
    g_clear_pointer (&st.title, g_free);
    g_array_unref (st.events);
    g_array_unref (st.plan);
    st.plan = NULL;
    g_main_loop_unref (st.loop);
    return st.exit_code;
}
