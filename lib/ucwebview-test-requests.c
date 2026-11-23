/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The request modes of ucwebview-test (library 0.7.0), see ucwebview-test-requests.h.
 *
 * A mode is a list of steps. A step does one thing like a host or a user would (load a page, run a script in the page
 * with a user gesture, reload, go back, kill the web process, tap the page), answers the requests it gets in a given
 * way, and records every request and load callback in a transcript ("dialog:alert", "auth:basic:retry", "cancelled",
 * "new-window:loaded:<uri>", "refused:download:<uri>:<type>", "load:committed:<uri>", "terminated"). When the
 * transcript has the expected length and the page's title is the expected one, the step waits a little longer for
 * events that should not come, then compares the transcript with its patterns (g_pattern_match_simple(), a leading
 * '?' marks an event that may be missing). Every answer is checked as well: a wrong kind of answer, an unknown id and
 * a second answer must return -1, request_cancelled() must name a pending request, and no request callback may come
 * from inside uc_webview_free().
 *
 * Output follows the library's logging rules: dialog texts, prompt answers and credentials are compared, not printed.
 */
#include "ucwebview-test-requests.h"

#include "uc_uri_policy.h"

#include <dirent.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- options ----------------------------------------------------------------------------------------------- */

static gboolean opt_dialog_test;
static gboolean opt_auth_test;
static gboolean opt_window_test;
static gboolean opt_download_test;
static char *opt_dialog_answer;     /* accept | reject | text:S | never */
static char *opt_auth_answer;       /* login | cancel | no | never */
static char *opt_auth_scheme;       /* basic | digest */
static char *opt_credentials;       /* USER:PASSWORD for login answers on other pages */
static gboolean opt_reply_in_callback;

const GOptionEntry request_test_entries[] = {
    { "dialog-test", 0, 0, G_OPTION_ARG_NONE, &opt_dialog_test,
      "Library 0.7.0: alert, confirm, prompt, the dialog loop guard (0.7.2: its end at a tap), beforeunload on reload "
      "and back, a dialog pending at uc_webview_free(); with --dialog-answer never: cancelled by a host navigation and "
      "by a web process crash",
      NULL },
    { "auth-test", 0, 0, G_OPTION_ARG_NONE, &opt_auth_test,
      "Library 0.7.0: HTTP authentication against the tool's own server (--auth-scheme, --auth-answer)", NULL },
    { "window-test", 0, 0, G_OPTION_ARG_NONE, &opt_window_test,
      "Library 0.7.0: window.close(), target=_blank, window.open() of http and data: URLs, a form posted to a new window",
      NULL },
    { "download-test", 0, 0, G_OPTION_ARG_NONE, &opt_download_test,
      "Library 0.7.0: an attachment, <a download>, an unknown type and a PDF, in the page and in frames", NULL },
    /* FILENAME: the bytes as given (STRING converts from the locale's charset, which fails for UTF-8 in the C locale);
     * request_test_check_options() wants UTF-8 */
    { "dialog-answer", 0, 0, G_OPTION_ARG_FILENAME, &opt_dialog_answer,
      "How the tool answers script dialogs: accept, reject, text:S (accept, the prompt gets S), never "
      "(default: accept in --dialog-test, reject otherwise)", "ANSWER" },
    { "auth-answer", 0, 0, G_OPTION_ARG_STRING, &opt_auth_answer,
      "How the tool answers logins: login (in --auth-test a wrong password first), cancel "
      "(uc_webview_request_auth(NULL)), no (uc_webview_request_reply(0)), never (default: login in --auth-test, "
      "cancel otherwise)", "ANSWER" },
    { "auth-scheme", 0, 0, G_OPTION_ARG_STRING, &opt_auth_scheme,
      "The scheme of the tool's server in --auth-test: basic (default) or digest", "SCHEME" },
    { "credentials", 0, 0, G_OPTION_ARG_FILENAME, &opt_credentials,
      "USER:PASSWORD for --auth-answer login outside --auth-test (never printed)", "USER:PASSWORD" },
    { "reply-in-callback", 0, 0, G_OPTION_ARG_NONE, &opt_reply_in_callback,
      "Answer requests from inside their callback instead of 50 ms later", NULL },
    { NULL }
};

typedef enum { MODE_NONE, MODE_DIALOG, MODE_AUTH, MODE_WINDOW, MODE_DOWNLOAD } Mode;

static Mode
mode (void)
{
    if (opt_dialog_test)
        return MODE_DIALOG;
    if (opt_auth_test)
        return MODE_AUTH;
    if (opt_window_test)
        return MODE_WINDOW;
    if (opt_download_test)
        return MODE_DOWNLOAD;
    return MODE_NONE;
}

static const char *
mode_name (void)
{
    static const char *const names[] = { "requests", "dialog-test", "auth-test", "window-test", "download-test" };
    return names[mode ()];
}

gboolean
request_test_mode (void)
{
    return mode () != MODE_NONE;
}

int
request_test_default_timeout (void)
{
    return 180;
}

/* answers: one letter per request of a step; the last letter repeats */
#define A_ACCEPT  'a'   /* script dialog: yes */
#define A_REJECT  'r'   /* script dialog: no */
#define A_TEXT    't'   /* script dialog: yes, the prompt gets the --dialog-answer text */
#define A_NEVER   'n'   /* no answer */
#define A_WRONG   'w'   /* login: the right user, a wrong password */
#define A_LOGIN   'l'   /* login: the right user and password */
#define A_CANCEL  'c'   /* login: uc_webview_request_auth(id, NULL, NULL) */
#define A_NO      'o'   /* login: uc_webview_request_reply(id, 0, NULL) */

/* outside the request modes: the tool's quit, for a refused document before any page committed */
static RequestTestQuit tool_quit;
static gboolean any_commit;

static char dialog_answer = A_ACCEPT;
static const char *dialog_text;     /* the text of text:S */
static char auth_answer = A_LOGIN;

gboolean
request_test_check_options (void)
{
    int modes = (opt_dialog_test ? 1 : 0) + (opt_auth_test ? 1 : 0) + (opt_window_test ? 1 : 0)
                + (opt_download_test ? 1 : 0);

    if (modes > 1) {
        g_printerr ("ucwebview-test: --dialog-test, --auth-test, --window-test and --download-test are separate tests\n");
        return FALSE;
    }
    dialog_answer = mode () == MODE_DIALOG ? A_ACCEPT : A_REJECT;
    if (opt_dialog_answer != NULL) {
        if (g_str_equal (opt_dialog_answer, "accept"))
            dialog_answer = A_ACCEPT;
        else if (g_str_equal (opt_dialog_answer, "reject"))
            dialog_answer = A_REJECT;
        else if (g_str_equal (opt_dialog_answer, "never"))
            dialog_answer = A_NEVER;
        else if (g_str_has_prefix (opt_dialog_answer, "text:") && g_utf8_validate (opt_dialog_answer, -1, NULL)
                 && strpbrk (opt_dialog_answer + 5, "\"\\") == NULL) {
            dialog_answer = A_TEXT;
            dialog_text = opt_dialog_answer + 5;
        } else {
            g_printerr ("ucwebview-test: --dialog-answer is accept, reject, never or text:S (UTF-8, without \" and \\)\n");
            return FALSE;
        }
    }
    auth_answer = mode () == MODE_AUTH ? A_LOGIN : A_CANCEL;
    if (opt_auth_answer != NULL) {
        if (g_str_equal (opt_auth_answer, "login"))
            auth_answer = A_LOGIN;
        else if (g_str_equal (opt_auth_answer, "cancel"))
            auth_answer = A_CANCEL;
        else if (g_str_equal (opt_auth_answer, "no"))
            auth_answer = A_NO;
        else if (g_str_equal (opt_auth_answer, "never"))
            auth_answer = A_NEVER;
        else {
            g_printerr ("ucwebview-test: --auth-answer is login, cancel, no or never\n");
            return FALSE;
        }
    }
    if (opt_auth_scheme != NULL && !g_str_equal (opt_auth_scheme, "basic") && !g_str_equal (opt_auth_scheme, "digest")) {
        g_printerr ("ucwebview-test: --auth-scheme is basic or digest\n");
        return FALSE;
    }
    if (opt_credentials != NULL && strchr (opt_credentials, ':') == NULL) {
        g_printerr ("ucwebview-test: --credentials is USER:PASSWORD\n");
        return FALSE;
    }
    return TRUE;
}

/* ---- the server ---------------------------------------------------------------------------------------------- */

/* The test strings below are markers: none of them may appear in a line of the library (package README, device
 * checks: grep 'ucwebview: ' | grep -c -e uc-alert ...). */
#define REALM          "uc-realm"
#define TEST_USER      "ucuser"
#define TEST_PASSWORD  "ucsecret"
#define WRONG_PASSWORD "ucwrong"

#define COMMON_JS \
    "function q(s){return s===null?'null':JSON.stringify(s);}" \
    "function later(f){setTimeout(f,0);}" \
    "function dialogs(){var r=['alert'];alert('uc-alert');r.push('confirm='+confirm('uc-confirm'));" \
    "r.push('prompt='+q(prompt('uc-prompt','uc-default')));document.title='dialogs '+r.join(' ');}" \
    "function loop(n){var c=0;for(var i=0;i<n;i++){alert('uc-loop');c++;}document.title='loop '+c;}" \
    "function arm(){addEventListener('beforeunload',function(e){e.preventDefault();e.returnValue='uc-leave';" \
    "return 'uc-leave';});document.title='armed';}" \
    "function pending(m){alert(m);document.title='answered';}" \
    "function openWin(u){var w=window.open(u);document.title='open '+(w===null?'null':'window');}" \
    "function click(id){document.getElementById(id).click();}" \
    "function post(){document.getElementById('post').submit();}" \
    "function where(){document.title='at '+location.pathname;}"

#define PAGE(title, body) \
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width'>" \
    "<style>body{font:20px sans-serif;margin:16px;background:#fff}a{display:block;margin:12px 0}</style>" \
    "<script>" COMMON_JS "</script><title>" title "</title></head><body><h1>" title "</h1>" \
    "<a id=blank href='/page2' target=_blank>page2 in a new window</a>" \
    "<form id=post method=post action='/page2' target=_blank><input name=x value=1></form>" body "</body></html>"

static const struct {
    const char *path;
    const char *html;
} pages[] = {
    { "/dialog", PAGE ("dialog-page", "") },
    { "/page2", PAGE ("page2", "") },
    { "/page3", PAGE ("page3", "") },
    { "/window", PAGE ("window-page", "") },
    { "/download", PAGE ("download-page",
                         "<a id=attachment href='/attachment.txt'>attachment</a>"
                         "<a id=dl href='/file.bin' download>download attribute</a>"
                         "<a id=unknown href='/unknown'>unknown type</a>"
                         "<a id=pdf href='/doc.pdf'>PDF</a>"
                         "<iframe src='/attachment.txt?frame' width=100 height=40></iframe>"
                         "<iframe src='/doc.pdf?frame' width=100 height=40></iframe>") },
    { "/basic", PAGE ("auth-ok", "") },
    { "/digest", PAGE ("auth-ok", "") },
};

static const char page_401[] = PAGE ("auth-401", "<p>the server's 401 page</p>");

static SoupServer *server;
static char *base;      /* http://127.0.0.1:<port> */

static void
serve (SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers (msg);
    static const char text[] = "uc attachment\n";
    static const char binary[] = "0123456789abcdef";
    static const char pdf[] = "%PDF-1.4\n1 0 obj<<>>endobj\ntrailer<<>>\n%%EOF\n";

    (void) srv; (void) query; (void) user_data;
    soup_message_headers_replace (headers, "Cache-Control", "no-store");
    for (gsize i = 0; i < G_N_ELEMENTS (pages); i++)
        if (g_str_equal (path, pages[i].path)) {
            soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
            soup_server_message_set_response (msg, "text/html; charset=utf-8", SOUP_MEMORY_STATIC, pages[i].html,
                                              strlen (pages[i].html));
            return;
        }
    if (g_str_equal (path, "/attachment.txt")) {
        soup_message_headers_replace (headers, "Content-Disposition", "attachment; filename=\"uc.txt\"");
        soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response (msg, "text/plain", SOUP_MEMORY_STATIC, text, strlen (text));
    } else if (g_str_equal (path, "/file.bin")) {
        soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response (msg, "application/octet-stream", SOUP_MEMORY_STATIC, binary, strlen (binary));
    } else if (g_str_equal (path, "/unknown")) {
        soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response (msg, "application/x-ucwebview-unknown", SOUP_MEMORY_STATIC, binary,
                                          strlen (binary));
    } else if (g_str_equal (path, "/doc.pdf")) {
        soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
        soup_server_message_set_response (msg, "application/pdf", SOUP_MEMORY_STATIC, pdf, strlen (pdf));
    } else {
        soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
    }
}

/* An auth domain answers a request without (or with wrong) credentials with 401 before any handler runs
 * (soup-server.c got_headers()); a body of its own makes the page the browser shows recognisable. */
static void
on_request_read (SoupServer *srv, SoupServerMessage *msg, gpointer user_data)
{
    (void) srv; (void) user_data;
    if (soup_server_message_get_status (msg) == SOUP_STATUS_UNAUTHORIZED)
        soup_server_message_set_response (msg, "text/html; charset=utf-8", SOUP_MEMORY_STATIC, page_401,
                                          strlen (page_401));
}

static gboolean
basic_auth (SoupAuthDomain *domain, SoupServerMessage *msg, const char *user, const char *password, gpointer data)
{
    (void) domain; (void) msg; (void) data;
    return g_strcmp0 (user, TEST_USER) == 0 && g_strcmp0 (password, TEST_PASSWORD) == 0;
}

static char *
digest_auth (SoupAuthDomain *domain, SoupServerMessage *msg, const char *user, gpointer data)
{
    (void) domain; (void) msg; (void) data;
    return g_strcmp0 (user, TEST_USER) == 0 ? soup_auth_domain_digest_encode_password (user, REALM, TEST_PASSWORD)
                                            : NULL;
}

static gboolean
server_start (void)
{
    GError *error = NULL;
    SoupAuthDomain *basic, *digest;
    GSList *uris;

    server = soup_server_new ("server-header", "ucwebview-test", NULL);
    soup_server_add_handler (server, NULL, serve, NULL, NULL);
    g_signal_connect (server, "request-read", G_CALLBACK (on_request_read), NULL);
    basic = soup_auth_domain_basic_new ("realm", REALM, NULL);
    soup_auth_domain_basic_set_auth_callback (basic, basic_auth, NULL, NULL);
    soup_auth_domain_add_path (basic, "/basic");
    soup_server_add_auth_domain (server, basic);
    g_object_unref (basic);
    digest = soup_auth_domain_digest_new ("realm", REALM, NULL);
    soup_auth_domain_digest_set_auth_callback (digest, digest_auth, NULL, NULL);
    soup_auth_domain_add_path (digest, "/digest");
    soup_server_add_auth_domain (server, digest);
    g_object_unref (digest);
    if (!soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
        g_printerr ("%s: cannot start the HTTP server on 127.0.0.1: %s\n", mode_name (), error->message);
        g_clear_error (&error);
        return FALSE;
    }
    uris = soup_server_get_uris (server);
    if (uris == NULL)
        return FALSE;
    base = g_strdup_printf ("http://127.0.0.1:%d", g_uri_get_port (uris->data));
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    g_print ("%s: server at %s\n", mode_name (), base);
    return TRUE;
}

/* ---- steps ------------------------------------------------------------------------------------------------- */

/* ACT_TAP: a still tap like remote-ui's (touch down, up TAP_UP_MS later) at TAP_X,TAP_Y, the page's top left corner
 * outside the body's 16 px margin: a click on the root element, nothing to follow or focus */
typedef enum { ACT_LOAD, ACT_JS, ACT_RELOAD, ACT_BACK, ACT_KILL, ACT_TAP } Action;

#define TAP_X       8.0
#define TAP_Y       8.0
#define TAP_UP_MS   80

typedef struct {
    char *name;
    Action action;
    char *arg;              /* path for ACT_LOAD, script for ACT_JS */
    char *title;            /* the page's title the step waits for, NULL: none */
    char *answers;          /* one letter per request, the last repeats; NULL: none expected */
    GPtrArray *expect;      /* patterns of the transcript */
    guint settle_ms;        /* quiet time after the expectations are met */
    guint delay_ms;         /* before the action */
} Step;

typedef struct {
    guint64 id;
    gboolean auth;
    int type;
    gboolean answered;
} Pending;

static struct {
    uc_webview *wv;
    gboolean plain;
    RequestTestQuit quit;
    GPtrArray *steps;
    guint index;
    Step *step;
    GPtrArray *transcript;
    guint answer_index;
    gboolean title_seen;
    guint settle_source, timeout_source, action_source;
    GArray *pending;        /* Pending */
    int failures;
    gboolean freeing;
    gboolean done;
    int callbacks_in_free;
    guint64 pending_at_free;
    int requests;           /* request callbacks in total */
    gboolean wrong_dialog_call_done, wrong_auth_call_done;
} T;

static void
step_free (gpointer data)
{
    Step *s = data;
    g_free (s->name);
    g_free (s->arg);
    g_free (s->title);
    g_free (s->answers);
    g_ptr_array_unref (s->expect);
    g_free (s);
}

/* a step: name, action, arg, title, answers, settle; then the transcript patterns up to the first NULL (a mode builds
 * the plain variant of a step by putting NULL where its list ends) */
static Step *
add_step (const char *name, Action action, const char *arg, const char *title, const char *answers, guint settle_ms,
          ...)
{
    Step *s = g_new0 (Step, 1);
    va_list ap;
    const char *pattern;

    s->name = g_strdup (name);
    s->action = action;
    s->arg = g_strdup (arg);
    s->title = g_strdup (title);
    s->answers = g_strdup (answers);
    s->settle_ms = settle_ms;
    s->expect = g_ptr_array_new_with_free_func (g_free);
    va_start (ap, settle_ms);
    while ((pattern = va_arg (ap, const char *)) != NULL)
        g_ptr_array_add (s->expect, g_strdup (pattern));
    va_end (ap);
    g_ptr_array_add (T.steps, s);
    return s;
}

static void
fail (const char *format, ...) G_GNUC_PRINTF (1, 2);

static void
fail (const char *format, ...)
{
    va_list ap;
    char *text;

    va_start (ap, format);
    text = g_strdup_vprintf (format, ap);
    va_end (ap);
    g_print ("%s: CHECK FAILED%s%s: %s\n", mode_name (), T.step != NULL ? " in step " : "",
             T.step != NULL ? T.step->name : "", text);
    g_free (text);
    T.failures++;
}

static void step_begin (guint index);

static void
finish (void)
{
    if (T.done)
        return;
    T.done = TRUE;
    if (T.settle_source != 0)
        g_source_remove (T.settle_source);
    if (T.timeout_source != 0)
        g_source_remove (T.timeout_source);
    if (T.action_source != 0)
        g_source_remove (T.action_source);
    T.settle_source = T.timeout_source = T.action_source = 0;
    T.quit (T.failures == 0 ? 0 : 1);
}

/* sequential match; "?pattern" may be missing */
static gboolean
transcript_matches (const Step *s)
{
    guint t = 0;

    for (guint p = 0; p < s->expect->len; p++) {
        const char *pattern = g_ptr_array_index (s->expect, p);
        gboolean optional = pattern[0] == '?';
        if (optional)
            pattern++;
        if (t < T.transcript->len && g_pattern_match_simple (pattern, g_ptr_array_index (T.transcript, t)))
            t++;
        else if (!optional)
            return FALSE;
    }
    return t == T.transcript->len;
}

static guint
required_events (const Step *s)
{
    guint n = 0;
    for (guint p = 0; p < s->expect->len; p++)
        if (((const char *) g_ptr_array_index (s->expect, p))[0] != '?')
            n++;
    return n;
}

static gboolean
step_evaluate (gpointer user_data)
{
    Step *s = T.step;
    gboolean timeout = GPOINTER_TO_INT (user_data);

    if (T.done || s == NULL)
        return G_SOURCE_REMOVE;
    if (T.settle_source != 0 && timeout)
        g_source_remove (T.settle_source);
    if (T.timeout_source != 0 && !timeout)
        g_source_remove (T.timeout_source);
    T.settle_source = T.timeout_source = 0;
    if (!transcript_matches (s) || (s->title != NULL && !T.title_seen)) {
        /* by length: the transcript is not NULL-terminated (g_strjoinv() read past its end, freed strings of earlier
         * steps included, up to 0.7.1) */
        GString *got = g_string_new (NULL);
        GString *want = g_string_new (NULL);
        for (guint i = 0; i < T.transcript->len; i++)
            g_string_append_printf (got, "%s%s", i > 0 ? ", " : "", (const char *) g_ptr_array_index (T.transcript, i));
        for (guint p = 0; p < s->expect->len; p++)
            g_string_append_printf (want, "%s%s", p > 0 ? ", " : "", (const char *) g_ptr_array_index (s->expect, p));
        fail ("%s; expected [%s]%s%s%s, got [%s]", timeout ? "timeout" : "unexpected events", want->str,
              s->title != NULL ? " and the title \"" : "", s->title != NULL ? s->title : "",
              s->title != NULL ? (T.title_seen ? "\" (seen)" : "\" (not seen)") : "", got->str);
        g_string_free (got, TRUE);
        g_string_free (want, TRUE);
        finish ();
        return G_SOURCE_REMOVE;
    }
    g_print ("%s: step %u ok: %s\n", mode_name (), T.index + 1, s->name);
    if (T.failures > 0) {
        finish ();
        return G_SOURCE_REMOVE;
    }
    step_begin (T.index + 1);
    return G_SOURCE_REMOVE;
}

/* the expectations are met: a quiet time for events that should not come */
static void
maybe_complete (void)
{
    Step *s = T.step;

    if (T.done || s == NULL || T.settle_source != 0 || T.action_source != 0)
        return;
    if (T.transcript->len < required_events (s) || (s->title != NULL && !T.title_seen))
        return;
    T.settle_source = g_timeout_add (s->settle_ms, step_evaluate, GINT_TO_POINTER (FALSE));
}

/* the web process(es) among the tool's descendants: WPEWebProcess on WPE, WebKitWebProcess (cut to 15 characters in
 * /proc/<pid>/comm) on WebKitGTK; with kill each gets SIGKILL (the view's one dies, which is all the test needs) */
int
request_test_web_processes (gboolean kill_them)
{
    GHashTable *parent = g_hash_table_new (NULL, NULL);
    GHashTable *name = g_hash_table_new_full (NULL, NULL, NULL, g_free);
    DIR *dir = opendir ("/proc");
    struct dirent *entry;
    int killed = 0;

    if (dir == NULL)
        return 0;
    while ((entry = readdir (dir)) != NULL) {
        int pid = atoi (entry->d_name), ppid = 0;
        char *path, *stat = NULL, *close_paren;
        if (pid <= 0)
            continue;
        path = g_strdup_printf ("/proc/%d/stat", pid);
        if (g_file_get_contents (path, &stat, NULL, NULL) && (close_paren = strrchr (stat, ')')) != NULL) {
            char *open_paren = strchr (stat, '(');
            char state = '?';
            /* a zombie is gone already (its parent has not reaped it yet) */
            if (open_paren != NULL && sscanf (close_paren + 2, "%c %d", &state, &ppid) == 2 && state != 'Z') {
                g_hash_table_insert (parent, GINT_TO_POINTER (pid), GINT_TO_POINTER (ppid));
                g_hash_table_insert (name, GINT_TO_POINTER (pid), g_strndup (open_paren + 1, close_paren - open_paren - 1));
            }
        }
        g_free (stat);
        g_free (path);
    }
    closedir (dir);
    {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, name);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            int pid = GPOINTER_TO_INT (key), up = pid;
            gboolean ours = FALSE;
            if (!g_str_has_prefix (value, "WPEWebProcess") && !g_str_has_prefix (value, "WebKitWebProces"))
                continue;
            for (int depth = 0; depth < 8 && up > 1; depth++) {
                up = GPOINTER_TO_INT (g_hash_table_lookup (parent, GINT_TO_POINTER (up)));
                if (up == (int) getpid ()) {
                    ours = TRUE;
                    break;
                }
            }
            if (ours && (!kill_them || kill (pid, SIGKILL) == 0))
                killed++;
        }
    }
    g_hash_table_unref (parent);
    g_hash_table_unref (name);
    return killed;
}

static gboolean
step_timeout (gpointer user_data)
{
    (void) user_data;
    T.timeout_source = 0;
    return step_evaluate (GINT_TO_POINTER (TRUE));
}

/* the tap's release; the step's expectations count from here (maybe_complete() waits for the action) */
static gboolean
tap_release (gpointer user_data)
{
    (void) user_data;
    T.action_source = 0;
    uc_webview_touch (T.wv, 0, UC_WEBVIEW_TOUCH_UP, TAP_X, TAP_Y, 0);
    maybe_complete ();
    return G_SOURCE_REMOVE;
}

static gboolean
step_act (gpointer user_data)
{
    Step *s = T.step;
    char *uri;

    (void) user_data;
    T.action_source = 0;
    switch (s->action) {
    case ACT_TAP:
        g_print ("%s: tap at %.0f,%.0f\n", mode_name (), TAP_X, TAP_Y);
        uc_webview_touch (T.wv, 0, UC_WEBVIEW_TOUCH_DOWN, TAP_X, TAP_Y, 0);
        T.action_source = g_timeout_add (TAP_UP_MS, tap_release, NULL);
        return G_SOURCE_REMOVE;
    case ACT_LOAD:
        uri = g_strconcat (base, s->arg, NULL);
        uc_webview_load_uri (T.wv, uri);
        g_free (uri);
        break;
    case ACT_JS:
        uc_webview_run_javascript (T.wv, s->arg);
        break;
    case ACT_RELOAD:
        uc_webview_reload (T.wv);
        break;
    case ACT_BACK:
        uc_webview_go_back (T.wv);
        break;
    case ACT_KILL:
        g_print ("%s: killed %d web process(es)\n", mode_name (), request_test_web_processes (TRUE));
        break;
    }
    maybe_complete ();
    return G_SOURCE_REMOVE;
}

static void
step_begin (guint index)
{
    T.index = index;
    if (index >= T.steps->len) {
        T.step = NULL;
        finish ();
        return;
    }
    T.step = g_ptr_array_index (T.steps, index);
    g_ptr_array_set_size (T.transcript, 0);
    T.answer_index = 0;
    T.title_seen = FALSE;
    g_print ("%s: step %u: %s\n", mode_name (), index + 1, T.step->name);
    T.timeout_source = g_timeout_add_seconds (20, step_timeout, NULL);
    T.action_source = g_timeout_add (T.step->delay_ms, step_act, NULL);
}

static void
record (const char *format, ...) G_GNUC_PRINTF (1, 2);

static void
record (const char *format, ...)
{
    va_list ap;
    char *event;

    va_start (ap, format);
    event = g_strdup_vprintf (format, ap);
    va_end (ap);
    if (!request_test_mode () || T.done) {
        g_free (event);
        return;
    }
    g_print ("%s:   > %s\n", mode_name (), event);
    g_ptr_array_add (T.transcript, event);
    maybe_complete ();
}

/* ---- answers ------------------------------------------------------------------------------------------------ */

static char
next_answer (gboolean auth)
{
    const char *answers = T.step != NULL ? T.step->answers : NULL;
    char answer;

    if (!request_test_mode () || answers == NULL || *answers == '\0')
        return auth ? auth_answer : dialog_answer;
    answer = answers[MIN (T.answer_index, (guint) strlen (answers) - 1)];
    T.answer_index++;
    return answer;
}

static Pending *
find_pending (guint64 id)
{
    for (guint i = 0; i < T.pending->len; i++)
        if (g_array_index (T.pending, Pending, i).id == id)
            return &g_array_index (T.pending, Pending, i);
    return NULL;
}

static void
drop_pending (guint64 id)
{
    for (guint i = 0; i < T.pending->len; i++)
        if (g_array_index (T.pending, Pending, i).id == id) {
            g_array_remove_index (T.pending, i);
            return;
        }
}

typedef struct {
    guint64 id;
    char answer;
} Answer;

static gboolean
answer_now (gpointer user_data)
{
    Answer *a = user_data;
    Pending *p = find_pending (a->id);
    const char *user = TEST_USER, *password = TEST_PASSWORD;
    char *credentials = NULL;
    int r;

    if (p == NULL || T.freeing) {
        /* cancelled meanwhile: a late answer is refused */
        if (!T.freeing && uc_webview_request_reply (T.wv, a->id, 0, NULL) != -1)
            fail ("an answer to the cancelled request %" G_GUINT64_FORMAT " was taken", a->id);
        g_free (a);
        return G_SOURCE_REMOVE;
    }
    if (!p->auth) {
        gboolean accept = a->answer == A_ACCEPT || a->answer == A_TEXT;
        const char *text = a->answer == A_TEXT ? dialog_text : NULL;
        /* the wrong calls first (once per run: the library warns about them): refused, the request stays pending */
        if (!T.wrong_dialog_call_done) {
            T.wrong_dialog_call_done = TRUE;
            if (uc_webview_request_auth (T.wv, a->id, "x", "y") != -1)
                fail ("uc_webview_request_auth() took the dialog %" G_GUINT64_FORMAT, a->id);
        }
        if (uc_webview_request_reply (T.wv, a->id + 1000000, 1, NULL) != -1 || uc_webview_request_reply (T.wv, 0, 1, NULL) != -1)
            fail ("uc_webview_request_reply() took an unknown id");
        r = uc_webview_request_reply (T.wv, a->id, accept, text);
        g_print ("%s: request %" G_GUINT64_FORMAT " answered %s%s: %d\n", mode_name (), a->id, accept ? "yes" : "no",
                 text != NULL ? " with text" : "", r);
    } else {
        if (!T.wrong_auth_call_done) {
            T.wrong_auth_call_done = TRUE;
            if (uc_webview_request_reply (T.wv, a->id, 1, NULL) != -1)
                fail ("uc_webview_request_reply(accept=1) took the login %" G_GUINT64_FORMAT, a->id);
        }
        if (opt_credentials != NULL && mode () != MODE_AUTH) {
            credentials = g_strdup (opt_credentials);
            *strchr (credentials, ':') = '\0';
            user = credentials;
            password = credentials + strlen (credentials) + 1;
        }
        switch (a->answer) {
        case A_WRONG:
            r = uc_webview_request_auth (T.wv, a->id, user, WRONG_PASSWORD);
            g_print ("%s: request %" G_GUINT64_FORMAT " answered with a wrong password: %d\n", mode_name (), a->id, r);
            break;
        case A_LOGIN:
            r = uc_webview_request_auth (T.wv, a->id, user, password);
            g_print ("%s: request %" G_GUINT64_FORMAT " answered with credentials: %d\n", mode_name (), a->id, r);
            break;
        case A_NO:
            r = uc_webview_request_reply (T.wv, a->id, 0, NULL);
            g_print ("%s: request %" G_GUINT64_FORMAT " answered no (uc_webview_request_reply): %d\n", mode_name (), a->id, r);
            break;
        case A_CANCEL:
        default:
            r = uc_webview_request_auth (T.wv, a->id, NULL, NULL);
            g_print ("%s: request %" G_GUINT64_FORMAT " answered without credentials: %d\n", mode_name (), a->id, r);
            break;
        }
        g_free (credentials);
    }
    if (r != 0)
        fail ("the answer to request %" G_GUINT64_FORMAT " returned %d", a->id, r);
    p = find_pending (a->id);
    if (p != NULL)
        p->answered = TRUE;
    drop_pending (a->id);
    /* a second answer finds nothing */
    if (uc_webview_request_reply (T.wv, a->id, 0, NULL) != -1 || uc_webview_request_auth (T.wv, a->id, NULL, NULL) != -1)
        fail ("a second answer to request %" G_GUINT64_FORMAT " was taken", a->id);
    g_free (a);
    return G_SOURCE_REMOVE;
}

static void
answer (guint64 id, gboolean auth, int type)
{
    Pending p = { id, auth, type, FALSE };
    Answer *a = g_new0 (Answer, 1);

    a->id = id;
    a->answer = next_answer (auth);
    g_array_append_val (T.pending, p);
    if (a->answer == A_NEVER) {
        g_print ("%s: request %" G_GUINT64_FORMAT " left unanswered\n", mode_name (), id);
        T.pending_at_free = id;
        g_free (a);
        return;
    }
    if (opt_reply_in_callback)
        answer_now (a);
    else
        g_timeout_add (50, answer_now, a);
}

/* ---- request callbacks ------------------------------------------------------------------------------------- */

static void
check_in_free (const char *what)
{
    T.requests++;
    if (T.freeing) {
        T.callbacks_in_free++;
        fail ("%s from inside uc_webview_free()", what);
    }
}

static const char *
dialog_name (int type)
{
    static const char *const names[] = { "alert", "confirm", "prompt", "beforeunload" };
    return type >= 0 && type <= 3 ? names[type] : "unknown";
}

static void
on_script_dialog (uc_webview *wv, uint64_t request, int type, const char *origin, const char *message,
                  const char *default_text, void *user)
{
    T.wv = wv;
    (void) user;
    check_in_free ("script_dialog()");
    g_print ("%s: request %" G_GUINT64_FORMAT ": %s from %s, message %s%s\n", mode_name (), (guint64) request,
             dialog_name (type), origin != NULL ? origin : "(no origin)",
             message != NULL ? "given" : "NULL", type == UC_WEBVIEW_DIALOG_PROMPT
             ? (default_text != NULL ? ", default text given" : ", default text NULL") : "");
    if (request == 0)
        fail ("request id 0");
    if (request_test_mode ()) {
        if (g_strcmp0 (origin, base) != 0)
            fail ("origin %s, expected %s", origin != NULL ? origin : "NULL", base);
        if (type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD) {
            if (message != NULL || default_text != NULL)
                fail ("beforeunload with a message or default text (the page's text must not reach the host)");
        } else if (message == NULL || !g_str_has_prefix (message, "uc-")) {
            fail ("the dialog's message is not the page's");
        }
        if (type == UC_WEBVIEW_DIALOG_PROMPT && g_strcmp0 (default_text, "uc-default") != 0)
            fail ("the prompt's default text is not the page's");
        if (type != UC_WEBVIEW_DIALOG_PROMPT && default_text != NULL)
            fail ("default text for a %s", dialog_name (type));
    }
    record ("dialog:%s", dialog_name (type));
    answer (request, FALSE, type);
}

static const char *
scheme_name (int scheme)
{
    switch (scheme) {
    case UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC:
        return "basic";
    case UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST:
        return "digest";
    case UC_WEBVIEW_AUTH_SCHEME_NTLM:
        return "ntlm";
    case UC_WEBVIEW_AUTH_SCHEME_NEGOTIATE:
        return "negotiate";
    default:
        return "other";
    }
}

static void
on_authenticate (uc_webview *wv, uint64_t request, const char *origin, const char *realm, int scheme, unsigned flags,
                 void *user)
{
    T.wv = wv;
    (void) user;
    check_in_free ("authenticate()");
    g_print ("%s: request %" G_GUINT64_FORMAT ": login for %s, %s, realm %s%s%s\n", mode_name (), (guint64) request,
             origin != NULL ? origin : "(no origin)", scheme_name (scheme),
             realm == NULL ? "NULL" : (request_test_mode () ? (g_str_equal (realm, REALM) ? "as expected" : "OTHER")
                                                            : "given"),
             flags & UC_WEBVIEW_AUTH_RETRY ? ", retry" : "", flags & UC_WEBVIEW_AUTH_PROXY ? ", proxy" : "");
    if (request == 0)
        fail ("request id 0");
    if (request_test_mode () && (g_strcmp0 (origin, base) != 0 || g_strcmp0 (realm, REALM) != 0))
        fail ("origin or realm not the server's");
    record ("auth:%s%s%s", scheme_name (scheme), flags & UC_WEBVIEW_AUTH_RETRY ? ":retry" : "",
            flags & UC_WEBVIEW_AUTH_PROXY ? ":proxy" : "");
    answer (request, TRUE, scheme);
}

static void
on_request_cancelled (uc_webview *wv, uint64_t request, void *user)
{
    Pending *p = find_pending (request);

    (void) wv; (void) user;
    check_in_free ("request_cancelled()");
    g_print ("%s: request %" G_GUINT64_FORMAT " cancelled\n", mode_name (), (guint64) request);
    if (p == NULL)
        fail ("request_cancelled(%" G_GUINT64_FORMAT ") for no pending request", (guint64) request);
    drop_pending (request);
    if (T.pending_at_free == request)
        T.pending_at_free = 0;
    /* a late answer: refused */
    if (uc_webview_request_reply (T.wv, request, 1, NULL) != -1 || uc_webview_request_auth (T.wv, request, NULL, NULL) != -1)
        fail ("an answer after request_cancelled(%" G_GUINT64_FORMAT ") was taken", (guint64) request);
    record ("cancelled");
}

static void
on_close_requested (uc_webview *wv, void *user)
{
    (void) wv; (void) user;
    check_in_free ("close_requested()");
    g_print ("%s: the page asked to close the view\n", mode_name ());
    record ("close");
}

/* A URI in the transcript: the loggable form plus the path of a URI with a host. The steps compare paths
 * ("*\/page2"), and in a request mode every such URI is a page of the tool's own server, named by the tool; query,
 * fragment and payloads stay out as in the log (library 0.10.1 logs the origin only). */
static char *
transcript_uri (const char *uri)
{
    gboolean full = uc_env_value_true (g_getenv ("UC_WEBVIEW_LOG_FULL_URLS"));
    char *loggable = uc_uri_policy_loggable (uri, full);
    GUri *parsed = !full && uri != NULL ? g_uri_parse (uri, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_ENCODED, NULL)
                                        : NULL;
    char *text;

    if (parsed == NULL || g_uri_get_host (parsed) == NULL || *g_uri_get_host (parsed) == '\0')
        text = g_strdup (loggable);
    else
        text = g_strconcat (loggable, g_uri_get_path (parsed), NULL);
    g_clear_pointer (&parsed, g_uri_unref);
    g_free (loggable);
    return text;
}

static void
on_new_window (uc_webview *wv, const char *uri, int result, void *user)
{
    char *loggable = uc_uri_policy_loggable (uri, uc_env_value_true (g_getenv ("UC_WEBVIEW_LOG_FULL_URLS")));
    char *page = transcript_uri (uri);

    (void) wv; (void) user;
    check_in_free ("new_window()");
    g_print ("%s: new window %s: %s\n", mode_name (), loggable,
             result == UC_WEBVIEW_NEW_WINDOW_LOADED ? "loaded into this view" : "refused");
    record ("new-window:%s:%s", result == UC_WEBVIEW_NEW_WINDOW_LOADED ? "loaded" : "refused", page);
    g_free (page);
    g_free (loggable);
}

static void
on_response_refused (uc_webview *wv, const char *uri, const char *mime_type, int reason, void *user)
{
    char *loggable = uc_uri_policy_loggable (uri, uc_env_value_true (g_getenv ("UC_WEBVIEW_LOG_FULL_URLS")));

    (void) wv; (void) user;
    check_in_free ("response_refused()");
    g_print ("%s: refused %s %s (%s)\n", mode_name (), reason == UC_WEBVIEW_RESPONSE_DOWNLOAD ? "download" : "document",
             loggable, mime_type != NULL ? mime_type : "type not known");
    if (!request_test_mode () && !any_commit && tool_quit != NULL) {
        /* the URL the tool was given is a download or a document WebKit does not show: nothing to render (up to
         * 0.6.1 this was "load FAILED") */
        g_print ("load refused: %s\n", loggable);
        tool_quit (1);
    }
    {
        char *page = transcript_uri (uri);
        record ("refused:%s:%s:%s", reason == UC_WEBVIEW_RESPONSE_DOWNLOAD ? "download" : "unsupported", page,
                mime_type != NULL ? mime_type : "null");
        g_free (page);
    }
    g_free (loggable);
}

void
request_test_set_quit (RequestTestQuit quit)
{
    tool_quit = quit;
}

void
request_test_set_callbacks (uc_webview_callbacks *cb)
{
    /* the request callbacks answer in every mode */
    if (T.pending == NULL)
        T.pending = g_array_new (FALSE, FALSE, sizeof (Pending));
    if (T.transcript == NULL)
        T.transcript = g_ptr_array_new_with_free_func (g_free);
    cb->script_dialog = on_script_dialog;
    cb->authenticate = on_authenticate;
    cb->request_cancelled = on_request_cancelled;
    cb->close_requested = on_close_requested;
    cb->new_window = on_new_window;
    cb->response_refused = on_response_refused;
}

/* ---- the tool's other callbacks --------------------------------------------------------------------------- */

gboolean
request_test_load_changed (int event, const char *uri)
{
    static const char *const names[] = { "started", "redirected", "committed", "finished", "FAILED" };

    if (event == UC_WEBVIEW_LOAD_COMMITTED)
        any_commit = TRUE;
    if (!request_test_mode ())
        return FALSE;
    if (event == UC_WEBVIEW_LOAD_COMMITTED) {
        char *page = transcript_uri (uri);
        record ("load:committed:%s", page);
        g_free (page);
    } else
        record ("load:%s", event >= 0 && event <= 4 ? names[event] : "?");
    return TRUE;
}

void
request_test_title_changed (const char *title)
{
    if (!request_test_mode () || T.step == NULL || T.step->title == NULL || title == NULL)
        return;
    if (g_pattern_match_simple (T.step->title, title)) {
        T.title_seen = TRUE;
        maybe_complete ();
    }
}

gboolean
request_test_terminated (int reason)
{
    if (!request_test_mode ())
        return FALSE;
    record ("terminated");
    (void) reason;
    return TRUE;
}

void
request_test_freeing (gboolean freeing)
{
    T.freeing = freeing;
}

int
request_test_finish (int exit_code)
{
    if (!request_test_mode ())
        return exit_code;
    if (T.pending_at_free != 0)
        g_print ("%s: uc_webview_free() with request %" G_GUINT64_FORMAT " pending: %s\n", mode_name (),
                 T.pending_at_free, T.callbacks_in_free == 0 ? "no callback" : "CALLBACKS");
    if (server != NULL) {
        soup_server_disconnect (server);
        g_clear_object (&server);
    }
    if (exit_code == 0 && T.failures > 0)
        exit_code = 1;
    if (exit_code == 0 && T.steps != NULL && T.index < T.steps->len) {
        g_print ("%s: stopped in step %u of %u\n", mode_name (), T.index + 1, T.steps->len);
        exit_code = exit_code != 0 ? exit_code : 1;
    }
    g_print ("%s: %s (%d request callback(s), %d failed check(s))\n", mode_name (), exit_code == 0 ? "ok" : "FAILED",
             T.requests, T.failures);
    return exit_code;
}

/* ---- the modes ----------------------------------------------------------------------------------------------- */

#define LOADED(path) "load:started", "load:committed:*" path, "load:finished"

static void
build_dialog_test (void)
{
    char answers[2] = { dialog_answer, '\0' };
    char *title;

    add_step ("load the dialog page", ACT_LOAD, "/dialog", "dialog-page", NULL, 300, LOADED ("/dialog"), NULL);
    if (T.plain) {
        /* uc_webview_new(): no request callbacks, the library's and WebKit's defaults */
        add_step ("alert, confirm, prompt without callbacks: closed at once (WebKit's default)", ACT_JS,
                  "later(dialogs)", "dialogs alert confirm=false prompt=null", NULL, 300, NULL);
        add_step ("six alerts in a row", ACT_JS, "later(function(){loop(6)})", "loop 6", NULL, 300, NULL);
        add_step ("arm beforeunload", ACT_JS, "arm()", "armed", NULL, 300, NULL);
        add_step ("reload: beforeunload left without a callback (library default)", ACT_RELOAD, NULL, NULL, NULL, 500,
                  LOADED ("/dialog"), NULL);
        add_step ("load page2", ACT_LOAD, "/page2", "page2", NULL, 300, LOADED ("/page2"), NULL);
        add_step ("arm beforeunload on page2", ACT_JS, "arm()", "armed", NULL, 300, NULL);
        add_step ("back: beforeunload left", ACT_BACK, NULL, NULL, NULL, 500, LOADED ("/dialog"), NULL);
        add_step ("an alert, closed at once", ACT_JS, "later(function(){pending('uc-pending')})", "answered", NULL, 300,
                  NULL);
        return;
    }
    if (dialog_answer == A_NEVER) {
        add_step ("an alert, never answered", ACT_JS, "later(dialogs)", NULL, "n", 1000, "dialog:alert", NULL);
        add_step ("a host navigation ends it: request_cancelled(), the page's next dialogs get the default, page2 loads",
                  ACT_LOAD, "/page2", "page2", NULL, 500, "cancelled", LOADED ("/page2"), NULL);
        add_step ("an alert in page2, never answered", ACT_JS, "later(function(){pending('uc-crash')})", NULL, "n", 1000,
                  "dialog:alert", NULL);
        add_step ("the web process is killed: request_cancelled() before web_process_terminated()", ACT_KILL, NULL, NULL,
                  NULL, 1000, "cancelled", "terminated", NULL);
        add_step ("reload after the crash", ACT_RELOAD, NULL, "page2", NULL, 500, LOADED ("/page2"), NULL);
        add_step ("a dialog after the crash reaches the host", ACT_JS, "later(function(){pending('uc-after-crash')})",
                  "answered", "a", 300, "dialog:alert", NULL);
        add_step ("an alert left pending for uc_webview_free()", ACT_JS, "later(function(){pending('uc-pending')})",
                  NULL, "n", 500, "dialog:alert", NULL);
        return;
    }
    if (dialog_answer == A_REJECT)
        title = g_strdup ("dialogs alert confirm=false prompt=null");
    else if (dialog_answer == A_TEXT)
        title = g_strdup_printf ("dialogs alert confirm=true prompt=\"%s\"", dialog_text);
    else
        title = g_strdup ("dialogs alert confirm=true prompt=\"\"");
    add_step ("alert, confirm, prompt", ACT_JS, "later(dialogs)", title, answers, 300, "dialog:alert", "dialog:confirm",
              "dialog:prompt", NULL);
    g_free (title);
    add_step ("six alerts in a row: four reach the host, then the dialog loop guard", ACT_JS,
              "later(function(){loop(6)})", "loop 6", "a", 500, "dialog:alert", "dialog:alert", "dialog:alert",
              "dialog:alert", NULL)->delay_ms = 1200;     /* not quick after the prompt's answer */
    /* library 0.7.2, by design: the guard lasts until the user's input in the page, not a time */
    add_step ("2 s later the guard still holds: an alert gets WebKit's default", ACT_JS,
              "later(function(){pending('uc-guarded')})", "answered", NULL, 500, NULL)->delay_ms = 2000;
    add_step ("a tap on an empty spot of the page ends the guard", ACT_TAP, NULL, NULL, NULL, 300, NULL);
    add_step ("six alerts in a row after the tap: four reach the host again, then the guard", ACT_JS,
              "later(function(){loop(6)})", "loop 6", "a", 500, "dialog:alert", "dialog:alert", "dialog:alert",
              "dialog:alert", NULL);
    add_step ("arm beforeunload", ACT_JS, "arm()", "armed", NULL, 300, NULL);
    if (dialog_answer == A_REJECT) {
        add_step ("reload: beforeunload, stay", ACT_RELOAD, NULL, NULL, "r", 1500, "dialog:beforeunload", NULL);
        add_step ("reload again: beforeunload, leave", ACT_RELOAD, NULL, "dialog-page", "a", 500, "dialog:beforeunload",
                  LOADED ("/dialog"), NULL);
    } else {
        add_step ("reload: beforeunload, leave", ACT_RELOAD, NULL, "dialog-page", "a", 500, "dialog:beforeunload",
                  LOADED ("/dialog"), NULL);
    }
    add_step ("load page2", ACT_LOAD, "/page2", "page2", NULL, 300, LOADED ("/page2"), NULL);
    add_step ("arm beforeunload on page2", ACT_JS, "arm()", "armed", NULL, 300, NULL);
    add_step ("back: beforeunload, leave", ACT_BACK, NULL, NULL, "a", 500, "dialog:beforeunload", LOADED ("/dialog"),
              NULL);
    add_step ("an alert left pending for uc_webview_free()", ACT_JS, "later(function(){pending('uc-pending')})", NULL,
              "n", 500, "dialog:alert", NULL);
}

static void
build_auth_test (void)
{
    const char *scheme = opt_auth_scheme != NULL ? opt_auth_scheme : "basic";
    char *path = g_strdup_printf ("/%s", scheme);
    char *ask = g_strdup_printf ("auth:%s", scheme);
    char *retry = g_strdup_printf ("auth:%s:retry", scheme);
    char *committed = g_strdup_printf ("load:committed:*%s", path);
    char *go_page3 = g_strdup ("location.href='/page3'");

    add_step ("load page2", ACT_LOAD, "/page2", "page2", NULL, 300, LOADED ("/page2"), NULL);
    if (T.plain) {
        add_step ("no authenticate callback: continued without credentials, the server's 401 page", ACT_LOAD, path,
                  "auth-401", NULL, 500, "load:started", committed, "load:finished", NULL);
    } else if (auth_answer == A_LOGIN) {
        add_step ("a wrong password, a retry, then the right one", ACT_LOAD, path, "auth-ok", "wl", 500, "load:started",
                  ask, retry, committed, "load:finished", NULL);
    } else if (auth_answer == A_CANCEL || auth_answer == A_NO) {
        add_step (auth_answer == A_CANCEL ? "uc_webview_request_auth(NULL): the server's 401 page"
                                          : "uc_webview_request_reply(0): the server's 401 page",
                  ACT_LOAD, path, "auth-401", auth_answer == A_CANCEL ? "c" : "o", 500, "load:started", ask, committed,
                  "load:finished", NULL);
    } else {
        add_step ("a login, never answered", ACT_LOAD, path, NULL, "n", 1000, "load:started", ask, NULL);
        /* WebKit ends the provisional load it replaces with FINISHED (webkitWebViewWillStartLoad(),
         * WebKitWebView.cpp:2712-2730) and cancels its request before that (:2734-2780) */
        add_step ("the page navigates: WebKit cancels it", ACT_JS, go_page3, "page3", NULL, 500, "cancelled",
                  "load:finished", LOADED ("/page3"), NULL);
        add_step ("a login again, never answered", ACT_LOAD, path, NULL, "n", 1000, "load:started", ask, NULL);
        add_step ("a host navigation cancels it", ACT_LOAD, "/page2", "page2", NULL, 500, "cancelled", "load:finished",
                  LOADED ("/page2"), NULL);
    }
    g_free (path);
    g_free (ask);
    g_free (retry);
    g_free (committed);
    g_free (go_page3);
}

static void
build_window_test (void)
{
    gboolean cb = !T.plain;

    add_step ("load the window page", ACT_LOAD, "/window", "window-page", NULL, 300, LOADED ("/window"), NULL);
    add_step ("window.close() of a page with one history entry", ACT_JS, "later(function(){window.close()})", NULL,
              NULL, 1000, cb ? "close" : NULL, NULL);
    add_step ("a target=_blank link loads into this view", ACT_JS, "later(function(){click('blank')})", "page2", NULL,
              500, cb ? "new-window:loaded:*/page2" : "load:started", cb ? "load:started" : "load:committed:*/page2",
              cb ? "load:committed:*/page2" : "load:finished", cb ? "load:finished" : NULL, NULL);
    add_step ("window.open('/page3') loads into this view", ACT_JS, "later(function(){openWin('/page3')})", "page3",
              NULL, 500, cb ? "new-window:loaded:*/page3" : "load:started", cb ? "load:started" : "load:committed:*/page3",
              cb ? "load:committed:*/page3" : "load:finished", cb ? "load:finished" : NULL, NULL);
    add_step ("window.open() of a data: URL is refused", ACT_JS,
              "later(function(){openWin('data:text/html,<title>uc-data</title>')})", NULL, NULL, 1500,
              cb ? "new-window:refused:data:*" : NULL, NULL);
    if (cb)
        add_step ("confirm(), then window.open() from the same gesture", ACT_JS,
                  "later(function(){if(confirm('uc-confirm'))openWin('/page2')})", "page2", "a", 500, "dialog:confirm",
                  "new-window:loaded:*/page2", LOADED ("/page2"), NULL);
    else
        add_step ("confirm() is false without a callback: no window", ACT_JS,
                  "later(function(){if(confirm('uc-confirm'))openWin('/page2')})", NULL, NULL, 1000, NULL);
    add_step ("a form posted to a new window is refused", ACT_JS, "later(post)", NULL, NULL, 1500,
              cb ? "new-window:refused:*/page2" : NULL, NULL);
    add_step ("the page stays", ACT_JS, "where()", cb ? "at /page2" : "at /page3", NULL, 300, NULL);
}

static void
build_download_test (void)
{
    gboolean cb = !T.plain;

    add_step ("load the download page (frames with an attachment and a PDF: refused silently)", ACT_LOAD, "/download",
              "download-page", NULL, 1500, LOADED ("/download"), NULL);
    add_step ("an attachment link: refused, no FAILED, the page stays", ACT_JS, "later(function(){click('attachment')})",
              NULL, NULL, 1000, "load:started", cb ? "refused:download:*/attachment.txt:text/plain" : "load:finished",
              cb ? "load:finished" : NULL, NULL);
    add_step ("<a download>: refused", ACT_JS, "later(function(){click('dl')})", NULL, NULL, 1500,
              cb ? "refused:download:*/file.bin:*" : NULL, NULL);
    add_step ("an unknown type: refused", ACT_JS, "later(function(){click('unknown')})", NULL, NULL, 1000,
              "load:started", cb ? "refused:unsupported:*/unknown:application/x-ucwebview-unknown" : "load:finished",
              cb ? "load:finished" : NULL, NULL);
    add_step ("a PDF (viewer off): refused as unsupported", ACT_JS, "later(function(){click('pdf')})", NULL, NULL, 1000,
              "load:started", cb ? "refused:unsupported:*/doc.pdf:application/pdf" : "load:finished",
              cb ? "load:finished" : NULL, NULL);
    add_step ("a host load of an attachment: refused, no FAILED", ACT_LOAD, "/attachment.txt", NULL, NULL, 1000,
              "load:started", cb ? "refused:download:*/attachment.txt:text/plain" : "load:finished",
              cb ? "load:finished" : NULL, NULL);
    add_step ("the page stays", ACT_JS, "where()", "at /download", NULL, 300, NULL);
}

gboolean
request_test_start (uc_webview *wv, gboolean plain, RequestTestQuit quit)
{
    T.wv = wv;
    T.plain = plain;
    T.quit = quit;
    T.steps = g_ptr_array_new_with_free_func (step_free);
    if (!server_start ())
        return FALSE;
    switch (mode ()) {
    case MODE_DIALOG:
        build_dialog_test ();
        break;
    case MODE_AUTH:
        build_auth_test ();
        break;
    case MODE_WINDOW:
        build_window_test ();
        break;
    case MODE_DOWNLOAD:
        build_download_test ();
        break;
    case MODE_NONE:
    default:
        return FALSE;
    }
    g_print ("%s: %u steps%s%s\n", mode_name (), T.steps->len, plain ? ", view from uc_webview_new(): no request callbacks" : "",
             opt_reply_in_callback ? ", answers from inside the callbacks" : "");
    step_begin (0);
    return TRUE;
}
