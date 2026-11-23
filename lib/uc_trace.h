/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The library's input trace (UC_WEBVIEW_TRACE=1, read by uc_webview_global_init()): one g_message() line per key
 * event and text input call of the host, per text input report of WebKit, per uc_webview_when_rendered() result and
 * per pinch begin and end and magnification change (library 0.6.0) and per request event (library 0.7.0),
 * "ucwebview: trace <view>: ...".
 * GLib only, so that the rules below are unit-tested on a host without WPE (tests/test-trace.c).
 *
 * A trace is made to be shared (device tests, bug reports), so it never carries what the user typed or what an
 * editable element holds, whatever the element's input purpose (a PIN pad may be a plain text field). The
 * functions take the real values from their call sites and drop them here, in one place:
 *   - keys: "key printable" for every key that is not one of the named special keys of uc_trace_key_name()
 *     (characters, keypad digits, dead keys), without keysym and hardware code (the code names the key on a
 *     hardware keyboard); special keys with name, keysym and code;
 *   - the host's IME calls: commit, pre-edit (shown or ended), delete-surrounding, and whether a focused
 *     editable element took them; never the text, its length, the pre-edit caret or the deleted range;
 *   - WebKit's reports: editable focus in/out with the hint bits and the purpose number, and "surrounding text
 *     changed" without text, length, caret or selection offsets (in a password field the caret offset is the
 *     number of characters typed).
 *   - requests (library 0.7.0): the kind, the id and the outcome; never a dialog's message or default text, a
 *     prompt answer, a realm, a user name or a password.
 * What remains visible is how many key or IME events happened, not what they carried. Load URIs go through
 * uc_uri_policy_loggable() instead.
 */
#ifndef UC_TRACE_H
#define UC_TRACE_H

#include "uc_rendered.h"
#include "uc_requests.h"

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

void        uc_trace_set_enabled (gboolean enabled);
gboolean    uc_trace_enabled     (void);

/* The name of a special (non-character) key: Return, BackSpace, Tab, the arrows, Home/End, Page_Up/Down,
 * Delete, Insert, Escape, KP_Enter, ISO_Left_Tab, the modifiers and F1..F12. NULL for every other keysym. */
const char *uc_trace_key_name    (uint32_t keysym);

/* view is only printed as a pointer, to tell several views apart. */
void uc_trace_key                    (const void *view, uint32_t keysym, uint32_t keycode, gboolean pressed);
void uc_trace_ime_focus              (const void *view, gboolean focused, unsigned hints, unsigned purpose,
                                      const char *surrounding, int cursor);
void uc_trace_ime_surrounding        (const void *view, const char *text, int cursor, int anchor);
/* delivered: a focused editable element took the call (FALSE: ignored, no focused element) */
void uc_trace_ime_commit             (const void *view, const char *utf8, gboolean delivered);
void uc_trace_ime_preedit            (const void *view, const char *utf8, int cursor, gboolean delivered);
void uc_trace_ime_delete_surrounding (const void *view, int offset, int n_chars, gboolean delivered);

/* One line per uc_webview_when_rendered() result (library 0.5.0) with the time since the request, for render time
 * measurements; nothing of the page:
 *   rendered after 2840 ms (done)                                      update reported, a frame had come
 *   rendered after 2851 ms (done, rendering update after 2840 ms)      the frame came after the update
 *   rendered after 2940 ms (done without a new frame, rendering update after 2840 ms)
 *   rendered after 15000 ms (timeout)
 *   rendered after 120 ms (cancelled: navigation)
 * A timeout or cancellation after the update report names it as well. */
void uc_trace_rendered               (const void *view, const UcRenderedResult *result);

/* Pinch zoom (library 0.6.0): a line at a pinch's begin and end (none per update), one per magnification the host sets,
 * with its render time (the library measures it with a uc_webview_when_rendered() request of its own while the trace
 * is on), and one per change WebKit makes by itself (navigation, history). Scales and view pixels only, numbers with a
 * decimal point whatever the locale:
 *   pinch begin at 240,400
 *   pinch end, scale 2.00 at 240,400
 *   pinch cancelled, scale 1.42
 *   magnification 1.00 -> 2.00 at 240,400
 *   magnification 2.00 rendered after 2840 ms (done)          (the result forms of uc_trace_rendered())
 *   magnification changed to 1.00 by a navigation or history step */
void uc_trace_pinch                  (const void *view, int phase, double scale, double x, double y);
void uc_trace_magnification          (const void *view, double from, double to, double x, double y);
void uc_trace_magnification_rendered (const void *view, double scale, const UcRenderedResult *result);
void uc_trace_magnification_changed  (const void *view, double scale);

/* Requests (library 0.7.0): kind, id and outcome, nothing the page or the user wrote. The reply functions take the
 * host's text, user name and password and drop them here.
 *   request 7: alert                          (confirm, prompt, beforeunload, auth) passed to the host
 *   request 7: answered yes                   (no); prompt: "answered yes with text" (never the text)
 *   request 8: answered with credentials      (without credentials)
 *   request 7: cancelled (navigation)         the library or WebKit ended it, request_cancelled() follows;
 *                                             reasons: navigation, commit, web process terminated, webkit, load
 *   request 7: ended (view freed)             uc_webview_free(), no callback
 *   dialog: alert closed by the dialog loop guard
 *   dialog: loop guard ended by user input    a tripped guard ended at the user's tap or key press (0.7.2)
 *   default: <what>                           a decision without the host (no callback, client certificate) */
void uc_trace_request                (const void *view, guint64 id, UcRequestKind kind, int type);
void uc_trace_request_reply          (const void *view, guint64 id, int type, gboolean accept, const char *text);
void uc_trace_request_auth           (const void *view, guint64 id, const char *user, const char *password);
void uc_trace_request_end            (const void *view, guint64 id, const char *reason, gboolean reported);
void uc_trace_request_guard          (const void *view, int type);
void uc_trace_request_guard_ended    (const void *view);
void uc_trace_request_default        (const void *view, const char *what);

G_END_DECLS

#endif /* UC_TRACE_H */
