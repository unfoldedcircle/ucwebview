/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 */
#include "uc_events.h"
#include "uc_webview.h"

/* XKB keysyms of the modifier keys (WPEKeysyms.h:192-203). */
#define XKB_SHIFT_L   0xffe1
#define XKB_SHIFT_R   0xffe2
#define XKB_CONTROL_L 0xffe3
#define XKB_CONTROL_R 0xffe4
#define XKB_CAPS_LOCK 0xffe5
#define XKB_META_L    0xffe7
#define XKB_META_R    0xffe8
#define XKB_ALT_L     0xffe9
#define XKB_ALT_R     0xffea
#define XKB_SUPER_L   0xffeb
#define XKB_SUPER_R   0xffec

int
uc_events_touch_phase_to_wpe (int phase)
{
    switch (phase) {
    case UC_WEBVIEW_TOUCH_DOWN:   return UC_WPE_EVENT_TOUCH_DOWN;
    case UC_WEBVIEW_TOUCH_MOVE:   return UC_WPE_EVENT_TOUCH_MOVE;
    case UC_WEBVIEW_TOUCH_UP:     return UC_WPE_EVENT_TOUCH_UP;
    case UC_WEBVIEW_TOUCH_CANCEL: return UC_WPE_EVENT_TOUCH_CANCEL;
    default:                      return -1;
    }
}

unsigned
uc_events_button_to_mod (int button)
{
    switch (button) {
    case UC_WEBVIEW_BUTTON_PRIMARY:   return UC_WEBVIEW_MOD_BUTTON1;
    case UC_WEBVIEW_BUTTON_MIDDLE:    return UC_WEBVIEW_MOD_BUTTON2;
    case UC_WEBVIEW_BUTTON_SECONDARY: return UC_WEBVIEW_MOD_BUTTON3;
    default:                          return 0;
    }
}

gboolean
uc_events_button_valid (int button)
{
    return uc_events_button_to_mod (button) != 0;
}

unsigned
uc_events_sanitize_mods (unsigned mods)
{
    return mods & UC_EVENTS_ALL_MODS;
}

uint32_t
uc_events_time (uint32_t ms)
{
    if (ms != 0)
        return ms;
    return (uint32_t) (g_get_monotonic_time () / 1000);
}

uint32_t
uc_events_drag_threshold (const char *value, uint32_t fallback)
{
    guint64 parsed = 0;

    if (value == NULL || *value == '\0')
        return fallback;
    if (!g_ascii_string_to_unsigned (value, 10, 1, 200, &parsed, NULL))
        return fallback;
    return (uint32_t) parsed;
}

uint32_t
uc_events_display_drag_threshold (const char *env_value, gboolean *env_invalid)
{
    uint32_t threshold = uc_events_drag_threshold (env_value, 0);

    if (env_invalid != NULL)
        *env_invalid = threshold == 0 && env_value != NULL && *env_value != '\0';
    return threshold != 0 ? threshold : UC_EVENTS_DEFAULT_DRAG_THRESHOLD;
}

/* ---- pointer -------------------------------------------------------------- */

void
uc_pointer_state_init (UcPointerState *state)
{
    state->buttons = 0;
    state->last_x = state->last_y = 0;
    state->has_last = FALSE;
}

unsigned
uc_pointer_state_button (UcPointerState *state, int button, gboolean pressed, unsigned host_mods)
{
    unsigned bit = uc_events_button_to_mod (button);
    unsigned mods;

    if (pressed) {
        state->buttons |= bit;
        mods = state->buttons;
    } else {
        /* The button of a release event is still reported as pressed in the event's modifiers
         * (libinput/GTK semantics, matched by WebEventFactoryWPE.cpp pressedMouseButtons for MouseUp). */
        mods = state->buttons;
        state->buttons &= ~bit;
    }
    return (host_mods & UC_EVENTS_KEYBOARD_MODS) | mods;
}

void
uc_pointer_state_move (UcPointerState *state, double x, double y, double *dx, double *dy)
{
    if (state->has_last) {
        *dx = x - state->last_x;
        *dy = y - state->last_y;
    } else {
        *dx = *dy = 0;
        state->has_last = TRUE;
    }
    state->last_x = x;
    state->last_y = y;
}

unsigned
uc_pointer_state_mods (const UcPointerState *state, unsigned host_mods)
{
    return (host_mods & UC_EVENTS_KEYBOARD_MODS) | state->buttons;
}

/* ---- keyboard ------------------------------------------------------------- */

void
uc_key_state_init (UcKeyState *state)
{
    state->held = 0;
    state->caps_lock = FALSE;
}

static unsigned
modifier_bit_for_keysym (uint32_t keysym)
{
    switch (keysym) {
    case XKB_SHIFT_L: case XKB_SHIFT_R:     return UC_WEBVIEW_MOD_SHIFT;
    case XKB_CONTROL_L: case XKB_CONTROL_R: return UC_WEBVIEW_MOD_CONTROL;
    case XKB_ALT_L: case XKB_ALT_R:         return UC_WEBVIEW_MOD_ALT;
    case XKB_META_L: case XKB_META_R:
    case XKB_SUPER_L: case XKB_SUPER_R:     return UC_WEBVIEW_MOD_META;
    default:                                return 0;
    }
}

unsigned
uc_key_state_update (UcKeyState *state, uint32_t keysym, gboolean pressed, unsigned host_mods)
{
    unsigned bit = modifier_bit_for_keysym (keysym);
    unsigned before = uc_key_state_mods (state);

    if (keysym == XKB_CAPS_LOCK) {
        if (pressed)
            state->caps_lock = !state->caps_lock;
        return (host_mods & UC_EVENTS_ALL_MODS) | before;
    }
    if (bit != 0) {
        if (pressed)
            state->held |= bit;
        else
            state->held &= ~bit;
        /* the modifier key itself: report the state before the press / after the release */
        return (host_mods & UC_EVENTS_ALL_MODS) | (pressed ? before : uc_key_state_mods (state));
    }
    return (host_mods & UC_EVENTS_ALL_MODS) | before;
}

unsigned
uc_key_state_mods (const UcKeyState *state)
{
    return state->held | (state->caps_lock ? UC_WEBVIEW_MOD_CAPS_LOCK : 0);
}
