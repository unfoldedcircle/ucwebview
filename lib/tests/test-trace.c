/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_trace.c (GLib only): a password session through the library's input trace and its IME
 * state machine, with every log line of the process captured. No line may carry what was typed, in which form
 * ever (characters, keysyms, hardware codes, lengths, caret offsets), nor (library 0.7.0) a dialog's text, a
 * prompt answer, a realm, a user name or a password. Run with `make check`.
 */
#include "uc_ime_state.h"
#include "uc_pinch.h"
#include "uc_trace.h"
#include "uc_webview.h"

#include <string.h>

/* What the user types into the password field: characters that never occur in the trace's own words or in a
 * hexadecimal pointer, so that finding one in a line means it leaked. */
static const char *const typed[] = { "Q", "J", "Z", "#", "\xc2\xa7" /* § */, "\xe2\x82\xac" /* € */,
                                     "\xd0\xb6" /* ж */, "\xce\xa9" /* Ω */ };
static const char typed_text[] = "QJZ#\xc2\xa7\xe2\x82\xac\xd0\xb6\xce\xa9";

/* their XKB keysyms (Latin-1 directly, others 0x01000000 | code point) and evdev + 8 codes of a US keyboard */
static const struct {
    uint32_t keysym;
    uint32_t keycode;
} typed_keys[] = {
    { 0x51, 32 }, { 0x4a, 52 }, { 0x5a, 60 }, { 0x23, 20 }, { 0xa7, 0 }, { 0x10020ac, 0 }, { 0x1000436, 0 },
    { 0x10003a9, 0 },
    { 0xffb7, 79 },   /* KP_7: a keypad digit types a character too */
    { 0xfe51, 0 },    /* dead_acute: composes the next character */
};

static GPtrArray *captured;

static void
capture (const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer user_data)
{
    (void) domain; (void) level; (void) user_data;
    g_ptr_array_add (captured, g_strdup (message));
}

static void
capture_start (void)
{
    captured = g_ptr_array_new_with_free_func (g_free);
    /* every g_message/g_debug/g_warning of the process lands here, debug ones included */
    g_log_set_default_handler (capture, NULL);
}

static void
capture_stop (void)
{
    g_log_set_default_handler (g_log_default_handler, NULL);
    g_ptr_array_unref (captured);
    captured = NULL;
}

static void
ime_emit (UcImeSignal signal, const char *text, int offset, unsigned count, gpointer user_data)
{
    (void) signal; (void) text; (void) offset; (void) count;
    (*(int *) user_data)++;
}

/* the formats uc_trace.h allows, nothing else */
static const char *const allowed_lines[] = {
    "^ucwebview: trace 0x[0-9a-f]+: key printable (down|up)$",
    "^ucwebview: trace 0x[0-9a-f]+: key special [A-Za-z_0-9]+ (down|up) \\(keysym 0x[0-9a-f]+, code [0-9]+\\)$",
    "^ucwebview: trace 0x[0-9a-f]+: ime focus in, hints 0x[0-9a-f]+, purpose [0-9]+$",
    "^ucwebview: trace 0x[0-9a-f]+: ime focus out$",
    "^ucwebview: trace 0x[0-9a-f]+: ime surrounding text changed$",
    "^ucwebview: trace 0x[0-9a-f]+: ime commit( ignored \\(no focused editable element\\))?$",
    "^ucwebview: trace 0x[0-9a-f]+: ime preedit (shown|ended)( ignored \\(no focused editable element\\))?$",
    "^ucwebview: trace 0x[0-9a-f]+: ime delete-surrounding( ignored \\(no focused editable element\\))?$",
    "^ucwebview: trace 0x[0-9a-f]+: (magnification [0-9]+\\.[0-9]{2} )?rendered after [0-9]+ ms "
    "\\((done|done without a new frame|timeout"
    "|cancelled: (navigation|web process terminated|view freed|script refused))"
    "(, rendering update after [0-9]+ ms)?\\)$",
    "^ucwebview: trace 0x[0-9a-f]+: pinch begin at -?[0-9]+,-?[0-9]+$",
    "^ucwebview: trace 0x[0-9a-f]+: pinch end, scale [0-9]+\\.[0-9]{2} at -?[0-9]+,-?[0-9]+$",
    "^ucwebview: trace 0x[0-9a-f]+: pinch cancelled, scale [0-9]+\\.[0-9]{2}$",
    "^ucwebview: trace 0x[0-9a-f]+: magnification [0-9]+\\.[0-9]{2} -> [0-9]+\\.[0-9]{2} at [0-9]+,[0-9]+$",
    "^ucwebview: trace 0x[0-9a-f]+: magnification changed to [0-9]+\\.[0-9]{2} by a navigation or history step$",
    "^ucwebview: trace 0x[0-9a-f]+: request [0-9]+: (alert|confirm|prompt|beforeunload|dialog|auth)$",
    "^ucwebview: trace 0x[0-9a-f]+: request [0-9]+: answered (yes|no)( with text)?$",
    "^ucwebview: trace 0x[0-9a-f]+: request [0-9]+: answered (with|without) credentials$",
    "^ucwebview: trace 0x[0-9a-f]+: request [0-9]+: (cancelled|ended) "
    "\\((navigation|commit|web process terminated|webkit|load|view freed)\\)$",
    "^ucwebview: trace 0x[0-9a-f]+: dialog: (alert|confirm|prompt|dialog) closed by the dialog loop guard$",
    "^ucwebview: trace 0x[0-9a-f]+: dialog: loop guard ended by user input$",
    "^ucwebview: trace 0x[0-9a-f]+: default: (((alert|confirm|prompt|dialog) closed by WebKit's default"
    "|beforeunload (left|stayed)) \\(no script_dialog callback\\)"
    "|(alert|confirm|prompt|dialog) closed by WebKit's default \\(the host navigates away\\))$",
    "^ucwebview: trace 0x[0-9a-f]+: default: (auth|client certificate) (continued without credentials|cancelled)"
    "( \\(no authenticate callback\\))?$",
};

static gboolean
line_allowed (const char *line)
{
    for (gsize i = 0; i < G_N_ELEMENTS (allowed_lines); i++)
        if (g_regex_match_simple (allowed_lines[i], line, 0, 0)) {
            if (strstr (line, "key special ") != NULL) {
                /* the keysym of a special line is one of the named keys, never a character */
                const char *hex = strstr (line, "keysym 0x");
                return hex != NULL && uc_trace_key_name ((uint32_t) g_ascii_strtoull (hex + 9, NULL, 16)) != NULL;
            }
            return TRUE;
        }
    return FALSE;
}

/* asserts (printed by the test framework, not through the captured log handler) */
static void
assert_line_allowed (const char *line)
{
    if (!line_allowed (line))
        g_test_message ("trace line outside the allowed formats: %s", line);
    g_assert_true (line_allowed (line));
}

static void
test_password_session (void)
{
    const void *view = GINT_TO_POINTER (0x9990);
    const unsigned hints = UC_WEBVIEW_IME_HINT_INHIBIT_OSK | UC_WEBVIEW_IME_HINT_PRIVATE;
    char *bullets = g_strnfill (0, 0);
    UcImeState state;
    int emitted = 0;
    guint expected = 0;

    capture_start ();
    uc_trace_set_enabled (TRUE);
    uc_ime_state_init (&state, ime_emit, &emitted);

    /* the page focuses a pre-filled password field (WebKit reports its masked text, U+25CF per character) */
    for (gsize i = 0; i < G_N_ELEMENTS (typed); i++) {
        char *more = g_strconcat (bullets, "\xe2\x97\x8f", NULL);
        g_free (bullets);
        bullets = more;
    }
    uc_trace_ime_focus (view, TRUE, hints, UC_WEBVIEW_IME_PURPOSE_PASSWORD, typed_text, (int) G_N_ELEMENTS (typed));
    uc_ime_state_set_focused (&state, TRUE);
    expected++;

    /* typed on a hardware keyboard: press and release per key, the field reports its new text each time */
    for (gsize i = 0; i < G_N_ELEMENTS (typed_keys); i++) {
        uc_trace_key (view, typed_keys[i].keysym, typed_keys[i].keycode, TRUE);
        uc_trace_key (view, typed_keys[i].keysym, typed_keys[i].keycode, FALSE);
        uc_trace_ime_surrounding (view, typed_text, (int) i + 1, (int) i + 1);
        uc_trace_ime_surrounding (view, bullets, (int) i + 1, (int) i + 1);
        expected += 4;
    }

    /* typed on the on-screen keyboard: commits, a composition, a correction */
    for (gsize i = 0; i < G_N_ELEMENTS (typed); i++) {
        uc_trace_ime_commit (view, typed[i], TRUE);
        g_assert_true (uc_ime_state_commit (&state, typed[i]));
        expected++;
    }
    uc_trace_ime_preedit (view, typed_text, 3, TRUE);
    g_assert_true (uc_ime_state_preedit (&state, typed_text, 3));
    uc_trace_ime_preedit (view, "", 0, TRUE);
    g_assert_true (uc_ime_state_preedit (&state, "", 0));
    uc_trace_ime_delete_surrounding (view, -4, 4, TRUE);
    g_assert_true (uc_ime_state_delete_surrounding (&state, -4, 4));
    uc_trace_ime_commit (view, typed_text, TRUE);
    g_assert_true (uc_ime_state_commit (&state, typed_text));
    expected += 4;

    /* special keys keep their names: Return submits, BackSpace corrects */
    uc_trace_key (view, 0xff08, 22, TRUE);
    uc_trace_key (view, 0xff0d, 36, TRUE);
    expected += 2;

    /* the field goes, the keyboard still sends: ignored, and the state machine refuses it */
    uc_trace_ime_focus (view, FALSE, hints, UC_WEBVIEW_IME_PURPOSE_PASSWORD, NULL, -1);
    uc_ime_state_set_focused (&state, FALSE);
    uc_trace_ime_commit (view, typed_text, FALSE);
    g_assert_false (uc_ime_state_commit (&state, typed_text));
    expected += 2;

    g_assert_cmpint (emitted, >, 0);
    g_assert_cmpuint (captured->len, ==, expected);
    for (guint i = 0; i < captured->len; i++) {
        const char *line = g_ptr_array_index (captured, i);
        assert_line_allowed (line);
        for (gsize t = 0; t < G_N_ELEMENTS (typed); t++)
            g_assert_null (strstr (line, typed[t]));
        g_assert_null (strstr (line, "\xe2\x97\x8f"));
        for (gsize k = 0; k < G_N_ELEMENTS (typed_keys); k++) {
            char hex[16], code[24];
            g_snprintf (hex, sizeof hex, "0x%x", (unsigned) typed_keys[k].keysym);
            g_snprintf (code, sizeof code, "code %u", (unsigned) typed_keys[k].keycode);
            g_assert_null (strstr (line, hex));
            if (typed_keys[k].keycode != 0)
                g_assert_null (strstr (line, code));
        }
    }
    /* the classes are there: what the trace is for */
    g_assert_cmpstr (g_ptr_array_index (captured, 0), ==, "ucwebview: trace 0x9990: ime focus in, hints 0x880, purpose 8");
    g_assert_cmpstr (g_ptr_array_index (captured, 1), ==, "ucwebview: trace 0x9990: key printable down");
    g_assert_cmpstr (g_ptr_array_index (captured, 3), ==, "ucwebview: trace 0x9990: ime surrounding text changed");
    g_assert_cmpstr (g_ptr_array_index (captured, expected - 4),
                     ==, "ucwebview: trace 0x9990: key special BackSpace down (keysym 0xff08, code 22)");
    g_assert_cmpstr (g_ptr_array_index (captured, expected - 1),
                     ==, "ucwebview: trace 0x9990: ime commit ignored (no focused editable element)");

    uc_ime_state_clear (&state);
    uc_trace_set_enabled (FALSE);
    g_free (bullets);
    capture_stop ();
}

static UcRenderedResult
rendered_result (UcRenderedStatus status, gint64 elapsed_us, gint64 update_us, UcRenderedFrame frame,
                 const char *reason)
{
    UcRenderedResult result;
    result.id = 7;
    result.status = status;
    result.elapsed_us = elapsed_us;
    result.update_us = update_us;
    result.frame = frame;
    result.reason = reason;
    return result;
}

/* the render time lines of uc_webview_when_rendered() (library 0.5.0): one per result, with the times */
static void
test_rendered (void)
{
    const void *view = GINT_TO_POINTER (0x9990);
    const UcRenderedResult results[] = {
        rendered_result (UC_RENDERED_DONE, 2840400, 2840400, UC_RENDERED_FRAME_BEFORE_UPDATE, NULL),
        rendered_result (UC_RENDERED_DONE, 2851000, 2839600, UC_RENDERED_FRAME_AFTER_UPDATE, NULL),
        rendered_result (UC_RENDERED_DONE, 2940000, 2840000, UC_RENDERED_NO_FRAME, NULL),
        rendered_result (UC_RENDERED_TIMEOUT, 15000300, -1, UC_RENDERED_NO_FRAME, NULL),
        rendered_result (UC_RENDERED_TIMEOUT, 15000000, 14950000, UC_RENDERED_NO_FRAME, NULL),
        rendered_result (UC_RENDERED_CANCELLED, 120000, -1, UC_RENDERED_NO_FRAME, "navigation"),
        rendered_result (UC_RENDERED_CANCELLED, 80000, 70000, UC_RENDERED_NO_FRAME, "view freed"),
        rendered_result (UC_RENDERED_CANCELLED, 5000, -1, UC_RENDERED_NO_FRAME, "web process terminated"),
        rendered_result (UC_RENDERED_CANCELLED, 1000, -1, UC_RENDERED_NO_FRAME, "script refused"),
    };
    static const char *const expected[] = {
        "ucwebview: trace 0x9990: rendered after 2840 ms (done)",
        "ucwebview: trace 0x9990: rendered after 2851 ms (done, rendering update after 2840 ms)",
        "ucwebview: trace 0x9990: rendered after 2940 ms (done without a new frame, rendering update after 2840 ms)",
        "ucwebview: trace 0x9990: rendered after 15000 ms (timeout)",
        "ucwebview: trace 0x9990: rendered after 15000 ms (timeout, rendering update after 14950 ms)",
        "ucwebview: trace 0x9990: rendered after 120 ms (cancelled: navigation)",
        "ucwebview: trace 0x9990: rendered after 80 ms (cancelled: view freed, rendering update after 70 ms)",
        "ucwebview: trace 0x9990: rendered after 5 ms (cancelled: web process terminated)",
        "ucwebview: trace 0x9990: rendered after 1 ms (cancelled: script refused)",
    };

    capture_start ();
    uc_trace_set_enabled (TRUE);
    for (gsize i = 0; i < G_N_ELEMENTS (results); i++)
        uc_trace_rendered (view, &results[i]);
    uc_trace_set_enabled (FALSE);
    uc_trace_rendered (view, &results[0]);   /* off: nothing */
    g_assert_cmpuint (captured->len, ==, G_N_ELEMENTS (expected));
    for (guint i = 0; i < captured->len; i++) {
        assert_line_allowed (g_ptr_array_index (captured, i));
        g_assert_cmpstr (g_ptr_array_index (captured, i), ==, expected[i]);
    }
    capture_stop ();
}

/* pinch zoom (library 0.6.0): begin and end lines (no update lines), the host's magnification with its focal point
 * and render time, WebKit's own changes; numbers with a decimal point */
static void
test_pinch (void)
{
    const void *view = GINT_TO_POINTER (0x7770);
    const UcRenderedResult done = rendered_result (UC_RENDERED_DONE, 2840400, 2840400, UC_RENDERED_FRAME_BEFORE_UPDATE,
                                                   NULL);
    const UcRenderedResult timeout = rendered_result (UC_RENDERED_TIMEOUT, 60000000, -1, UC_RENDERED_NO_FRAME, NULL);
    static const char *const expected[] = {
        "ucwebview: trace 0x7770: pinch begin at 240,400",
        "ucwebview: trace 0x7770: pinch end, scale 2.48 at 241,398",
        "ucwebview: trace 0x7770: pinch cancelled, scale 0.75",
        "ucwebview: trace 0x7770: magnification 1.00 -> 2.48 at 240,400",
        "ucwebview: trace 0x7770: magnification 2.48 rendered after 2840 ms (done)",
        "ucwebview: trace 0x7770: magnification 4.00 rendered after 60000 ms (timeout)",
        "ucwebview: trace 0x7770: magnification changed to 1.00 by a navigation or history step",
    };

    capture_start ();
    uc_trace_set_enabled (TRUE);
    uc_trace_pinch (view, UC_PINCH_BEGIN, 1.0, 240.2, 399.6);
    uc_trace_pinch (view, UC_PINCH_UPDATE, 1.7, 240, 400);     /* no line */
    uc_trace_pinch (view, UC_PINCH_END, 2.4789, 240.6, 398.4);
    uc_trace_pinch (view, UC_PINCH_CANCEL, 0.75, 1, 1);
    uc_trace_magnification (view, 1.0, 2.4789, 240, 400);
    uc_trace_magnification_rendered (view, 2.4789, &done);
    uc_trace_magnification_rendered (view, 4.0, &timeout);
    uc_trace_magnification_changed (view, 1.0);
    uc_trace_set_enabled (FALSE);
    uc_trace_pinch (view, UC_PINCH_BEGIN, 1.0, 1, 1);           /* off: nothing */
    uc_trace_magnification (view, 1.0, 2.0, 1, 1);
    uc_trace_magnification_changed (view, 1.0);
    g_assert_cmpuint (captured->len, ==, G_N_ELEMENTS (expected));
    for (guint i = 0; i < captured->len; i++) {
        assert_line_allowed (g_ptr_array_index (captured, i));
        g_assert_cmpstr (g_ptr_array_index (captured, i), ==, expected[i]);
    }
    capture_stop ();
}

/* Requests (library 0.7.0): a dialog session and a login with every log line captured. The page's texts, the host's
 * answers and the credentials are markers that occur in no line format; finding one in a line means it leaked. */
static void
test_requests (void)
{
    const void *view = GINT_TO_POINTER (0x5550);
    static const char *const markers[] = { "XQZJ-message", "XQZJ-default", "XQZJ-answer", "XQZJ-realm", "XQZJ-user",
                                           "XQZJ-password", "XQZJ" };
    static const char *const expected[] = {
        "ucwebview: trace 0x5550: request 41: alert",
        "ucwebview: trace 0x5550: request 41: answered yes",
        "ucwebview: trace 0x5550: request 42: confirm",
        "ucwebview: trace 0x5550: request 42: answered no",
        "ucwebview: trace 0x5550: request 43: prompt",
        "ucwebview: trace 0x5550: request 43: answered yes with text",
        "ucwebview: trace 0x5550: request 44: prompt",
        "ucwebview: trace 0x5550: request 44: answered no",
        "ucwebview: trace 0x5550: request 45: beforeunload",
        "ucwebview: trace 0x5550: request 45: cancelled (navigation)",
        "ucwebview: trace 0x5550: request 46: auth",
        "ucwebview: trace 0x5550: request 46: answered with credentials",
        "ucwebview: trace 0x5550: request 47: auth",
        "ucwebview: trace 0x5550: request 47: answered without credentials",
        "ucwebview: trace 0x5550: request 48: auth",
        "ucwebview: trace 0x5550: request 48: cancelled (webkit)",
        "ucwebview: trace 0x5550: request 49: alert",
        "ucwebview: trace 0x5550: request 49: cancelled (web process terminated)",
        "ucwebview: trace 0x5550: request 50: confirm",
        "ucwebview: trace 0x5550: request 50: ended (view freed)",
        "ucwebview: trace 0x5550: dialog: alert closed by the dialog loop guard",
        "ucwebview: trace 0x5550: dialog: loop guard ended by user input",
        "ucwebview: trace 0x5550: default: beforeunload left (no script_dialog callback)",
        "ucwebview: trace 0x5550: default: auth continued without credentials (no authenticate callback)",
        "ucwebview: trace 0x5550: default: client certificate continued without credentials",
        "ucwebview: trace 0x5550: default: prompt closed by WebKit's default (no script_dialog callback)",
        "ucwebview: trace 0x5550: default: confirm closed by WebKit's default (the host navigates away)",
    };
    UcDialogAnswer answer;

    capture_start ();
    uc_trace_set_enabled (TRUE);
    uc_trace_request (view, 41, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_ALERT);
    uc_trace_request_reply (view, 41, UC_WEBVIEW_DIALOG_ALERT, TRUE, markers[2]);
    uc_trace_request (view, 42, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_CONFIRM);
    uc_trace_request_reply (view, 42, UC_WEBVIEW_DIALOG_CONFIRM, FALSE, NULL);
    uc_trace_request (view, 43, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_PROMPT);
    answer = uc_dialog_answer (UC_WEBVIEW_DIALOG_PROMPT, TRUE, markers[2]);
    uc_trace_request_reply (view, 43, UC_WEBVIEW_DIALOG_PROMPT, TRUE, answer.text);
    uc_trace_request (view, 44, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_PROMPT);
    uc_trace_request_reply (view, 44, UC_WEBVIEW_DIALOG_PROMPT, FALSE, markers[1]);
    uc_trace_request (view, 45, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD);
    uc_trace_request_end (view, 45, "navigation", TRUE);
    uc_trace_request (view, 46, UC_REQUEST_AUTH, UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC);
    uc_trace_request_auth (view, 46, markers[4], markers[5]);
    uc_trace_request (view, 47, UC_REQUEST_AUTH, UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST);
    uc_trace_request_auth (view, 47, NULL, markers[5]);
    uc_trace_request (view, 48, UC_REQUEST_AUTH, UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC);
    uc_trace_request_end (view, 48, "webkit", TRUE);
    uc_trace_request (view, 49, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_ALERT);
    uc_trace_request_end (view, 49, "web process terminated", TRUE);
    uc_trace_request (view, 50, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_CONFIRM);
    uc_trace_request_end (view, 50, "view freed", FALSE);
    uc_trace_request_guard (view, UC_WEBVIEW_DIALOG_ALERT);
    uc_trace_request_guard_ended (view);
    uc_trace_request_default (view, "beforeunload left (no script_dialog callback)");
    uc_trace_request_default (view, "auth continued without credentials (no authenticate callback)");
    uc_trace_request_default (view, "client certificate continued without credentials");
    uc_trace_request_default (view, "prompt closed by WebKit's default (no script_dialog callback)");
    uc_trace_request_default (view, "confirm closed by WebKit's default (the host navigates away)");
    uc_trace_set_enabled (FALSE);
    uc_trace_request (view, 51, UC_REQUEST_DIALOG, UC_WEBVIEW_DIALOG_ALERT);     /* off: nothing */
    uc_trace_request_reply (view, 51, UC_WEBVIEW_DIALOG_ALERT, TRUE, markers[2]);
    uc_trace_request_auth (view, 51, markers[4], markers[5]);
    uc_trace_request_end (view, 51, "navigation", TRUE);
    uc_trace_request_guard (view, UC_WEBVIEW_DIALOG_ALERT);
    uc_trace_request_guard_ended (view);
    uc_trace_request_default (view, "x");

    g_assert_cmpuint (captured->len, ==, G_N_ELEMENTS (expected));
    for (guint i = 0; i < captured->len; i++) {
        const char *line = g_ptr_array_index (captured, i);
        assert_line_allowed (line);
        g_assert_cmpstr (line, ==, expected[i]);
        for (gsize m = 0; m < G_N_ELEMENTS (markers); m++)
            g_assert_null (strstr (line, markers[m]));
    }
    capture_stop ();
}

static void
test_disabled (void)
{
    capture_start ();
    uc_trace_set_enabled (FALSE);
    uc_trace_key (NULL, 0xff0d, 36, TRUE);
    uc_trace_key (NULL, 0x61, 38, TRUE);
    uc_trace_ime_focus (NULL, TRUE, 0, 0, "x", 1);
    uc_trace_ime_surrounding (NULL, "x", 1, 1);
    uc_trace_ime_commit (NULL, "x", TRUE);
    uc_trace_ime_preedit (NULL, "x", 1, TRUE);
    uc_trace_ime_delete_surrounding (NULL, -1, 1, TRUE);
    g_assert_cmpuint (captured->len, ==, 0);
    g_assert_false (uc_trace_enabled ());
    capture_stop ();
}

static void
test_key_names (void)
{
    g_assert_cmpstr (uc_trace_key_name (0xff0d), ==, "Return");
    g_assert_cmpstr (uc_trace_key_name (0xff08), ==, "BackSpace");
    g_assert_cmpstr (uc_trace_key_name (0xff52), ==, "Up");
    g_assert_cmpstr (uc_trace_key_name (0xff8d), ==, "KP_Enter");
    g_assert_cmpstr (uc_trace_key_name (0xffbe), ==, "F1");
    g_assert_cmpstr (uc_trace_key_name (0xffc9), ==, "F12");
    g_assert_cmpstr (uc_trace_key_name (0xffe1), ==, "Shift_L");
    /* characters, keypad digits and operators, dead keys, legacy and Unicode keysyms: no name */
    for (uint32_t c = 0x20; c <= 0x7e; c++)
        g_assert_null (uc_trace_key_name (c));
    for (uint32_t c = 0xa0; c <= 0xff; c++)
        g_assert_null (uc_trace_key_name (c));
    for (uint32_t kp = 0xffaa; kp <= 0xffb9; kp++)
        g_assert_null (uc_trace_key_name (kp));
    g_assert_null (uc_trace_key_name (0xff80)); /* KP_Space */
    g_assert_null (uc_trace_key_name (0xfe51)); /* dead_acute */
    g_assert_null (uc_trace_key_name (0x06c6)); /* Cyrillic_zhe */
    g_assert_null (uc_trace_key_name (0x1000436));
    g_assert_null (uc_trace_key_name (0));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/trace/password-session", test_password_session);
    g_test_add_func ("/trace/disabled", test_disabled);
    g_test_add_func ("/trace/key-names", test_key_names);
    g_test_add_func ("/trace/rendered", test_rendered);
    g_test_add_func ("/trace/pinch", test_pinch);
    g_test_add_func ("/trace/requests", test_requests);
    return g_test_run ();
}
