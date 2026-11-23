/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * UcWpeIme: the WPEInputMethodContext that connects the page's editable
 * elements to the host's keyboard. WebKit wraps it in a
 * WebKitInputMethodContextImplWPE (WebKitWebView.cpp:1044-1048,
 * WebKitInputMethodContextImplWPE.cpp:188-205) and
 *  - sets input purpose/hints on it (wpe_input_method_context_set_input_purpose/
 *    _hints, WebKitInputMethodContextImplWPE.cpp:60-68) before
 *  - calling focus_in when an editable element gains focus while the view is
 *    focused (InputMethodFilter.cpp:91-102), focus_out when it loses it,
 *    set_cursor_area with the caret rectangle, set_surrounding with the text
 *    around the caret, reset when the page cancels a composition;
 *  - listens to the signals preedit-started/-changed/-finished, committed(text)
 *    and delete-surrounding(offset, count) (WPEInputMethodContext.cpp:281-360)
 *    and reads get_preedit_string for the composition text.
 * The host side of these calls is uc_webview_ime_commit/preedit/
 * delete_surrounding, driven through uc_ime_state.c, which also decides which
 * focus calls reach the host: since 0.7.5 none for an element whose web
 * process is gone (uc_ime_state.h, "Focus reports").
 */
#include "uc_internal.h"

/* The stale marker of uc_wpe_ime_web_process_gone(): a hint bit that neither WebKitInputHints (bits 0-5) nor
 * WPEInputHints (bits 0-11) defines, so no state of a web process maps to it (InputMethodFilter.cpp:174-190,
 * WebKitInputMethodContextImplWPE.cpp:60-76). It never reaches the host: the context reports no focus before WebKit
 * replaced it. */
#define UC_IME_HINT_STALE (1u << 30)

struct _UcWpeIme {
    WPEInputMethodContext parent;
    UcImeState state;           /* state.focused: what the host was told */
    char *surrounding;
    int surrounding_cursor;
    int surrounding_anchor;
    guint scheduled_source;     /* uc_ime_state_scheduled_focus() (0.7.5) */
};

G_DEFINE_FINAL_TYPE (UcWpeIme, uc_wpe_ime, WPE_TYPE_INPUT_METHOD_CONTEXT)

static uc_webview *
ime_owner (UcWpeIme *ime)
{
    WPEView *view = wpe_input_method_context_get_view (WPE_INPUT_METHOD_CONTEXT (ime));
    return UC_IS_WPE_VIEW (view) ? uc_wpe_view_get_owner (UC_WPE_VIEW (view)) : NULL;
}

static void
ime_emit (UcImeSignal signal, const char *text, int offset, unsigned count, gpointer user_data)
{
    UcWpeIme *ime = user_data;
    switch (signal) {
    case UC_IME_SIGNAL_PREEDIT_STARTED:     g_signal_emit_by_name (ime, "preedit-started"); break;
    case UC_IME_SIGNAL_PREEDIT_CHANGED:     g_signal_emit_by_name (ime, "preedit-changed"); break;
    case UC_IME_SIGNAL_PREEDIT_FINISHED:    g_signal_emit_by_name (ime, "preedit-finished"); break;
    case UC_IME_SIGNAL_COMMITTED:           g_signal_emit_by_name (ime, "committed", text); break;
    case UC_IME_SIGNAL_DELETE_SURROUNDING:  g_signal_emit_by_name (ime, "delete-surrounding", offset, count); break;
    }
}

static void
notify_focus (UcWpeIme *ime)
{
    uc_webview *owner = ime_owner (ime);
    WPEInputMethodContext *ctx = WPE_INPUT_METHOD_CONTEXT (ime);
    gboolean focused = ime->state.focused;
    if (owner == NULL)
        return;
    uc_webview_internal_ime_focus (owner, focused,
                                   (unsigned) wpe_input_method_context_get_input_hints (ctx),
                                   (unsigned) wpe_input_method_context_get_input_purpose (ctx),
                                   focused ? ime->surrounding : NULL,
                                   focused ? ime->surrounding_cursor : -1);
}

static gboolean scheduled_focus (gpointer user_data);

static void
apply (UcWpeIme *ime, UcImeFocusAction action)
{
    switch (action) {
    case UC_IME_FOCUS_KEEP:
        break;
    case UC_IME_FOCUS_REPORT:
        if (!ime->state.focused) {
            /* WebKit forgets the text around the caret with the focus too (InputMethodFilter.cpp:211-220) */
            g_clear_pointer (&ime->surrounding, g_free);
            ime->surrounding_cursor = ime->surrounding_anchor = -1;
        }
        notify_focus (ime);
        break;
    case UC_IME_FOCUS_SCHEDULE:
        /* The first state of a new document. WebKit sets the purpose and then the hints of its context with the
         * notifications frozen (InputMethodFilter.cpp:197-200), and GLib delivers them newest first (2.76:
         * g_object_notify_queue_add() prepends): the hints change can come before the purpose's, so the report
         * waits until WebKit's update is done. G_PRIORITY_HIGH: before the next message of the web process, which
         * WebKit dispatches at G_PRIORITY_DEFAULT (RunLoopSourcePriority.h:66-78). */
        if (ime->scheduled_source == 0)
            ime->scheduled_source = g_idle_add_full (G_PRIORITY_HIGH, scheduled_focus, ime, NULL);
        break;
    }
}

static gboolean
scheduled_focus (gpointer user_data)
{
    UcWpeIme *ime = user_data;
    WPEView *view = wpe_input_method_context_get_view (WPE_INPUT_METHOD_CONTEXT (ime));

    ime->scheduled_source = 0;
    apply (ime, uc_ime_state_scheduled_focus (&ime->state, view != NULL && wpe_view_get_has_focus (view)));
    return G_SOURCE_REMOVE;
}

static void
uc_wpe_ime_get_preedit_string (WPEInputMethodContext *context, gchar **text, GList **underlines, guint *cursor_offset)
{
    UcWpeIme *ime = UC_WPE_IME (context);
    guint cursor = 0;
    const char *preedit = uc_ime_state_preedit_text (&ime->state, &cursor);

    if (text != NULL)
        *text = g_strdup (preedit);
    if (underlines != NULL) {
        *underlines = NULL;
        if (*preedit != '\0')
            *underlines = g_list_append (NULL, wpe_input_method_underline_new (0, (guint) g_utf8_strlen (preedit, -1)));
    }
    if (cursor_offset != NULL)
        *cursor_offset = cursor;
}

static gboolean
uc_wpe_ime_filter_key_event (WPEInputMethodContext *context, WPEEvent *event)
{
    (void) context; (void) event;
    return FALSE; /* the host's keyboard sends text through commit/preedit, hardware keys go to the page */
}

static void
uc_wpe_ime_focus_in (WPEInputMethodContext *context)
{
    UcWpeIme *ime = UC_WPE_IME (context);
    if (ime->state.stale)
        g_debug ("ucwebview: the view's focus replays the focus of an element whose web process is gone: not reported");
    apply (ime, uc_ime_state_focus_in (&ime->state));
}

static void
uc_wpe_ime_focus_out (WPEInputMethodContext *context)
{
    UcWpeIme *ime = UC_WPE_IME (context);
    apply (ime, uc_ime_state_focus_out (&ime->state));
}

static void
uc_wpe_ime_set_cursor_area (WPEInputMethodContext *context, int x, int y, int width, int height)
{
    UcWpeIme *ime = UC_WPE_IME (context);
    uc_webview *owner = ime_owner (ime);
    if (owner != NULL)
        uc_webview_internal_ime_cursor_area (owner, x, y, width, height);
}

static int
byte_index_to_chars (const char *text, guint byte_index, guint length)
{
    if (text == NULL)
        return -1;
    if (byte_index > length)
        byte_index = length;
    return (int) g_utf8_strlen (text, byte_index);
}

static void
uc_wpe_ime_set_surrounding (WPEInputMethodContext *context, const gchar *text, guint length, guint cursor_index, guint selection_index)
{
    UcWpeIme *ime = UC_WPE_IME (context);
    uc_webview *owner = ime_owner (ime);

    g_free (ime->surrounding);
    ime->surrounding = text != NULL ? g_strndup (text, length) : NULL;
    /* WebKit passes byte offsets into text (InputMethodFilter.cpp notifySurrounding); the C API uses characters. */
    ime->surrounding_cursor = byte_index_to_chars (ime->surrounding, cursor_index, length);
    ime->surrounding_anchor = byte_index_to_chars (ime->surrounding, selection_index, length);
    if (owner != NULL && ime->state.focused)
        uc_webview_internal_ime_surrounding (owner, ime->surrounding != NULL ? ime->surrounding : "",
                                             ime->surrounding_cursor, ime->surrounding_anchor);
}

static void
uc_wpe_ime_reset (WPEInputMethodContext *context)
{
    uc_ime_state_reset (&UC_WPE_IME (context)->state);
}

/* user_data: TRUE for the hints, FALSE for the purpose */
static void
on_content_type_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    UcWpeIme *ime = UC_WPE_IME (object);
    (void) pspec;
    /* focused: the field type changed (focus moved between inputs); stale: the first state of the new document */
    apply (ime, uc_ime_state_content_type (&ime->state, GPOINTER_TO_INT (user_data)));
}

static void
uc_wpe_ime_dispose (GObject *object)
{
    UcWpeIme *ime = UC_WPE_IME (object);
    if (ime->scheduled_source != 0)
        g_source_remove (ime->scheduled_source);
    ime->scheduled_source = 0;
    uc_ime_state_clear (&ime->state);
    g_clear_pointer (&ime->surrounding, g_free);
    G_OBJECT_CLASS (uc_wpe_ime_parent_class)->dispose (object);
}

static void
uc_wpe_ime_class_init (UcWpeImeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    WPEInputMethodContextClass *ime_class = WPE_INPUT_METHOD_CONTEXT_CLASS (klass);

    object_class->dispose = uc_wpe_ime_dispose;
    ime_class->get_preedit_string = uc_wpe_ime_get_preedit_string;
    ime_class->filter_key_event = uc_wpe_ime_filter_key_event;
    ime_class->focus_in = uc_wpe_ime_focus_in;
    ime_class->focus_out = uc_wpe_ime_focus_out;
    ime_class->set_cursor_area = uc_wpe_ime_set_cursor_area;
    ime_class->set_surrounding = uc_wpe_ime_set_surrounding;
    ime_class->reset = uc_wpe_ime_reset;
}

static void
uc_wpe_ime_init (UcWpeIme *ime)
{
    uc_ime_state_init (&ime->state, ime_emit, ime);
    ime->surrounding = NULL;
    ime->surrounding_cursor = ime->surrounding_anchor = -1;
    ime->scheduled_source = 0;
    g_signal_connect (ime, "notify::input-hints", G_CALLBACK (on_content_type_changed), GINT_TO_POINTER (TRUE));
    g_signal_connect (ime, "notify::input-purpose", G_CALLBACK (on_content_type_changed), GINT_TO_POINTER (FALSE));
}

WPEInputMethodContext *
uc_wpe_ime_new (WPEView *view)
{
    return WPE_INPUT_METHOD_CONTEXT (g_object_new (UC_TYPE_WPE_IME, "view", view, NULL));
}

gboolean
uc_wpe_ime_commit (UcWpeIme *ime, const char *utf8)
{
    return uc_ime_state_commit (&ime->state, utf8);
}

gboolean
uc_wpe_ime_preedit (UcWpeIme *ime, const char *utf8, int cursor)
{
    return uc_ime_state_preedit (&ime->state, utf8, cursor);
}

gboolean
uc_wpe_ime_delete_surrounding (UcWpeIme *ime, int offset, int n_chars)
{
    return uc_ime_state_delete_surrounding (&ime->state, offset, n_chars);
}

gboolean
uc_wpe_ime_is_focused (UcWpeIme *ime)
{
    return ime->state.focused;
}

void
uc_wpe_ime_web_process_gone (UcWpeIme *ime, WebKitInputMethodContext *webkit_context)
{
    WPEInputMethodContext *context = WPE_INPUT_METHOD_CONTEXT (ime);

    if (ime->state.focused)
        g_debug ("ucwebview: the focused editable element is gone with its web process: reported as focus out");
    apply (ime, uc_ime_state_web_process_gone (&ime->state));
    /* The marker. WebKit's next state for the page sets the hints of its own context, which copies them to this one,
     * and each copy notifies only when the value changes (WebKitInputMethodContext.cpp:588-597,
     * WebKitInputMethodContextImplWPE.cpp:83-86, WPEInputMethodContext.cpp:465-474); the new document's state may equal
     * the gone one. With a bit no state has in both contexts, that next state (InputMethodFilter.cpp:98, 192-201)
     * changes the hints here, whatever it is: uc_ime_state_content_type(). The two changes made here are not reports
     * (handler blocked). */
    g_signal_handlers_block_matched (ime, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_content_type_changed, NULL);
    if (webkit_context != NULL)
        webkit_input_method_context_set_input_hints (webkit_context, (WebKitInputHints) UC_IME_HINT_STALE);
    wpe_input_method_context_set_input_hints (context, (WPEInputHints) UC_IME_HINT_STALE);
    g_signal_handlers_unblock_matched (ime, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_content_type_changed, NULL);
}
