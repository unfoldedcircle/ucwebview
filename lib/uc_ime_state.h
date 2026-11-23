/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Input method state machine for libucwebview: turns the host's
 * uc_webview_ime_commit/preedit/delete_surrounding calls into the signal
 * sequence a WPEInputMethodContext must emit (WPEInputMethodContext.cpp:281-360:
 * preedit-started, preedit-changed, preedit-finished, committed(text),
 * delete-surrounding(offset, count)), and decides which of WebKit's focus calls
 * reach the host (focus reports, library 0.7.5). GLib only; the emitter is
 * injected so the logic is unit-testable without WPE.
 */
#ifndef UC_IME_STATE_H
#define UC_IME_STATE_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    UC_IME_SIGNAL_PREEDIT_STARTED,
    UC_IME_SIGNAL_PREEDIT_CHANGED,
    UC_IME_SIGNAL_PREEDIT_FINISHED,
    UC_IME_SIGNAL_COMMITTED,          /* text */
    UC_IME_SIGNAL_DELETE_SURROUNDING  /* offset, count */
} UcImeSignal;

typedef void (*UcImeEmit) (UcImeSignal signal, const char *text, int offset, unsigned count, gpointer user_data);

/* Focus reports (library 0.7.5): what the context tells the host about the page's focused editable element, with a
 * workaround for WebKit 2.54. WebKit's InputMethodFilter (UI process) keeps the state the web process sent last and
 * calls focus_out only when it goes from an element to none, focus_in only from none to an element
 * (InputMethodFilter.cpp:91-102). A web process sends a state only when its own one changes (WebPageGLib.cpp:215-227).
 * When the page moves to another web process at a navigation (process swap, WebPageProxy.cpp:6230-6307) or its web
 * process terminates, the UI side resets nothing (PageClient.h:309, PageClientImpl.cpp:149-151 are empty for WPE) and
 * the new web process starts without a state, so it never sends "none": the filter keeps the gone element's state.
 * WebKit then calls no focus_out, replays focus_in for the gone element whenever the view gains the focus
 * (WPEWebViewPlatform.cpp:104-114), and a field the new document focuses only changes the state, without focus_in.
 * The workaround: uc_ime_state_web_process_gone() reports the element gone and marks WebKit's state stale. While it is
 * stale every focus_in is such a replay and stays unreported. The first state of the new document changes the
 * context's input hints (UcWpeIme puts a marker into them, uc_wpe_ime_web_process_gone()); that ends the stale time,
 * and the host hears of the element when WebKit's update is complete (uc_ime_state_scheduled_focus()). */
typedef enum {
    UC_IME_FOCUS_KEEP,      /* nothing for the host */
    UC_IME_FOCUS_REPORT,    /* report state->focused with the context's hints and purpose */
    UC_IME_FOCUS_SCHEDULE,  /* call uc_ime_state_scheduled_focus() once WebKit's update is done (from an idle) */
} UcImeFocusAction;

typedef struct {
    UcImeEmit emit;
    gpointer  user_data;
    gboolean  focused;        /* the host was told an editable element has the focus (focus_in .. focus_out) */
    gboolean  preedit_active;
    GString  *preedit;        /* current composition text (UTF-8) */
    guint     cursor;         /* caret offset in characters within preedit */
    gboolean  stale;          /* WebKit's state belongs to a document whose web process is gone (0.7.5) */
    gboolean  scheduled;      /* uc_ime_state_scheduled_focus() is due (0.7.5) */
} UcImeState;

void        uc_ime_state_init             (UcImeState *state, UcImeEmit emit, gpointer user_data);
void        uc_ime_state_clear            (UcImeState *state);
/* focus_in/out from WebKit; losing focus drops a running pre-edit silently (WebKit cancels it itself,
 * InputMethodFilter::notifyFocusedOut). */
void        uc_ime_state_set_focused      (UcImeState *state, gboolean focused);
/* The host calls; all return FALSE (and emit nothing) when unfocused or the text is not valid UTF-8. */
gboolean    uc_ime_state_commit           (UcImeState *state, const char *utf8);
gboolean    uc_ime_state_preedit          (UcImeState *state, const char *utf8, int cursor);
gboolean    uc_ime_state_delete_surrounding (UcImeState *state, int offset, int n_chars);
/* WebKit reset (composition cancelled by the page): ends a running pre-edit with changed("") + finished. */
void        uc_ime_state_reset            (UcImeState *state);
/* Current pre-edit for WPEInputMethodContextClass::get_preedit_string: "" and 0 when none. */
const char *uc_ime_state_preedit_text     (const UcImeState *state, guint *cursor);

/* Focus reports (0.7.5, see UcImeFocusAction). WebKit's focus_in and focus_out of the context: */
UcImeFocusAction uc_ime_state_focus_in         (UcImeState *state);
UcImeFocusAction uc_ime_state_focus_out        (UcImeState *state);
/* The web process of the page's document is gone (a process swap at a navigation, or it terminated). */
UcImeFocusAction uc_ime_state_web_process_gone (UcImeState *state);
/* WebKit changed the context's input hints (hints TRUE) or its input purpose (FALSE). */
UcImeFocusAction uc_ime_state_content_type     (UcImeState *state, gboolean hints);
/* The scheduled decision; view_focused: the view has the focus (WebKit's condition for focus_in). */
UcImeFocusAction uc_ime_state_scheduled_focus  (UcImeState *state, gboolean view_focused);

G_END_DECLS

#endif /* UC_IME_STATE_H */
