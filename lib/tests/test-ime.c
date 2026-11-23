/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_ime_state.c (GLib only): run with `make check`.
 * Records the signal sequence the state machine emits and compares it with
 * what a WPEInputMethodContext consumer (WebKitInputMethodContextImplWPE.cpp)
 * expects.
 */
#include "uc_ime_state.h"

#include <string.h>

static void
record (UcImeSignal signal, const char *text, int offset, unsigned count, gpointer user_data)
{
    GString *log = user_data;
    switch (signal) {
    case UC_IME_SIGNAL_PREEDIT_STARTED:    g_string_append (log, "start;"); break;
    case UC_IME_SIGNAL_PREEDIT_CHANGED:    g_string_append (log, "changed;"); break;
    case UC_IME_SIGNAL_PREEDIT_FINISHED:   g_string_append (log, "finished;"); break;
    case UC_IME_SIGNAL_COMMITTED:          g_string_append_printf (log, "commit(%s);", text); break;
    case UC_IME_SIGNAL_DELETE_SURROUNDING: g_string_append_printf (log, "delete(%d,%u);", offset, count); break;
    }
}

static void
test_unfocused_ignored (void)
{
    GString *log = g_string_new (NULL);
    UcImeState state;
    uc_ime_state_init (&state, record, log);

    g_assert_false (uc_ime_state_commit (&state, "a"));
    g_assert_false (uc_ime_state_preedit (&state, "a", 1));
    g_assert_false (uc_ime_state_delete_surrounding (&state, -1, 1));
    g_assert_cmpstr (log->str, ==, "");
    g_assert_cmpstr (uc_ime_state_preedit_text (&state, NULL), ==, "");
    uc_ime_state_clear (&state);
    g_string_free (log, TRUE);
}

static void
test_commit (void)
{
    GString *log = g_string_new (NULL);
    UcImeState state;
    uc_ime_state_init (&state, record, log);
    uc_ime_state_set_focused (&state, TRUE);

    g_assert_true (uc_ime_state_commit (&state, "abc"));
    g_assert_true (uc_ime_state_commit (&state, "")); /* nothing emitted */
    g_assert_true (uc_ime_state_commit (&state, "\xc3\xa4")); /* 'ä' */
    g_assert_false (uc_ime_state_commit (&state, "\xff")); /* invalid UTF-8 */
    g_assert_false (uc_ime_state_commit (&state, NULL));
    g_assert_cmpstr (log->str, ==, "commit(abc);commit(\xc3\xa4);");
    uc_ime_state_clear (&state);
    g_string_free (log, TRUE);
}

static void
test_preedit_sequence (void)
{
    GString *log = g_string_new (NULL);
    UcImeState state;
    guint cursor = 99;
    uc_ime_state_init (&state, record, log);
    uc_ime_state_set_focused (&state, TRUE);

    g_assert_true (uc_ime_state_preedit (&state, "x", 1));
    g_assert_cmpstr (uc_ime_state_preedit_text (&state, &cursor), ==, "x");
    g_assert_cmpuint (cursor, ==, 1);
    g_assert_true (uc_ime_state_preedit (&state, "xy", 7)); /* cursor beyond the text is clamped */
    g_assert_cmpstr (uc_ime_state_preedit_text (&state, &cursor), ==, "xy");
    g_assert_cmpuint (cursor, ==, 2);
    g_assert_true (uc_ime_state_preedit (&state, "xy", -1));
    g_assert_cmpuint (cursor, ==, 2);
    g_assert_true (uc_ime_state_preedit (&state, "", 0)); /* ends */
    g_assert_cmpstr (uc_ime_state_preedit_text (&state, &cursor), ==, "");
    g_assert_cmpuint (cursor, ==, 0);
    g_assert_true (uc_ime_state_preedit (&state, NULL, 0)); /* ending twice emits nothing */
    g_assert_cmpstr (log->str, ==, "start;changed;changed;changed;changed;finished;");
    uc_ime_state_clear (&state);
    g_string_free (log, TRUE);
}

static void
test_commit_ends_preedit (void)
{
    GString *log = g_string_new (NULL);
    UcImeState state;
    uc_ime_state_init (&state, record, log);
    uc_ime_state_set_focused (&state, TRUE);

    uc_ime_state_preedit (&state, "ab", 2);
    uc_ime_state_commit (&state, "abc");
    g_assert_cmpstr (log->str, ==, "start;changed;changed;finished;commit(abc);");
    g_string_truncate (log, 0);

    /* delete_surrounding also ends a pre-edit; n <= 0 is rejected */
    uc_ime_state_preedit (&state, "q", 1);
    g_assert_false (uc_ime_state_delete_surrounding (&state, -1, 0));
    g_assert_true (uc_ime_state_delete_surrounding (&state, -1, 1));
    g_assert_cmpstr (log->str, ==, "start;changed;changed;finished;delete(-1,1);");
    g_string_truncate (log, 0);

    /* WebKit reset during a pre-edit: changed("") + finished; without one: nothing */
    uc_ime_state_preedit (&state, "r", 1);
    uc_ime_state_reset (&state);
    uc_ime_state_reset (&state);
    g_assert_cmpstr (log->str, ==, "start;changed;changed;finished;");
    g_string_truncate (log, 0);

    /* focus out drops the pre-edit silently (WebKit cancelled the composition itself) */
    uc_ime_state_preedit (&state, "s", 1);
    uc_ime_state_set_focused (&state, FALSE);
    g_assert_cmpstr (uc_ime_state_preedit_text (&state, NULL), ==, "");
    g_assert_cmpstr (log->str, ==, "start;changed;");
    uc_ime_state_clear (&state);
    g_string_free (log, TRUE);
}

/* ---- focus reports (library 0.7.5) -------------------------------------------------------------------------------
 * A model of what drives the state machine in the library: WebKit's InputMethodFilter (its state, setState() and the
 * view's focus changes, InputMethodFilter.cpp:91-102, WPEWebViewPlatform.cpp:104-114) and UcWpeIme (the context's
 * hints and purpose, the stale marker, the scheduled idle). The log has the host's reports. */

#define MARKER (1u << 30)

typedef struct {
    UcImeState state;
    GString *log;
    gboolean wk_state;      /* the filter has a state (an element is focused as far as WebKit knows) */
    gboolean view_focused;
    unsigned hints, purpose;
    gboolean idle;          /* the scheduled idle is pending */
} Model;

static void
model_init (Model *m)
{
    memset (m, 0, sizeof *m);
    m->log = g_string_new (NULL);
    uc_ime_state_init (&m->state, record, m->log);
    m->view_focused = TRUE;     /* the library focuses a new view */
}

static void
model_clear (Model *m)
{
    uc_ime_state_clear (&m->state);
    g_string_free (m->log, TRUE);
}

static void
model_apply (Model *m, UcImeFocusAction action)
{
    switch (action) {
    case UC_IME_FOCUS_KEEP:
        break;
    case UC_IME_FOCUS_REPORT:
        if (m->state.focused)
            g_string_append_printf (m->log, "in(%u,%u);", m->hints, m->purpose);
        else
            g_string_append (m->log, "out;");
        break;
    case UC_IME_FOCUS_SCHEDULE:
        m->idle = TRUE;
        break;
    }
}

/* setState(): focus_out for element -> none, the content type (WebKit sets the purpose, then the hints; GLib delivers
 * the hints change first, each only when the value changed), focus_in for none -> element in a focused view */
static void
model_set_state (Model *m, gboolean element, unsigned hints, unsigned purpose)
{
    gboolean changed = element != m->wk_state;
    if (changed && !element)
        model_apply (m, uc_ime_state_focus_out (&m->state));
    m->wk_state = element;
    if (element) {
        gboolean purpose_changed = purpose != m->purpose;
        if (hints != m->hints) {
            m->hints = hints;
            model_apply (m, uc_ime_state_content_type (&m->state, TRUE));
        }
        m->purpose = purpose;
        if (purpose_changed)
            model_apply (m, uc_ime_state_content_type (&m->state, FALSE));
    }
    if (changed && element && m->view_focused)
        model_apply (m, uc_ime_state_focus_in (&m->state));
}

/* the view's has-focus: notifyFocusedIn/Out while the filter has a state */
static void
model_view_focus (Model *m, gboolean focused)
{
    if (focused == m->view_focused)
        return;
    m->view_focused = focused;
    if (m->wk_state)
        model_apply (m, focused ? uc_ime_state_focus_in (&m->state) : uc_ime_state_focus_out (&m->state));
}

/* a process swap or termination: WebKit keeps its state, the context reports and marks its hints */
static void
model_gone (Model *m)
{
    model_apply (m, uc_ime_state_web_process_gone (&m->state));
    m->hints = MARKER;
}

static void
model_idle (Model *m)
{
    if (!m->idle)
        return;
    m->idle = FALSE;
    model_apply (m, uc_ime_state_scheduled_focus (&m->state, m->view_focused));
}

static void
test_focus_plain (void)
{
    Model m;
    model_init (&m);
    /* focus, the view's focus out and back (a replay of a field that still has the focus), blur */
    model_set_state (&m, TRUE, 0x81, 8);
    model_view_focus (&m, FALSE);
    model_view_focus (&m, TRUE);
    model_set_state (&m, FALSE, 0, 0);
    model_view_focus (&m, FALSE);
    model_view_focus (&m, TRUE);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(129,8);out;in(129,8);out;");
    g_assert_false (m.idle);
    /* the focus moving between inputs of one document: one report per changed property (unchanged since 0.1) */
    g_string_truncate (m.log, 0);
    model_set_state (&m, TRUE, 0x80, 0);
    model_set_state (&m, TRUE, 0x81, 6);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);in(129,0);in(129,6);");
    model_clear (&m);
}

static void
test_focus_gone_replay (void)
{
    Model m;
    model_init (&m);
    model_set_state (&m, TRUE, 0x80, 0);
    model_gone (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;");
    g_assert_false (uc_ime_state_commit (&m.state, "a"));     /* no element takes text */
    /* WebKit replays the gone element with every focus of the view: not reported */
    model_view_focus (&m, FALSE);
    model_view_focus (&m, TRUE);
    model_view_focus (&m, FALSE);
    model_view_focus (&m, TRUE);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;");
    g_assert_true (m.state.stale);
    model_clear (&m);
}

static void
test_focus_gone_same_state (void)
{
    Model m;
    model_init (&m);
    model_set_state (&m, TRUE, 0x80, 0);
    model_gone (&m);
    /* the new document focuses an element of the same state: element -> element, nothing but the marker changes */
    model_set_state (&m, TRUE, 0x80, 0);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;");
    g_assert_true (m.idle);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;in(128,0);");
    g_assert_false (m.state.stale);
    g_assert_true (uc_ime_state_commit (&m.state, "a"));
    /* WebKit is in step again: the blur comes, a view focus cycle has nothing to replay */
    model_set_state (&m, FALSE, 0, 0);
    model_view_focus (&m, FALSE);
    model_view_focus (&m, TRUE);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;in(128,0);commit(a);out;");
    model_clear (&m);
}

static void
test_focus_gone_new_purpose (void)
{
    Model m;
    model_init (&m);
    model_set_state (&m, TRUE, 0x80, 2);
    model_gone (&m);
    /* hints and purpose change: one report after the update, with both new values */
    model_set_state (&m, TRUE, 0x81, 6);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,2);out;in(129,6);");
    model_clear (&m);
}

static void
test_focus_gone_view_unfocused (void)
{
    Model m;
    model_init (&m);
    model_set_state (&m, TRUE, 0x80, 0);
    model_gone (&m);
    model_view_focus (&m, FALSE);
    /* the new document focuses an element while the view has no focus: reported when the view gets it */
    model_set_state (&m, TRUE, 0x81, 0);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;");
    model_view_focus (&m, TRUE);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;in(129,0);");
    model_clear (&m);
}

static void
test_focus_gone_blur_before_idle (void)
{
    Model m;
    model_init (&m);
    model_set_state (&m, TRUE, 0x80, 0);
    model_gone (&m);
    /* focus and blur in one batch of messages before the idle: nothing to report */
    model_set_state (&m, TRUE, 0x80, 0);
    model_set_state (&m, FALSE, 0, 0);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;");
    /* a view focus right after a schedule reports the element at once; the idle then has nothing to do */
    model_gone (&m);
    model_view_focus (&m, FALSE);
    model_set_state (&m, TRUE, 0x80, 0);
    model_view_focus (&m, TRUE);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;in(128,0);");
    model_clear (&m);
}

static void
test_focus_gone_without_element (void)
{
    Model m;
    model_init (&m);
    /* a swap without a focused element, twice: no report; the next focus is reported once, by WebKit's focus_in */
    model_gone (&m);
    model_gone (&m);
    model_set_state (&m, TRUE, 0x80, 0);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);");
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);");
    model_clear (&m);
}

static void
test_focus_gone_restored_page (void)
{
    Model m;
    model_init (&m);
    model_set_state (&m, TRUE, 0x80, 0);
    model_gone (&m);
    /* back to a page in another process that still had a focused element: its commit sends "none" */
    model_set_state (&m, FALSE, 0, 0);
    model_view_focus (&m, FALSE);
    model_view_focus (&m, TRUE);
    model_set_state (&m, TRUE, 0x81, 0);
    model_idle (&m);
    g_assert_cmpstr (m.log->str, ==, "in(128,0);out;in(129,0);");
    model_clear (&m);
}

static void
test_focus_gone_preedit (void)
{
    Model m;
    model_init (&m);
    model_set_state (&m, TRUE, 0x80, 0);
    g_assert_true (uc_ime_state_preedit (&m.state, "s", 1));
    /* the composition belonged to the gone document: dropped without signals to the new one */
    model_gone (&m);
    g_assert_cmpstr (uc_ime_state_preedit_text (&m.state, NULL), ==, "");
    g_assert_false (uc_ime_state_preedit (&m.state, "t", 1));
    g_assert_cmpstr (m.log->str, ==, "in(128,0);start;changed;out;");
    model_clear (&m);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/ime/unfocused-ignored", test_unfocused_ignored);
    g_test_add_func ("/ime/commit", test_commit);
    g_test_add_func ("/ime/preedit-sequence", test_preedit_sequence);
    g_test_add_func ("/ime/commit-ends-preedit", test_commit_ends_preedit);
    g_test_add_func ("/ime/focus/plain", test_focus_plain);
    g_test_add_func ("/ime/focus/gone-replay", test_focus_gone_replay);
    g_test_add_func ("/ime/focus/gone-same-state", test_focus_gone_same_state);
    g_test_add_func ("/ime/focus/gone-new-purpose", test_focus_gone_new_purpose);
    g_test_add_func ("/ime/focus/gone-view-unfocused", test_focus_gone_view_unfocused);
    g_test_add_func ("/ime/focus/gone-blur-before-idle", test_focus_gone_blur_before_idle);
    g_test_add_func ("/ime/focus/gone-without-element", test_focus_gone_without_element);
    g_test_add_func ("/ime/focus/gone-restored-page", test_focus_gone_restored_page);
    g_test_add_func ("/ime/focus/gone-preedit", test_focus_gone_preedit);
    return g_test_run ();
}
