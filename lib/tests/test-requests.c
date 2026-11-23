/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_requests.c (GLib only): run with `make check`; `make check-asan` runs it again built with
 * AddressSanitizer and UBSan. The request table and its id rules, every way a request ends (exactly once, also when
 * the host answers or navigates from inside a callback), the dialog answers, the dialog loop guard, the origins, the
 * scheme filter for new windows and the response verdicts. The WebKit side (uc_requests_wk.c) runs in the host run on
 * WebKitGTK and on the device (package README, "Requests (0.7.0)").
 */
#include "uc_requests.h"
#include "uc_webview.h"

#include <string.h>

static int objects[8];   /* stand-ins for WebKit objects: compared, never dereferenced */

/* ---- ids and the table ------------------------------------------------------------------------------------- */

static void
test_ids (void)
{
    UcRequestTable *a = uc_request_table_new (), *b = uc_request_table_new ();
    guint64 first = uc_request_next_id ();
    UcRequest *r1, *r2, *r3;
    guint64 id1;

    g_assert_cmpuint (first, >=, 1);
    /* process-wide: two views draw from the same counter, so an id names one request of the process */
    r1 = uc_request_table_add (a, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_ALERT, &objects[0]);
    r2 = uc_request_table_add (b, UC_REQUEST_AUTH, UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, &objects[1]);
    r3 = uc_request_table_add (a, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_PROMPT, &objects[2]);
    g_assert_cmpuint (r1->id, ==, first + 1);
    g_assert_cmpuint (r2->id, ==, first + 2);
    g_assert_cmpuint (r3->id, ==, first + 3);
    g_assert_true (r1->object == &objects[0] && r1->kind == UC_REQUEST_DIALOG && r1->type == UC_WEBVIEW_DIALOG_ALERT);
    g_assert_cmpuint (r1->handler, ==, 0);

    /* lookup: per table; 0 and unknown ids find nothing */
    g_assert_true (uc_request_table_lookup (a, r1->id) == r1);
    g_assert_null (uc_request_table_lookup (a, r2->id));
    g_assert_true (uc_request_table_lookup (b, r2->id) == r2);
    g_assert_null (uc_request_table_lookup (a, 0));
    g_assert_null (uc_request_table_lookup (a, G_MAXUINT64));
    g_assert_true (uc_request_table_find_object (a, &objects[2]) == r3);
    g_assert_null (uc_request_table_find_object (a, &objects[1]));

    /* steal ends it: a second steal, a lookup and a reply check find nothing */
    id1 = r1->id;
    g_assert_true (uc_request_table_steal (a, id1) == r1);
    g_free (r1);
    g_assert_null (uc_request_table_steal (a, id1));
    g_assert_null (uc_request_table_lookup (a, id1));
    g_assert_cmpint (uc_request_check_reply (uc_request_table_lookup (a, id1), FALSE, 1), ==, UC_REPLY_UNKNOWN);

    /* never reused: the next request after an end gets a new id */
    r1 = uc_request_table_add (a, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_CONFIRM, &objects[3]);
    g_assert_cmpuint (r1->id, ==, first + 4);
    g_assert_null (uc_request_table_lookup (a, id1));

    g_ptr_array_unref (uc_request_table_steal_kind (a, UC_REQUEST_ANY));
    g_ptr_array_unref (uc_request_table_steal_kind (b, UC_REQUEST_ANY));
    uc_request_table_free (a);
    uc_request_table_free (b);
    uc_request_table_free (NULL);
}

static void
test_steal_kind (void)
{
    UcRequestTable *t = uc_request_table_new ();
    UcRequest *d1 = uc_request_table_add (t, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_ALERT, &objects[0]);
    UcRequest *a1 = uc_request_table_add (t, UC_REQUEST_AUTH, UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, &objects[1]);
    UcRequest *a2 = uc_request_table_add (t, UC_REQUEST_AUTH, UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST, &objects[2]);
    UcRequest *d2 = uc_request_table_add (t, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_PROMPT, &objects[3]);
    GPtrArray *auth, *rest;

    g_assert_cmpuint (uc_request_table_count (t, UC_REQUEST_ANY), ==, 4);
    g_assert_cmpuint (uc_request_table_count (t, UC_REQUEST_AUTH), ==, 2);
    g_assert_cmpuint (uc_request_table_count (t, UC_REQUEST_DIALOG), ==, 2);

    /* the authentication requests, oldest first; the dialogs stay */
    auth = uc_request_table_steal_kind (t, UC_REQUEST_AUTH);
    g_assert_cmpuint (auth->len, ==, 2);
    g_assert_true (g_ptr_array_index (auth, 0) == a1 && g_ptr_array_index (auth, 1) == a2);
    g_assert_cmpuint (uc_request_table_count (t, UC_REQUEST_AUTH), ==, 0);
    g_assert_true (uc_request_table_lookup (t, d1->id) == d1 && uc_request_table_lookup (t, d2->id) == d2);
    g_ptr_array_unref (auth);

    /* nothing of a kind: an empty array */
    auth = uc_request_table_steal_kind (t, UC_REQUEST_AUTH);
    g_assert_cmpuint (auth->len, ==, 0);
    g_ptr_array_unref (auth);

    rest = uc_request_table_steal_kind (t, UC_REQUEST_ANY);
    g_assert_cmpuint (rest->len, ==, 2);
    g_assert_true (g_ptr_array_index (rest, 0) == d1 && g_ptr_array_index (rest, 1) == d2);
    g_assert_cmpuint (uc_request_table_count (t, UC_REQUEST_ANY), ==, 0);
    g_ptr_array_unref (rest);
    uc_request_table_free (t);
}

/* uc_request_table_free() with requests left is a bug of the owner: logged as critical (fatal in tests) */
static void
test_free_nonempty (void)
{
    if (g_test_subprocess ()) {
        UcRequestTable *t = uc_request_table_new ();
        uc_request_table_add (t, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_ALERT, &objects[0]);
        uc_request_table_free (t);
        return;
    }
    g_test_trap_subprocess (NULL, 0, (GTestSubprocessFlags) 0);
    g_test_trap_assert_failed ();
    g_test_trap_assert_stderr ("*1 request(s) still pending*");
}

static void
test_check_reply (void)
{
    UcRequest dialog = { 5, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_CONFIRM, NULL, 0 };
    UcRequest auth = { 6, UC_REQUEST_AUTH, UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, NULL, 0 };

    /* uc_webview_request_reply() */
    g_assert_cmpint (uc_request_check_reply (&dialog, FALSE, 1), ==, UC_REPLY_OK);
    g_assert_cmpint (uc_request_check_reply (&dialog, FALSE, 0), ==, UC_REPLY_OK);
    g_assert_cmpint (uc_request_check_reply (&dialog, FALSE, 7), ==, UC_REPLY_OK);      /* any non-zero is yes */
    g_assert_cmpint (uc_request_check_reply (&auth, FALSE, 0), ==, UC_REPLY_OK);        /* without credentials */
    g_assert_cmpint (uc_request_check_reply (&auth, FALSE, 1), ==, UC_REPLY_WRONG_KIND);
    g_assert_cmpint (uc_request_check_reply (NULL, FALSE, 0), ==, UC_REPLY_UNKNOWN);
    /* uc_webview_request_auth() */
    g_assert_cmpint (uc_request_check_reply (&auth, TRUE, 0), ==, UC_REPLY_OK);
    g_assert_cmpint (uc_request_check_reply (&dialog, TRUE, 0), ==, UC_REPLY_WRONG_KIND);
    g_assert_cmpint (uc_request_check_reply (NULL, TRUE, 0), ==, UC_REPLY_UNKNOWN);
}

/* ---- every request ends exactly once ------------------------------------------------------------------------ */

/*
 * A model of the owner (uc_requests_wk.c) on top of the table: every way a request ends steals it first, then
 * "finishes the WebKit object" (counted per request) and calls the host back for the library's own ends. The host
 * callbacks re-enter like a real host may: answer another request, navigate (end all) or answer the cancelled one.
 */
typedef enum { END_REPLY, END_CANCELLED, END_FREE } EndHow;

typedef struct {
    UcRequestTable *table;
    GHashTable *ends;           /* id -> number of ends */
    GHashTable *cancel_reports; /* id -> number of request_cancelled() */
    GArray *ids;                /* every id issued */
    int reentry;                /* 0: the host does nothing in request_cancelled() */
    gboolean freeing;
} Owner;

static int owner_reply (Owner *owner, guint64 id, gboolean is_auth_call, int accept);
static void owner_end_all (Owner *owner, EndHow how);

static void
count (GHashTable *map, guint64 id)
{
    guint64 *key = g_new (guint64, 1);
    *key = id;
    g_hash_table_replace (map, key, GINT_TO_POINTER (GPOINTER_TO_INT (g_hash_table_lookup (map, &id)) + 1));
}

static void
host_request_cancelled (Owner *owner, guint64 id)
{
    g_assert_false (owner->freeing);    /* never from inside uc_webview_free() */
    count (owner->cancel_reports, id);
    switch (owner->reentry) {
    case 1:
        /* a late answer to the request just reported: refused, nothing ends twice */
        g_assert_cmpint (owner_reply (owner, id, FALSE, 1), ==, -1);
        break;
    case 2:
        /* the host answers every other request it still shows */
        for (guint i = 0; i < owner->ids->len; i++)
            owner_reply (owner, g_array_index (owner->ids, guint64, i), FALSE, 0);
        break;
    case 3:
        /* the host navigates from inside the report: ends what is left */
        owner_end_all (owner, END_CANCELLED);
        break;
    default:
        break;
    }
}

static void
owner_finish (Owner *owner, UcRequest *request, EndHow how)
{
    count (owner->ends, request->id);
    if (how == END_CANCELLED)
        host_request_cancelled (owner, request->id);
}

static int
owner_reply (Owner *owner, guint64 id, gboolean is_auth_call, int accept)
{
    UcRequest *request;

    if (uc_request_check_reply (uc_request_table_lookup (owner->table, id), is_auth_call, accept) != UC_REPLY_OK)
        return -1;
    request = uc_request_table_steal (owner->table, id);
    owner_finish (owner, request, END_REPLY);
    g_free (request);
    return 0;
}

static void
owner_end_all (Owner *owner, EndHow how)
{
    GPtrArray *all = uc_request_table_steal_kind (owner->table, UC_REQUEST_ANY);
    for (guint i = 0; i < all->len; i++)
        owner_finish (owner, g_ptr_array_index (all, i), how);
    g_ptr_array_unref (all);
}

static guint64
owner_add (Owner *owner, UcRequestKind kind)
{
    UcRequest *request = uc_request_table_add (owner->table, kind, kind == UC_REQUEST_AUTH
                                               ? UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC : UC_WEBVIEW_DIALOG_ALERT, &objects[0]);
    g_array_append_val (owner->ids, request->id);
    return request->id;
}

static void
owner_init (Owner *owner, int reentry)
{
    owner->table = uc_request_table_new ();
    owner->ends = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
    owner->cancel_reports = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
    owner->ids = g_array_new (FALSE, FALSE, sizeof (guint64));
    owner->reentry = reentry;
    owner->freeing = FALSE;
}

/* each issued id ended exactly once, and request_cancelled() at most once and only for the library's ends */
static void
owner_check (Owner *owner)
{
    g_assert_cmpuint (uc_request_table_count (owner->table, UC_REQUEST_ANY), ==, 0);
    for (guint i = 0; i < owner->ids->len; i++) {
        guint64 id = g_array_index (owner->ids, guint64, i);
        g_assert_cmpint (GPOINTER_TO_INT (g_hash_table_lookup (owner->ends, &id)), ==, 1);
        g_assert_cmpint (GPOINTER_TO_INT (g_hash_table_lookup (owner->cancel_reports, &id)), <=, 1);
    }
    uc_request_table_free (owner->table);
    g_hash_table_unref (owner->ends);
    g_hash_table_unref (owner->cancel_reports);
    g_array_unref (owner->ids);
}

static void
test_end_paths (void)
{
    for (int reentry = 0; reentry <= 3; reentry++) {
        Owner owner;
        guint64 d1, d2, a1, a2, d3;

        owner_init (&owner, reentry);
        /* answered by the host: twice is refused */
        d1 = owner_add (&owner, UC_REQUEST_DIALOG);
        g_assert_cmpint (owner_reply (&owner, d1, FALSE, 1), ==, 0);
        g_assert_cmpint (owner_reply (&owner, d1, FALSE, 1), ==, -1);
        /* the wrong call leaves the request pending */
        a1 = owner_add (&owner, UC_REQUEST_AUTH);
        g_assert_cmpint (owner_reply (&owner, a1, FALSE, 1), ==, -1);
        g_assert_nonnull (uc_request_table_lookup (owner.table, a1));
        d2 = owner_add (&owner, UC_REQUEST_DIALOG);
        g_assert_cmpint (owner_reply (&owner, d2, TRUE, 0), ==, -1);
        a2 = owner_add (&owner, UC_REQUEST_AUTH);
        /* the library ends the rest (navigation, commit, termination), the host re-enters */
        owner_end_all (&owner, END_CANCELLED);
        g_assert_cmpint (owner_reply (&owner, a1, TRUE, 0), ==, -1);
        g_assert_cmpint (owner_reply (&owner, d2, FALSE, 0), ==, -1);
        g_assert_cmpint (owner_reply (&owner, a2, FALSE, 0), ==, -1);
        /* a new one after that, pending at uc_webview_free(): ends without a callback */
        d3 = owner_add (&owner, UC_REQUEST_DIALOG);
        owner.freeing = TRUE;
        owner_end_all (&owner, END_FREE);
        g_assert_cmpint (owner_reply (&owner, d3, FALSE, 1), ==, -1);
        g_assert_null (g_hash_table_lookup (owner.cancel_reports, &d3));
        owner_check (&owner);
    }
}

/* ---- dialogs ----------------------------------------------------------------------------------------------- */

static void
test_dialog_answer (void)
{
    static const char text[] = "the host's text";
    UcDialogAnswer a;

    /* alert: nothing to set, both answers close it */
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_ALERT, TRUE, text);
    g_assert_false (a.set_confirmed || a.set_text);
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_ALERT, FALSE, NULL);
    g_assert_false (a.set_confirmed || a.set_text);
    /* confirm: true / false */
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_CONFIRM, TRUE, text);
    g_assert_true (a.set_confirmed && a.confirmed && !a.set_text);
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_CONFIRM, FALSE, text);
    g_assert_true (a.set_confirmed && !a.confirmed && !a.set_text);
    /* prompt: the text, "" for NULL; the no leaves it unset (null) */
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_PROMPT, TRUE, text);
    g_assert_true (a.set_text && a.text == text && !a.set_confirmed);
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_PROMPT, TRUE, NULL);
    g_assert_true (a.set_text);
    g_assert_cmpstr (a.text, ==, "");
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_PROMPT, FALSE, text);
    g_assert_false (a.set_text || a.set_confirmed);
    /* beforeunload: yes = leave (TRUE), no = stay */
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, TRUE, NULL);
    g_assert_true (a.set_confirmed && a.confirmed);
    a = uc_dialog_answer (UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, FALSE, NULL);
    g_assert_true (a.set_confirmed && !a.confirmed);
    /* an unknown type: nothing set */
    a = uc_dialog_answer (42, TRUE, text);
    g_assert_false (a.set_confirmed || a.set_text);

    g_assert_cmpstr (uc_dialog_type_name (UC_WEBVIEW_DIALOG_ALERT), ==, "alert");
    g_assert_cmpstr (uc_dialog_type_name (UC_WEBVIEW_DIALOG_CONFIRM), ==, "confirm");
    g_assert_cmpstr (uc_dialog_type_name (UC_WEBVIEW_DIALOG_PROMPT), ==, "prompt");
    g_assert_cmpstr (uc_dialog_type_name (UC_WEBVIEW_DIALOG_BEFORE_UNLOAD), ==, "beforeunload");
    g_assert_cmpstr (uc_dialog_type_name (-1), ==, "dialog");
}

#define MS(n) ((gint64) (n) * 1000)

/* for (;;) alert (1) with a host that answers after 100 ms: four alerts, then WebKit's default */
static void
test_guard_loop (void)
{
    UcDialogGuard guard;
    gint64 t = MS (5000);
    int shown = 0, tripped = 0, suppressed = 0;

    uc_dialog_guard_reset (&guard);
    for (int i = 0; i < 20; i++) {
        switch (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_ALERT, t)) {
        case UC_GUARD_SHOW:
            shown++;
            t += MS (100);                  /* the user reads and answers */
            uc_dialog_guard_ended (&guard, UC_WEBVIEW_DIALOG_ALERT, t);
            break;
        case UC_GUARD_TRIPPED:
            tripped++;
            break;
        case UC_GUARD_SUPPRESS:
            suppressed++;
            break;
        }
        t += MS (1);                        /* the page's next alert */
    }
    g_assert_cmpint (shown, ==, 4);         /* the first and three quick ones */
    g_assert_cmpint (tripped, ==, 1);       /* logged once */
    g_assert_cmpint (suppressed, ==, 15);
    /* beforeunload is never suppressed: WebKit's default ("stay") would keep the user on the page */
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, t), ==, UC_GUARD_SHOW);
    /* the next document starts afresh */
    uc_dialog_guard_reset (&guard);
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_CONFIRM, t), ==, UC_GUARD_SHOW);
}

/* only consecutive quick dialogs count; 1 s and more after an answer is not quick */
static void
test_guard_window (void)
{
    UcDialogGuard guard;
    gint64 t = MS (1000);

    uc_dialog_guard_reset (&guard);
    /* three quick ones, then a slow one resets the count, then three quick ones again: all shown */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 4; i++) {
            g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_PROMPT, t), ==, UC_GUARD_SHOW);
            uc_dialog_guard_ended (&guard, UC_WEBVIEW_DIALOG_PROMPT, t);
            t += MS (999);                  /* just quick */
        }
        t += MS (1);                        /* exactly 1 s after the last answer: not quick */
    }
    g_assert_false (guard.tripped);
    /* now four quick ones in a row: the fifth dialog is suppressed */
    for (int i = 0; i < 4; i++) {
        g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_ALERT, t), ==, UC_GUARD_SHOW);
        uc_dialog_guard_ended (&guard, UC_WEBVIEW_DIALOG_ALERT, t);
        t += MS (10);
    }
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_ALERT, t), ==, UC_GUARD_TRIPPED);
    /* a long pause does not help within the document */
    t += MS (60000);
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_ALERT, t), ==, UC_GUARD_SUPPRESS);
}

/* a page loop: alerts 1 ms after each answer, the host answers after 100 ms; returns the alerts shown before the trip
 * (the check that trips is the last one) */
static int
run_loop (UcDialogGuard *guard, gint64 *t)
{
    int shown = 0;

    for (int i = 0; i < 20; i++) {
        UcGuardVerdict verdict = uc_dialog_guard_check (guard, UC_WEBVIEW_DIALOG_ALERT, *t);
        if (verdict != UC_GUARD_SHOW) {
            g_assert_cmpint (verdict, ==, UC_GUARD_TRIPPED);
            return shown;
        }
        shown++;
        *t += MS (100);
        uc_dialog_guard_ended (guard, UC_WEBVIEW_DIALOG_ALERT, *t);
        *t += MS (1);
    }
    g_assert_not_reached ();
    return shown;
}

/* by design, the user's tap or key press in the page ends a tripped guard; the page's own loop trips it again after
 * the same four dialogs (without it, "confirm+open" tapped 16 s after the "6 alerts" test got no question) */
static void
test_guard_user_input (void)
{
    UcDialogGuard guard;
    gint64 t = MS (5000);

    uc_dialog_guard_reset (&guard);
    g_assert_cmpint (run_loop (&guard, &t), ==, 4);
    g_assert_true (guard.tripped);
    /* time alone does not end it */
    t += MS (16000);
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_CONFIRM, t), ==, UC_GUARD_SUPPRESS);
    /* the tap: ends it (TRUE: logged), all of it */
    g_assert_true (uc_dialog_guard_user_input (&guard));
    g_assert_false (guard.tripped);
    g_assert_cmpuint (guard.quick, ==, 0);
    g_assert_cmpint (guard.last_end_us, ==, 0);
    /* a second input finds nothing to end: no second log line */
    g_assert_false (uc_dialog_guard_user_input (&guard));
    /* the tap's confirm() reaches the host */
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_CONFIRM, t), ==, UC_GUARD_SHOW);
    t += MS (500);
    uc_dialog_guard_ended (&guard, UC_WEBVIEW_DIALOG_CONFIRM, t);
    /* the counting starts from zero: the page's loop right after that answer gets four dialogs again (the first is
     * quick now, so three more), then the guard trips again */
    t += MS (1);
    g_assert_cmpint (run_loop (&guard, &t), ==, 3);
    g_assert_true (guard.tripped);

    /* an input right after an answer: the next dialog is not quick, whatever the last answer's time */
    g_assert_true (uc_dialog_guard_user_input (&guard));
    g_assert_cmpint (run_loop (&guard, &t), ==, 4);

    /* an input before the trip resets the count too: three quick ones, a tap, then four more shown before the trip */
    uc_dialog_guard_reset (&guard);
    for (int i = 0; i < 3; i++) {
        g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_ALERT, t), ==, UC_GUARD_SHOW);
        t += MS (10);
        uc_dialog_guard_ended (&guard, UC_WEBVIEW_DIALOG_ALERT, t);
        t += MS (10);
    }
    g_assert_cmpuint (guard.quick, ==, 2);
    g_assert_false (uc_dialog_guard_user_input (&guard));     /* not tripped: nothing to log */
    g_assert_cmpuint (guard.quick, ==, 0);
    g_assert_cmpint (run_loop (&guard, &t), ==, 4);

    /* beforeunload stays outside: shown with or without the trip, input changes nothing for it */
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, t), ==, UC_GUARD_SHOW);
    g_assert_true (uc_dialog_guard_user_input (&guard));
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, t), ==, UC_GUARD_SHOW);
    /* the next commit's reset still works on its own */
    g_assert_cmpint (run_loop (&guard, &t), ==, 4);
    uc_dialog_guard_reset (&guard);
    g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_PROMPT, t), ==, UC_GUARD_SHOW);
}

/* beforeunload between quick dialogs neither counts nor resets */
static void
test_guard_before_unload (void)
{
    UcDialogGuard guard;
    gint64 t = MS (1000);

    uc_dialog_guard_reset (&guard);
    for (int i = 0; i < 10; i++) {
        g_assert_cmpint (uc_dialog_guard_check (&guard, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, t), ==, UC_GUARD_SHOW);
        uc_dialog_guard_ended (&guard, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, t);
        t += MS (10);
    }
    g_assert_false (guard.tripped);
    g_assert_cmpuint (guard.quick, ==, 0);
    g_assert_cmpint (guard.last_end_us, ==, 0);
}

/* ---- origins, new windows, responses ---------------------------------------------------------------------- */

static void
check_origin (const char *uri, const char *expected)
{
    char *origin = uc_request_origin_from_uri (uri);
    g_assert_cmpstr (origin, ==, expected);
    g_free (origin);
}

static void
test_origin (void)
{
    char *s;

    check_origin ("https://example.com/path?q=1#f", "https://example.com");
    check_origin ("https://example.com:443/", "https://example.com");
    check_origin ("http://example.com:80/", "http://example.com");
    check_origin ("http://127.0.0.1:41234/dialog", "http://127.0.0.1:41234");
    check_origin ("https://example.com:8443/", "https://example.com:8443");
    check_origin ("HTTPS://Example.COM/", "https://example.com");
    check_origin ("https://user:secret@example.com/", "https://example.com");   /* no user info */
    check_origin ("http://[::1]:8080/x", "http://[::1]:8080");
    check_origin ("http://[::1]/x", "http://[::1]");
    check_origin ("file:///etc/os-release", "file://");
    check_origin ("blob:https://example.com/550e8400-e29b-41d4-a716-446655440000", "https://example.com");
    check_origin ("blob:blob:https://example.com/x", NULL);
    check_origin ("about:blank", NULL);
    check_origin ("data:text/html,<p>x</p>", NULL);
    check_origin ("javascript:alert(1)", NULL);
    check_origin ("example.com", NULL);
    check_origin ("", NULL);
    check_origin (NULL, NULL);

    s = uc_request_origin_format ("https", "example.com", 443);
    g_assert_cmpstr (s, ==, "https://example.com");
    g_free (s);
    s = uc_request_origin_format ("http", "proxy.lan", 3128);
    g_assert_cmpstr (s, ==, "http://proxy.lan:3128");
    g_free (s);
    s = uc_request_origin_format ("ftp", "files", 21);
    g_assert_cmpstr (s, ==, "ftp://files");
    g_free (s);
    s = uc_request_origin_format ("socks", "fe80::1", 1080);
    g_assert_cmpstr (s, ==, "socks://[fe80::1]:1080");
    g_free (s);
    s = uc_request_origin_format ("https", "example.com", 0);
    g_assert_cmpstr (s, ==, "https://example.com");
    g_free (s);
    g_assert_null (uc_request_origin_format ("https", "", 443));
    g_assert_null (uc_request_origin_format (NULL, "example.com", 443));
    g_assert_null (uc_request_origin_format ("https", NULL, 443));
}

static void
test_new_window (void)
{
    g_assert_cmpint (uc_request_new_window_verdict ("https://example.com/login", NULL), ==, UC_NEW_WINDOW_LOAD);
    g_assert_cmpint (uc_request_new_window_verdict ("http://127.0.0.1:8080/x", "GET"), ==, UC_NEW_WINDOW_LOAD);
    g_assert_cmpint (uc_request_new_window_verdict ("HTTPS://EXAMPLE.COM/", "get"), ==, UC_NEW_WINDOW_LOAD);
    /* a form posted to a new window */
    g_assert_cmpint (uc_request_new_window_verdict ("https://example.com/pay", "POST"), ==, UC_NEW_WINDOW_REFUSED_METHOD);
    /* every scheme but http(s), and what does not parse */
    g_assert_cmpint (uc_request_new_window_verdict ("data:text/html,<title>x</title>", NULL), ==,
                     UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict ("javascript:alert(1)", NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict ("blob:https://example.com/u", NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict ("file:///etc/passwd", NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict ("about:blank", NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict ("ftp://example.com/", NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict ("https:///nohost", NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict ("", NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
    g_assert_cmpint (uc_request_new_window_verdict (NULL, NULL), ==, UC_NEW_WINDOW_REFUSED_SCHEME);
}

static void
test_attachment (void)
{
    g_assert_true (uc_request_is_attachment ("attachment"));
    g_assert_true (uc_request_is_attachment ("attachment; filename=\"a.txt\""));
    g_assert_true (uc_request_is_attachment ("Attachment;filename=a.txt"));
    g_assert_true (uc_request_is_attachment ("  ATTACHMENT  ; x"));
    g_assert_true (uc_request_is_attachment ("\tattachment\r\n"));
    g_assert_false (uc_request_is_attachment ("inline"));
    g_assert_false (uc_request_is_attachment ("inline; filename=attachment"));
    g_assert_false (uc_request_is_attachment ("attachments"));
    g_assert_false (uc_request_is_attachment ("attach ment"));
    g_assert_false (uc_request_is_attachment ("filename=a.txt; attachment"));
    g_assert_false (uc_request_is_attachment (""));
    g_assert_false (uc_request_is_attachment (NULL));
}

static void
test_response (void)
{
    /* viewer off (production) */
    g_assert_cmpint (uc_request_response_verdict ("text/html", NULL, TRUE, 200, FALSE), ==, UC_RESPONSE_SHOW);
    g_assert_cmpint (uc_request_response_verdict ("text/plain", "attachment", TRUE, 200, FALSE), ==, UC_RESPONSE_DOWNLOAD);
    g_assert_cmpint (uc_request_response_verdict ("application/pdf", "attachment; filename=x.pdf", FALSE, 200, FALSE), ==,
                     UC_RESPONSE_DOWNLOAD);   /* an attachment is a download whatever its type, as in WebKit */
    g_assert_cmpint (uc_request_response_verdict ("application/pdf", NULL, FALSE, 200, FALSE), ==, UC_RESPONSE_PDF);
    g_assert_cmpint (uc_request_response_verdict ("text/pdf", "inline", TRUE, 200, FALSE), ==, UC_RESPONSE_PDF);
    g_assert_cmpint (uc_request_response_verdict ("application/x-unknown", NULL, FALSE, 200, FALSE), ==,
                     UC_RESPONSE_UNSUPPORTED);
    g_assert_cmpint (uc_request_response_verdict (NULL, NULL, FALSE, 200, FALSE), ==, UC_RESPONSE_UNSUPPORTED);
    g_assert_cmpint (uc_request_response_verdict ("application/x-unknown", NULL, FALSE, 204, FALSE), ==,
                     UC_RESPONSE_SHOW);       /* WebKit uses a 204 whatever its type */
    /* UC_WEBVIEW_PDF_VIEWER=1: a PDF is WebKit's (shown by the viewer); the rest as before */
    g_assert_cmpint (uc_request_response_verdict ("application/pdf", NULL, TRUE, 200, TRUE), ==, UC_RESPONSE_SHOW);
    g_assert_cmpint (uc_request_response_verdict ("application/pdf", "attachment", TRUE, 200, TRUE), ==,
                     UC_RESPONSE_DOWNLOAD);
}

/* the library's design decisions; a change here goes with the README's list */
static void
test_decisions (void)
{
    g_assert_cmpint (uc_request_decisions.before_unload_without_callback, ==, UC_BEFORE_UNLOAD_LEAVE);
    g_assert_cmpint (uc_request_decisions.auth_without_callback, ==, UC_AUTH_WITHOUT_CREDENTIALS);
    g_assert_cmpint (uc_request_decisions.client_certificate, ==, UC_AUTH_WITHOUT_CREDENTIALS);
    g_assert_false (uc_request_decisions.before_unload_message);
    g_assert_cmpuint (uc_request_decisions.guard_quick_dialogs, ==, 3);
    g_assert_cmpint (uc_request_decisions.guard_window_us, ==, G_USEC_PER_SEC);
    g_assert_true (uc_request_decisions.guard_ends_on_input);
    g_assert_false (uc_request_decisions.report_policy_interruption);
    g_assert_true (uc_request_decisions.pdf_as_unsupported);
    g_assert_cmpint (uc_request_decisions.navigation_quiet_us, ==, 2 * G_USEC_PER_SEC);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/requests/ids", test_ids);
    g_test_add_func ("/requests/steal-kind", test_steal_kind);
    g_test_add_func ("/requests/free-nonempty", test_free_nonempty);
    g_test_add_func ("/requests/check-reply", test_check_reply);
    g_test_add_func ("/requests/end-paths", test_end_paths);
    g_test_add_func ("/requests/dialog-answer", test_dialog_answer);
    g_test_add_func ("/requests/guard/loop", test_guard_loop);
    g_test_add_func ("/requests/guard/window", test_guard_window);
    g_test_add_func ("/requests/guard/beforeunload", test_guard_before_unload);
    g_test_add_func ("/requests/guard/user-input", test_guard_user_input);
    g_test_add_func ("/requests/origin", test_origin);
    g_test_add_func ("/requests/new-window", test_new_window);
    g_test_add_func ("/requests/attachment", test_attachment);
    g_test_add_func ("/requests/response", test_response);
    g_test_add_func ("/requests/decisions", test_decisions);
    return g_test_run ();
}
