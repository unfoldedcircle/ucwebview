/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_rendered.c (GLib only): the requests of uc_webview_when_rendered() end exactly once, as
 * DONE after the rendering update and a frame, TIMEOUT or CANCELLED. Timeouts are short here; the library uses
 * 60 s (15 s up to 0.5.x) and a 100 ms grace for the frame. Run with `make check`.
 */
#include "uc_rendered.h"
#include "uc_webview.h"

#include <string.h>

typedef struct {
    UcRenderedResult result;
    gpointer data;
    UcRenderedFunc func;
} Finished;

typedef struct {
    GArray *finished;           /* Finished */
    UcRendered *rendered;       /* for the finish functions that add or end requests */
    gboolean add_on_finish;     /* the finish function adds one request (a host that applies a coalesced zoom) */
    guint added_id;
    gboolean in_frame_call;     /* inside uc_rendered_frame(): nothing may finish there */
} Fixture;

static void
callback_a (void)
{
}

static void
callback_b (void)
{
}

static void
on_finish (UcRenderedFunc func, gpointer data, const UcRenderedResult *result, gpointer user_data)
{
    Fixture *fx = user_data;
    Finished f;

    g_assert_false (fx->in_frame_call);
    f.result = *result;
    f.data = data;
    f.func = func;
    g_array_append_val (fx->finished, f);
    if (fx->add_on_finish) {
        fx->add_on_finish = FALSE;
        fx->added_id = uc_rendered_add (fx->rendered, (UcRenderedFunc) callback_b, GINT_TO_POINTER (99));
    }
}

static void
fixture_init (Fixture *fx, guint timeout_ms, guint grace_ms)
{
    fx->finished = g_array_new (FALSE, TRUE, sizeof (Finished));
    fx->rendered = uc_rendered_new (timeout_ms, grace_ms, on_finish, fx);
    fx->add_on_finish = FALSE;
    fx->added_id = 0;
    fx->in_frame_call = FALSE;
}

static void
fixture_clear (Fixture *fx)
{
    uc_rendered_free (fx->rendered, "test end");
    g_array_unref (fx->finished);
}

static Finished *
finished (Fixture *fx, guint i)
{
    g_assert_cmpuint (i, <, fx->finished->len);
    return &g_array_index (fx->finished, Finished, i);
}

static void
frame (Fixture *fx)
{
    fx->in_frame_call = TRUE;
    uc_rendered_frame (fx->rendered);
    fx->in_frame_call = FALSE;
}

static gboolean
quit_loop (gpointer user_data)
{
    g_main_loop_quit (user_data);
    return G_SOURCE_REMOVE;
}

/* runs the default main context for ms milliseconds */
static void
run_for (guint ms)
{
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);
    g_timeout_add (ms, quit_loop, loop);
    g_main_loop_run (loop);
    g_main_loop_unref (loop);
}

static void
iterate_pending (void)
{
    while (g_main_context_iteration (NULL, FALSE))
        ;
}

static void
test_status_values (void)
{
    g_assert_cmpint (UC_RENDERED_DONE, ==, UC_WEBVIEW_RENDERED_DONE);
    g_assert_cmpint (UC_RENDERED_TIMEOUT, ==, UC_WEBVIEW_RENDERED_TIMEOUT);
    g_assert_cmpint (UC_RENDERED_CANCELLED, ==, UC_WEBVIEW_RENDERED_CANCELLED);
}

/* the usual case on a page at rest: the update is reported, the frame with the new layout follows */
static void
test_done_frame_after_update (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 5000, 1000);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, GINT_TO_POINTER (7));
    g_assert_cmpuint (id, >, 0);
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 0);         /* waits for the frame */
    frame (&fx);
    g_assert_cmpuint (fx.finished->len, ==, 0);         /* not inside the frame call */
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpuint (finished (&fx, 0)->result.id, ==, id);
    g_assert_cmpint (finished (&fx, 0)->result.status, ==, UC_RENDERED_DONE);
    g_assert_cmpint (finished (&fx, 0)->result.frame, ==, UC_RENDERED_FRAME_AFTER_UPDATE);
    g_assert_cmpint (finished (&fx, 0)->result.update_us, >=, 0);
    g_assert_cmpint (finished (&fx, 0)->result.elapsed_us, >=, finished (&fx, 0)->result.update_us);
    g_assert_null (finished (&fx, 0)->result.reason);
    g_assert_true (finished (&fx, 0)->func == (UcRenderedFunc) callback_a);
    g_assert_true (finished (&fx, 0)->data == GINT_TO_POINTER (7));
    g_assert_cmpuint (uc_rendered_pending (fx.rendered), ==, 0);
    /* exactly once: neither the update nor a cancel nor the timeout reach it again */
    g_assert_false (uc_rendered_update_done (fx.rendered, id));
    g_assert_false (uc_rendered_cancel (fx.rendered, id, "late"));
    fixture_clear (&fx);
}

/* the frame was there before the update was reported (fast GPU, or an animated page): done at the report */
static void
test_done_frame_before_update (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 5000, 1000);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    frame (&fx);
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 0);         /* a frame alone is not the update */
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    g_assert_cmpuint (fx.finished->len, ==, 1);         /* inside the report */
    g_assert_cmpint (finished (&fx, 0)->result.status, ==, UC_RENDERED_DONE);
    g_assert_cmpint (finished (&fx, 0)->result.frame, ==, UC_RENDERED_FRAME_BEFORE_UPDATE);
    fixture_clear (&fx);
}

/* frames before the request do not count */
static void
test_frames_before_the_request_do_not_count (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 5000, 1000);
    frame (&fx);
    frame (&fx);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    g_assert_cmpuint (fx.finished->len, ==, 0);
    frame (&fx);
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpint (finished (&fx, 0)->result.frame, ==, UC_RENDERED_FRAME_AFTER_UPDATE);
    fixture_clear (&fx);
}

/* an update that changed nothing visible: no frame comes, done after the grace period */
static void
test_done_without_a_frame (void)
{
    Fixture fx;
    guint id;
    gint64 start = g_get_monotonic_time ();

    fixture_init (&fx, 5000, 40);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    run_for (20);
    g_assert_cmpuint (fx.finished->len, ==, 0);
    run_for (60);
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpint (finished (&fx, 0)->result.status, ==, UC_RENDERED_DONE);
    g_assert_cmpint (finished (&fx, 0)->result.frame, ==, UC_RENDERED_NO_FRAME);
    g_assert_cmpint (finished (&fx, 0)->result.elapsed_us, >=, 40000);
    g_assert_cmpint (g_get_monotonic_time () - start, >=, 40000);
    /* a late frame changes nothing */
    frame (&fx);
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 1);
    fixture_clear (&fx);
}

static void
test_grace_zero (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 5000, 0);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpint (finished (&fx, 0)->result.frame, ==, UC_RENDERED_NO_FRAME);
    fixture_clear (&fx);
}

/* no update within the timeout (view unmapped, page hung): TIMEOUT, and a late update is ignored */
static void
test_timeout (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 50, 1000);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, GINT_TO_POINTER (3));
    frame (&fx);    /* frames without the update do not make it done */
    run_for (30);
    g_assert_cmpuint (fx.finished->len, ==, 0);
    run_for (60);
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpint (finished (&fx, 0)->result.status, ==, UC_RENDERED_TIMEOUT);
    g_assert_cmpint (finished (&fx, 0)->result.update_us, ==, -1);
    g_assert_cmpint (finished (&fx, 0)->result.elapsed_us, >=, 50000);
    g_assert_true (finished (&fx, 0)->data == GINT_TO_POINTER (3));
    g_assert_false (uc_rendered_update_done (fx.rendered, id));
    g_assert_cmpuint (uc_rendered_pending (fx.rendered), ==, 0);
    fixture_clear (&fx);
}

/* the timeout also ends a request whose update came but no frame within a grace longer than the rest */
static void
test_timeout_while_waiting_for_the_frame (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 50, 1000);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    run_for (90);
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpint (finished (&fx, 0)->result.status, ==, UC_RENDERED_TIMEOUT);
    g_assert_cmpint (finished (&fx, 0)->result.update_us, >=, 0);
    run_for (20);
    g_assert_cmpuint (fx.finished->len, ==, 1);
    fixture_clear (&fx);
}

/* the script did not run: one request ends, the others stay */
static void
test_cancel_one (void)
{
    Fixture fx;
    guint a, b;

    fixture_init (&fx, 5000, 1000);
    a = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, GINT_TO_POINTER (1));
    b = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_b, GINT_TO_POINTER (2));
    g_assert_cmpuint (a, !=, b);
    g_assert_true (uc_rendered_cancel (fx.rendered, a, "script refused"));
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpint (finished (&fx, 0)->result.status, ==, UC_RENDERED_CANCELLED);
    g_assert_cmpstr (finished (&fx, 0)->result.reason, ==, "script refused");
    g_assert_true (finished (&fx, 0)->data == GINT_TO_POINTER (1));
    g_assert_cmpuint (uc_rendered_pending (fx.rendered), ==, 1);
    g_assert_false (uc_rendered_cancel (fx.rendered, a, "again"));
    g_assert_false (uc_rendered_update_done (fx.rendered, 4242));   /* never issued */
    frame (&fx);
    g_assert_true (uc_rendered_update_done (fx.rendered, b));
    g_assert_cmpuint (fx.finished->len, ==, 2);
    g_assert_cmpint (finished (&fx, 1)->result.status, ==, UC_RENDERED_DONE);
    fixture_clear (&fx);
}

/* a navigation or a web process crash ends every pending request, in request order */
static void
test_cancel_all (void)
{
    Fixture fx;
    guint ids[3];

    fixture_init (&fx, 5000, 1000);
    for (int i = 0; i < 3; i++)
        ids[i] = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, GINT_TO_POINTER (i));
    g_assert_true (uc_rendered_update_done (fx.rendered, ids[1]));   /* one of them waits for its frame */
    uc_rendered_cancel_all (fx.rendered, "navigation");
    g_assert_cmpuint (fx.finished->len, ==, 3);
    for (int i = 0; i < 3; i++) {
        g_assert_cmpuint (finished (&fx, (guint) i)->result.id, ==, ids[i]);
        g_assert_cmpint (finished (&fx, (guint) i)->result.status, ==, UC_RENDERED_CANCELLED);
        g_assert_cmpstr (finished (&fx, (guint) i)->result.reason, ==, "navigation");
    }
    g_assert_cmpint (finished (&fx, 1)->result.update_us, >=, 0);
    g_assert_cmpuint (uc_rendered_pending (fx.rendered), ==, 0);
    /* their timers and the grace timer are gone */
    frame (&fx);
    run_for (30);
    g_assert_cmpuint (fx.finished->len, ==, 3);
    fixture_clear (&fx);
}

/* a host that issues the next request from the callback (remote-ui applies the coalesced zoom): it stays pending */
static void
test_request_from_the_callback_survives_cancel_all (void)
{
    Fixture fx;

    fixture_init (&fx, 5000, 1000);
    uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    fx.add_on_finish = TRUE;
    uc_rendered_cancel_all (fx.rendered, "navigation");
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpuint (fx.added_id, >, 0);
    g_assert_cmpuint (uc_rendered_pending (fx.rendered), ==, 1);
    frame (&fx);
    g_assert_true (uc_rendered_update_done (fx.rendered, fx.added_id));
    g_assert_cmpuint (fx.finished->len, ==, 2);
    g_assert_true (finished (&fx, 1)->data == GINT_TO_POINTER (99));
    fixture_clear (&fx);
}

/* the same from the frame idle: the new request waits for its own update */
static void
test_request_from_the_callback_after_a_frame (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 5000, 1000);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    fx.add_on_finish = TRUE;
    frame (&fx);
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpuint (fx.added_id, >, 0);
    g_assert_cmpuint (uc_rendered_pending (fx.rendered), ==, 1);
    /* the frame that ended the first one came before the second request: it does not count for it */
    g_assert_true (uc_rendered_update_done (fx.rendered, fx.added_id));
    g_assert_cmpuint (fx.finished->len, ==, 1);
    frame (&fx);
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 2);
    fixture_clear (&fx);
}

/* uc_webview_free(): every pending request is cancelled inside the call; no new ones are taken meanwhile */
static void
test_free_cancels (void)
{
    Fixture fx;

    fixture_init (&fx, 5000, 1000);
    uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, GINT_TO_POINTER (1));
    uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_b, GINT_TO_POINTER (2));
    fx.add_on_finish = TRUE;
    uc_rendered_free (fx.rendered, "view freed");
    g_assert_cmpuint (fx.finished->len, ==, 2);
    g_assert_cmpstr (finished (&fx, 0)->result.reason, ==, "view freed");
    g_assert_cmpstr (finished (&fx, 1)->result.reason, ==, "view freed");
    g_assert_cmpuint (fx.added_id, ==, 0);      /* refused while freeing */
    /* no timer of the freed queue fires */
    run_for (20);
    g_assert_cmpuint (fx.finished->len, ==, 2);
    g_array_unref (fx.finished);
}

/* a frame idle that is still pending when the queue goes */
static void
test_free_with_a_pending_frame_idle (void)
{
    Fixture fx;
    guint id;

    fixture_init (&fx, 5000, 1000);
    id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
    g_assert_true (uc_rendered_update_done (fx.rendered, id));
    frame (&fx);
    uc_rendered_free (fx.rendered, "view freed");
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpint (finished (&fx, 0)->result.status, ==, UC_RENDERED_CANCELLED);
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_array_unref (fx.finished);
}

static void
test_max_pending (void)
{
    Fixture fx;
    GHashTable *ids = g_hash_table_new (NULL, NULL);
    guint first;

    fixture_init (&fx, 5000, 1000);
    for (int i = 0; i < UC_RENDERED_MAX_PENDING; i++) {
        guint id = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL);
        g_assert_cmpuint (id, >, 0);
        g_assert_false (g_hash_table_contains (ids, GUINT_TO_POINTER (id)));
        g_hash_table_add (ids, GUINT_TO_POINTER (id));
    }
    g_assert_cmpuint (uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL), ==, 0);
    g_assert_cmpuint (fx.finished->len, ==, 0);         /* a refused request calls nothing */
    first = 1;
    g_assert_true (uc_rendered_cancel (fx.rendered, first, "test"));
    g_assert_cmpuint (uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, NULL), >, 0);
    g_hash_table_unref (ids);
    fixture_clear (&fx);
}

/* several requests in flight end independently */
static void
test_independent_requests (void)
{
    Fixture fx;
    guint a, b;

    fixture_init (&fx, 5000, 1000);
    a = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_a, GINT_TO_POINTER (1));
    frame (&fx);
    b = uc_rendered_add (fx.rendered, (UcRenderedFunc) callback_b, GINT_TO_POINTER (2));
    g_assert_true (uc_rendered_update_done (fx.rendered, b));   /* b's frame did not come yet */
    g_assert_true (uc_rendered_update_done (fx.rendered, a));   /* a had a frame */
    g_assert_cmpuint (fx.finished->len, ==, 1);
    g_assert_cmpuint (finished (&fx, 0)->result.id, ==, a);
    frame (&fx);
    iterate_pending ();
    g_assert_cmpuint (fx.finished->len, ==, 2);
    g_assert_cmpuint (finished (&fx, 1)->result.id, ==, b);
    g_assert_cmpint (finished (&fx, 1)->result.frame, ==, UC_RENDERED_FRAME_AFTER_UPDATE);
    fixture_clear (&fx);
}

/* the probe (0.9.0: run by the web process extension without a user gesture, no MessageChannel hop) */
static void
test_probe_script (void)
{
    char *script = uc_rendered_probe_script (4294967295u);

    g_assert_true (g_str_has_prefix (script, "(function (id) {"));
    g_assert_true (g_str_has_suffix (script, "}) (4294967295)"));
    g_assert_nonnull (strstr (script, "requestAnimationFrame (function () { requestAnimationFrame (function () {"));
    g_assert_nonnull (strstr (script, "window.webkit.messageHandlers." UC_RENDERED_HANDLER ".postMessage (id)"));
    /* it returns at once: nothing keeps the request's message waiting */
    g_assert_nonnull (strstr (script, " return id;}"));
    g_assert_null (strstr (script, "MessageChannel"));
    g_free (script);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/rendered/status-values", test_status_values);
    g_test_add_func ("/rendered/done/frame-after-update", test_done_frame_after_update);
    g_test_add_func ("/rendered/done/frame-before-update", test_done_frame_before_update);
    g_test_add_func ("/rendered/done/frames-before-the-request", test_frames_before_the_request_do_not_count);
    g_test_add_func ("/rendered/done/without-a-frame", test_done_without_a_frame);
    g_test_add_func ("/rendered/done/grace-zero", test_grace_zero);
    g_test_add_func ("/rendered/timeout", test_timeout);
    g_test_add_func ("/rendered/timeout/waiting-for-the-frame", test_timeout_while_waiting_for_the_frame);
    g_test_add_func ("/rendered/cancel/one", test_cancel_one);
    g_test_add_func ("/rendered/cancel/all", test_cancel_all);
    g_test_add_func ("/rendered/cancel/request-from-the-callback", test_request_from_the_callback_survives_cancel_all);
    g_test_add_func ("/rendered/done/request-from-the-callback", test_request_from_the_callback_after_a_frame);
    g_test_add_func ("/rendered/free/cancels", test_free_cancels);
    g_test_add_func ("/rendered/free/pending-frame-idle", test_free_with_a_pending_frame_idle);
    g_test_add_func ("/rendered/max-pending", test_max_pending);
    g_test_add_func ("/rendered/independent", test_independent_requests);
    g_test_add_func ("/rendered/probe-script", test_probe_script);
    return g_test_run ();
}
