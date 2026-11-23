/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The library's input trace, see uc_trace.h for what it may carry.
 */
#include "uc_trace.h"
#include "uc_pinch.h"
#include "uc_webview.h"

static gboolean trace_enabled;

void
uc_trace_set_enabled (gboolean enabled)
{
    trace_enabled = enabled;
}

gboolean
uc_trace_enabled (void)
{
    return trace_enabled;
}

/* XKB keysyms (WPEKeysyms.h, identical to xkbcommon-keysyms.h) of the keys that type no character. Everything
 * else, keypad digits (KP_0 0xffb0 ...) and dead keys included, is "printable" for the trace. */
static const struct {
    uint32_t keysym;
    const char *name;
} special_keys[] = {
    { 0xff08, "BackSpace" }, { 0xff09, "Tab" },        { 0xff0d, "Return" },    { 0xff1b, "Escape" },
    { 0xff50, "Home" },      { 0xff51, "Left" },       { 0xff52, "Up" },        { 0xff53, "Right" },
    { 0xff54, "Down" },      { 0xff55, "Page_Up" },    { 0xff56, "Page_Down" }, { 0xff57, "End" },
    { 0xff63, "Insert" },    { 0xff8d, "KP_Enter" },   { 0xffff, "Delete" },    { 0xfe20, "ISO_Left_Tab" },
    { 0xffe1, "Shift_L" },   { 0xffe2, "Shift_R" },    { 0xffe3, "Control_L" }, { 0xffe4, "Control_R" },
    { 0xffe5, "Caps_Lock" }, { 0xffe7, "Meta_L" },     { 0xffe8, "Meta_R" },    { 0xffe9, "Alt_L" },
    { 0xffea, "Alt_R" },     { 0xffeb, "Super_L" },    { 0xffec, "Super_R" },
    { 0xffbe, "F1" },        { 0xffbf, "F2" },         { 0xffc0, "F3" },        { 0xffc1, "F4" },
    { 0xffc2, "F5" },        { 0xffc3, "F6" },         { 0xffc4, "F7" },        { 0xffc5, "F8" },
    { 0xffc6, "F9" },        { 0xffc7, "F10" },        { 0xffc8, "F11" },       { 0xffc9, "F12" },
};

const char *
uc_trace_key_name (uint32_t keysym)
{
    for (gsize i = 0; i < G_N_ELEMENTS (special_keys); i++)
        if (special_keys[i].keysym == keysym)
            return special_keys[i].name;
    return NULL;
}

void
uc_trace_key (const void *view, uint32_t keysym, uint32_t keycode, gboolean pressed)
{
    const char *name;

    if (!trace_enabled)
        return;
    name = uc_trace_key_name (keysym);
    if (name != NULL)
        g_message ("ucwebview: trace %p: key special %s %s (keysym 0x%x, code %u)", view, name, pressed ? "down" : "up",
                   (unsigned) keysym, (unsigned) keycode);
    else
        g_message ("ucwebview: trace %p: key printable %s", view, pressed ? "down" : "up");
}

void
uc_trace_ime_focus (const void *view, gboolean focused, unsigned hints, unsigned purpose, const char *surrounding,
                    int cursor)
{
    (void) surrounding; (void) cursor; /* the field's content and caret: never logged */
    if (!trace_enabled)
        return;
    if (focused)
        g_message ("ucwebview: trace %p: ime focus in, hints 0x%x, purpose %u", view, hints, purpose);
    else
        g_message ("ucwebview: trace %p: ime focus out", view);
}

void
uc_trace_ime_surrounding (const void *view, const char *text, int cursor, int anchor)
{
    (void) text; (void) cursor; (void) anchor; /* content, caret and selection: never logged */
    if (trace_enabled)
        g_message ("ucwebview: trace %p: ime surrounding text changed", view);
}

static const char *
delivery (gboolean delivered)
{
    return delivered ? "" : " ignored (no focused editable element)";
}

void
uc_trace_ime_commit (const void *view, const char *utf8, gboolean delivered)
{
    (void) utf8;
    if (trace_enabled)
        g_message ("ucwebview: trace %p: ime commit%s", view, delivery (delivered));
}

void
uc_trace_ime_preedit (const void *view, const char *utf8, int cursor, gboolean delivered)
{
    (void) cursor;
    if (trace_enabled)
        g_message ("ucwebview: trace %p: ime preedit %s%s", view, utf8 != NULL && *utf8 != '\0' ? "shown" : "ended",
                   delivery (delivered));
}

void
uc_trace_ime_delete_surrounding (const void *view, int offset, int n_chars, gboolean delivered)
{
    (void) offset; (void) n_chars;
    if (trace_enabled)
        g_message ("ucwebview: trace %p: ime delete-surrounding%s", view, delivery (delivered));
}

static long
to_ms (gint64 us)
{
    return (long) ((us + 500) / 1000);
}

/* "rendered after N ms (...)" of a result */
static char *
rendered_text (const UcRenderedResult *result)
{
    const char *what;
    char *update = NULL, *text;

    switch (result->status) {
    case UC_RENDERED_DONE:
        what = result->frame == UC_RENDERED_NO_FRAME ? "done without a new frame" : "done";
        break;
    case UC_RENDERED_TIMEOUT:
        what = "timeout";
        break;
    case UC_RENDERED_CANCELLED:
    default:
        what = NULL;
        break;
    }
    /* the update's time unless the result came with it (a frame had arrived before the report) */
    if (result->update_us >= 0
        && !(result->status == UC_RENDERED_DONE && result->frame == UC_RENDERED_FRAME_BEFORE_UPDATE))
        update = g_strdup_printf (", rendering update after %ld ms", to_ms (result->update_us));
    if (what != NULL)
        text = g_strdup_printf ("rendered after %ld ms (%s%s)", to_ms (result->elapsed_us), what,
                                update != NULL ? update : "");
    else
        text = g_strdup_printf ("rendered after %ld ms (cancelled: %s%s)", to_ms (result->elapsed_us),
                                result->reason != NULL ? result->reason : "unknown", update != NULL ? update : "");
    g_free (update);
    return text;
}

void
uc_trace_rendered (const void *view, const UcRenderedResult *result)
{
    char *text;

    if (!trace_enabled || result == NULL)
        return;
    text = rendered_text (result);
    g_message ("ucwebview: trace %p: %s", view, text);
    g_free (text);
}

/* a scale or a coordinate with a decimal point whatever LC_NUMERIC says (the host may set a locale) */
static const char *
number (char *buf, gsize size, const char *format, double value)
{
    return g_ascii_formatd (buf, (int) size, format, value);
}

#define NUMBER_BUF 32

void
uc_trace_pinch (const void *view, int phase, double scale, double x, double y)
{
    char s[NUMBER_BUF], px[NUMBER_BUF], py[NUMBER_BUF];

    if (!trace_enabled)
        return;
    number (s, sizeof s, "%.2f", scale);
    number (px, sizeof px, "%.0f", x);
    number (py, sizeof py, "%.0f", y);
    switch (phase) {
    case UC_PINCH_BEGIN:
        g_message ("ucwebview: trace %p: pinch begin at %s,%s", view, px, py);
        break;
    case UC_PINCH_END:
        g_message ("ucwebview: trace %p: pinch end, scale %s at %s,%s", view, s, px, py);
        break;
    case UC_PINCH_CANCEL:
        g_message ("ucwebview: trace %p: pinch cancelled, scale %s", view, s);
        break;
    default:
        break;  /* no line per update */
    }
}

void
uc_trace_magnification (const void *view, double from, double to, double x, double y)
{
    char f[NUMBER_BUF], t[NUMBER_BUF], px[NUMBER_BUF], py[NUMBER_BUF];

    if (!trace_enabled)
        return;
    g_message ("ucwebview: trace %p: magnification %s -> %s at %s,%s", view, number (f, sizeof f, "%.2f", from),
               number (t, sizeof t, "%.2f", to), number (px, sizeof px, "%.0f", x), number (py, sizeof py, "%.0f", y));
}

void
uc_trace_magnification_rendered (const void *view, double scale, const UcRenderedResult *result)
{
    char s[NUMBER_BUF], *text;

    if (!trace_enabled || result == NULL)
        return;
    text = rendered_text (result);
    g_message ("ucwebview: trace %p: magnification %s %s", view, number (s, sizeof s, "%.2f", scale), text);
    g_free (text);
}

void
uc_trace_magnification_changed (const void *view, double scale)
{
    char s[NUMBER_BUF];

    if (trace_enabled)
        g_message ("ucwebview: trace %p: magnification changed to %s by a navigation or history step", view,
                   number (s, sizeof s, "%.2f", scale));
}

/* ---- requests (library 0.7.0) ------------------------------------------------------------------------------- */

static const char *
request_kind_name (UcRequestKind kind, int type)
{
    return kind == UC_REQUEST_AUTH ? "auth" : uc_dialog_type_name (type);
}

void
uc_trace_request (const void *view, guint64 id, UcRequestKind kind, int type)
{
    if (trace_enabled)
        g_message ("ucwebview: trace %p: request %" G_GUINT64_FORMAT ": %s", view, id, request_kind_name (kind, type));
}

void
uc_trace_request_reply (const void *view, guint64 id, int type, gboolean accept, const char *text)
{
    (void) text;    /* the prompt's answer: never logged */
    if (!trace_enabled)
        return;
    g_message ("ucwebview: trace %p: request %" G_GUINT64_FORMAT ": answered %s%s", view, id, accept ? "yes" : "no",
               accept && type == UC_WEBVIEW_DIALOG_PROMPT ? " with text" : "");
}

void
uc_trace_request_auth (const void *view, guint64 id, const char *user, const char *password)
{
    (void) password;   /* never logged, nor the user name */
    if (trace_enabled)
        g_message ("ucwebview: trace %p: request %" G_GUINT64_FORMAT ": answered %s credentials", view, id,
                   user != NULL ? "with" : "without");
}

void
uc_trace_request_end (const void *view, guint64 id, const char *reason, gboolean reported)
{
    if (trace_enabled)
        g_message ("ucwebview: trace %p: request %" G_GUINT64_FORMAT ": %s (%s)", view, id,
                   reported ? "cancelled" : "ended", reason != NULL ? reason : "unknown");
}

void
uc_trace_request_guard (const void *view, int type)
{
    if (trace_enabled)
        g_message ("ucwebview: trace %p: dialog: %s closed by the dialog loop guard", view, uc_dialog_type_name (type));
}

void
uc_trace_request_guard_ended (const void *view)
{
    if (trace_enabled)
        g_message ("ucwebview: trace %p: dialog: loop guard ended by user input", view);
}

void
uc_trace_request_default (const void *view, const char *what)
{
    if (trace_enabled)
        g_message ("ucwebview: trace %p: default: %s", view, what);
}
