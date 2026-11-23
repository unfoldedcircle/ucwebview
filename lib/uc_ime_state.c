/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 */
#include "uc_ime_state.h"

static void
emit (UcImeState *state, UcImeSignal signal, const char *text, int offset, unsigned count)
{
    if (state->emit != NULL)
        state->emit (signal, text, offset, count, state->user_data);
}

void
uc_ime_state_init (UcImeState *state, UcImeEmit emit_func, gpointer user_data)
{
    state->emit = emit_func;
    state->user_data = user_data;
    state->focused = FALSE;
    state->preedit_active = FALSE;
    state->preedit = g_string_new (NULL);
    state->cursor = 0;
    state->stale = FALSE;
    state->scheduled = FALSE;
}

void
uc_ime_state_clear (UcImeState *state)
{
    if (state->preedit != NULL)
        g_string_free (state->preedit, TRUE);
    state->preedit = NULL;
    state->preedit_active = FALSE;
    state->focused = FALSE;
    state->stale = FALSE;
    state->scheduled = FALSE;
}

void
uc_ime_state_set_focused (UcImeState *state, gboolean focused)
{
    state->focused = focused;
    if (!focused) {
        g_string_truncate (state->preedit, 0);
        state->cursor = 0;
        state->preedit_active = FALSE;
    }
}

static void
end_preedit (UcImeState *state)
{
    if (!state->preedit_active)
        return;
    g_string_truncate (state->preedit, 0);
    state->cursor = 0;
    /* GTK IM semantics: the last preedit-changed reports the empty string, then preedit-end */
    emit (state, UC_IME_SIGNAL_PREEDIT_CHANGED, NULL, 0, 0);
    state->preedit_active = FALSE;
    emit (state, UC_IME_SIGNAL_PREEDIT_FINISHED, NULL, 0, 0);
}

gboolean
uc_ime_state_commit (UcImeState *state, const char *utf8)
{
    if (!state->focused || utf8 == NULL || !g_utf8_validate (utf8, -1, NULL))
        return FALSE;
    end_preedit (state);
    if (*utf8 == '\0')
        return TRUE;
    emit (state, UC_IME_SIGNAL_COMMITTED, utf8, 0, 0);
    return TRUE;
}

gboolean
uc_ime_state_preedit (UcImeState *state, const char *utf8, int cursor)
{
    glong n_chars;

    if (!state->focused)
        return FALSE;
    if (utf8 == NULL || *utf8 == '\0') {
        end_preedit (state);
        return TRUE;
    }
    if (!g_utf8_validate (utf8, -1, NULL))
        return FALSE;

    n_chars = g_utf8_strlen (utf8, -1);
    if (cursor < 0 || cursor > n_chars)
        cursor = (int) n_chars;

    g_string_assign (state->preedit, utf8);
    state->cursor = (guint) cursor;
    if (!state->preedit_active) {
        state->preedit_active = TRUE;
        emit (state, UC_IME_SIGNAL_PREEDIT_STARTED, NULL, 0, 0);
    }
    emit (state, UC_IME_SIGNAL_PREEDIT_CHANGED, NULL, 0, 0);
    return TRUE;
}

gboolean
uc_ime_state_delete_surrounding (UcImeState *state, int offset, int n_chars)
{
    if (!state->focused || n_chars <= 0)
        return FALSE;
    end_preedit (state);
    emit (state, UC_IME_SIGNAL_DELETE_SURROUNDING, NULL, offset, (unsigned) n_chars);
    return TRUE;
}

void
uc_ime_state_reset (UcImeState *state)
{
    end_preedit (state);
}

const char *
uc_ime_state_preedit_text (const UcImeState *state, guint *cursor)
{
    if (cursor != NULL)
        *cursor = state->preedit_active ? state->cursor : 0;
    return state->preedit_active ? state->preedit->str : "";
}

/* ---- focus reports (library 0.7.5, uc_ime_state.h) ------------------------------------------------------------ */

UcImeFocusAction
uc_ime_state_focus_in (UcImeState *state)
{
    state->scheduled = FALSE;   /* WebKit's own report of the new state came first (setState(): none -> element) */
    if (state->stale || state->focused)
        return UC_IME_FOCUS_KEEP;   /* stale: the view's focus replays the gone element's */
    uc_ime_state_set_focused (state, TRUE);
    return UC_IME_FOCUS_REPORT;
}

UcImeFocusAction
uc_ime_state_focus_out (UcImeState *state)
{
    /* the element lost the focus, or the view did (then WebKit replays a focus_in when it is back) */
    state->scheduled = FALSE;
    if (!state->focused)
        return UC_IME_FOCUS_KEEP;
    uc_ime_state_set_focused (state, FALSE);
    return UC_IME_FOCUS_REPORT;
}

UcImeFocusAction
uc_ime_state_web_process_gone (UcImeState *state)
{
    /* Also stale when no element had the focus: harmless, WebKit then has no state to replay, and its next one goes
     * through uc_ime_state_content_type() before WebKit's own focus_in. */
    state->stale = TRUE;
    state->scheduled = FALSE;
    if (!state->focused)
        return UC_IME_FOCUS_KEEP;
    /* the pre-edit belonged to the gone document: dropped silently like at a focus_out */
    uc_ime_state_set_focused (state, FALSE);
    return UC_IME_FOCUS_REPORT;
}

UcImeFocusAction
uc_ime_state_content_type (UcImeState *state, gboolean hints)
{
    if (state->stale) {
        /* The marker makes every new state change the hints; a purpose change of the same update is reported with
         * them (GLib may deliver it before or after the hints). */
        if (!hints)
            return UC_IME_FOCUS_KEEP;
        state->stale = FALSE;
        state->scheduled = TRUE;
        return UC_IME_FOCUS_SCHEDULE;
    }
    /* while focused: the focus moved to an element of another type */
    return state->focused ? UC_IME_FOCUS_REPORT : UC_IME_FOCUS_KEEP;
}

UcImeFocusAction
uc_ime_state_scheduled_focus (UcImeState *state, gboolean view_focused)
{
    if (!state->scheduled)
        return UC_IME_FOCUS_KEEP;   /* a focus_in, focus_out or another web process change came first */
    state->scheduled = FALSE;
    /* without the view's focus WebKit would not call focus_in either; it replays one when the view gets it */
    if (state->focused || !view_focused)
        return UC_IME_FOCUS_KEEP;
    uc_ime_state_set_focused (state, TRUE);
    return UC_IME_FOCUS_REPORT;
}
