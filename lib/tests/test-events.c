/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_events.c (GLib only): run with `make check`.
 */
#include "uc_events.h"
#include "uc_webview.h"

static void
test_touch_phase (void)
{
    g_assert_cmpint (uc_events_touch_phase_to_wpe (UC_WEBVIEW_TOUCH_DOWN), ==, UC_WPE_EVENT_TOUCH_DOWN);
    g_assert_cmpint (uc_events_touch_phase_to_wpe (UC_WEBVIEW_TOUCH_MOVE), ==, UC_WPE_EVENT_TOUCH_MOVE);
    g_assert_cmpint (uc_events_touch_phase_to_wpe (UC_WEBVIEW_TOUCH_UP), ==, UC_WPE_EVENT_TOUCH_UP);
    g_assert_cmpint (uc_events_touch_phase_to_wpe (UC_WEBVIEW_TOUCH_CANCEL), ==, UC_WPE_EVENT_TOUCH_CANCEL);
    g_assert_cmpint (uc_events_touch_phase_to_wpe (4), ==, -1);
    g_assert_cmpint (uc_events_touch_phase_to_wpe (-1), ==, -1);
}

static void
test_drag_threshold (void)
{
    g_assert_cmpuint (uc_events_drag_threshold (NULL, 0), ==, 0);
    g_assert_cmpuint (uc_events_drag_threshold ("", 8), ==, 8);
    g_assert_cmpuint (uc_events_drag_threshold ("12", 8), ==, 12);
    g_assert_cmpuint (uc_events_drag_threshold ("1", 8), ==, 1);
    g_assert_cmpuint (uc_events_drag_threshold ("200", 8), ==, 200);
    g_assert_cmpuint (uc_events_drag_threshold ("0", 8), ==, 8);
    g_assert_cmpuint (uc_events_drag_threshold ("201", 8), ==, 8);
    g_assert_cmpuint (uc_events_drag_threshold ("-3", 8), ==, 8);
    g_assert_cmpuint (uc_events_drag_threshold ("12px", 8), ==, 8);
    g_assert_cmpuint (uc_events_drag_threshold ("abc", 8), ==, 8);
}

static void
test_display_drag_threshold (void)
{
    gboolean invalid = TRUE;

    /* the library's display: 16 px unless UC_WEBVIEW_DRAG_THRESHOLD names a valid value (WPE's own default is 8) */
    g_assert_cmpuint (UC_EVENTS_DEFAULT_DRAG_THRESHOLD, ==, 16);
    g_assert_cmpuint (uc_events_display_drag_threshold (NULL, &invalid), ==, 16);
    g_assert_false (invalid);
    g_assert_cmpuint (uc_events_display_drag_threshold ("", &invalid), ==, 16);
    g_assert_false (invalid);
    g_assert_cmpuint (uc_events_display_drag_threshold ("8", &invalid), ==, 8);
    g_assert_false (invalid);
    g_assert_cmpuint (uc_events_display_drag_threshold ("24", &invalid), ==, 24);
    g_assert_false (invalid);
    g_assert_cmpuint (uc_events_display_drag_threshold ("200", NULL), ==, 200);
    /* refused values fall back to 16 and are reported for the log */
    for (gsize i = 0; i < 4; i++) {
        static const char *const refused[] = { "0", "201", "16px", "abc" };
        invalid = FALSE;
        g_assert_cmpuint (uc_events_display_drag_threshold (refused[i], &invalid), ==, 16);
        g_assert_true (invalid);
    }
}

static void
test_buttons (void)
{
    g_assert_cmpuint (uc_events_button_to_mod (UC_WEBVIEW_BUTTON_PRIMARY), ==, UC_WEBVIEW_MOD_BUTTON1);
    g_assert_cmpuint (uc_events_button_to_mod (UC_WEBVIEW_BUTTON_MIDDLE), ==, UC_WEBVIEW_MOD_BUTTON2);
    g_assert_cmpuint (uc_events_button_to_mod (UC_WEBVIEW_BUTTON_SECONDARY), ==, UC_WEBVIEW_MOD_BUTTON3);
    g_assert_cmpuint (uc_events_button_to_mod (0), ==, 0);
    g_assert_cmpuint (uc_events_button_to_mod (4), ==, 0);
    g_assert_true (uc_events_button_valid (1));
    g_assert_false (uc_events_button_valid (7));
}

static void
test_mods (void)
{
    g_assert_cmpuint (uc_events_sanitize_mods (0xffffffffu), ==, UC_EVENTS_ALL_MODS);
    g_assert_cmpuint (uc_events_sanitize_mods (UC_WEBVIEW_MOD_SHIFT | (1u << 20)), ==, UC_WEBVIEW_MOD_SHIFT);
    g_assert_cmpuint (UC_EVENTS_ALL_MODS, ==, (UC_WEBVIEW_MOD_CONTROL | UC_WEBVIEW_MOD_SHIFT | UC_WEBVIEW_MOD_ALT
                                              | UC_WEBVIEW_MOD_META | UC_WEBVIEW_MOD_CAPS_LOCK | UC_WEBVIEW_MOD_BUTTON1
                                              | UC_WEBVIEW_MOD_BUTTON2 | UC_WEBVIEW_MOD_BUTTON3 | UC_WEBVIEW_MOD_BUTTON4
                                              | UC_WEBVIEW_MOD_BUTTON5));
}

static void
test_time (void)
{
    uint32_t now = uc_events_time (0);
    g_assert_cmpuint (uc_events_time (1234), ==, 1234);
    g_assert_cmpuint (now, !=, 0);
    g_assert_cmpuint (uc_events_time (0) - now, <, 1000);
}

static void
test_pointer_state (void)
{
    UcPointerState state;
    double dx, dy;

    uc_pointer_state_init (&state);
    uc_pointer_state_move (&state, 10, 20, &dx, &dy);
    g_assert_cmpfloat (dx, ==, 0);
    g_assert_cmpfloat (dy, ==, 0);
    uc_pointer_state_move (&state, 13, 16, &dx, &dy);
    g_assert_cmpfloat (dx, ==, 3);
    g_assert_cmpfloat (dy, ==, -4);

    /* press: button bit set in the event and remembered */
    g_assert_cmpuint (uc_pointer_state_button (&state, UC_WEBVIEW_BUTTON_PRIMARY, TRUE, UC_WEBVIEW_MOD_SHIFT), ==,
                      UC_WEBVIEW_MOD_SHIFT | UC_WEBVIEW_MOD_BUTTON1);
    g_assert_cmpuint (uc_pointer_state_mods (&state, 0), ==, UC_WEBVIEW_MOD_BUTTON1);
    /* a second button adds */
    g_assert_cmpuint (uc_pointer_state_button (&state, UC_WEBVIEW_BUTTON_SECONDARY, TRUE, 0), ==,
                      UC_WEBVIEW_MOD_BUTTON1 | UC_WEBVIEW_MOD_BUTTON3);
    /* release: the released button is still in the event's mask, gone afterwards */
    g_assert_cmpuint (uc_pointer_state_button (&state, UC_WEBVIEW_BUTTON_PRIMARY, FALSE, 0), ==,
                      UC_WEBVIEW_MOD_BUTTON1 | UC_WEBVIEW_MOD_BUTTON3);
    g_assert_cmpuint (uc_pointer_state_mods (&state, 0), ==, UC_WEBVIEW_MOD_BUTTON3);
    /* host supplied button bits are ignored, keyboard bits pass */
    g_assert_cmpuint (uc_pointer_state_mods (&state, UC_WEBVIEW_MOD_BUTTON1 | UC_WEBVIEW_MOD_CONTROL), ==,
                      UC_WEBVIEW_MOD_BUTTON3 | UC_WEBVIEW_MOD_CONTROL);
    g_assert_cmpuint (uc_pointer_state_button (&state, UC_WEBVIEW_BUTTON_SECONDARY, FALSE, 0), ==, UC_WEBVIEW_MOD_BUTTON3);
    g_assert_cmpuint (state.buttons, ==, 0);
}

static void
test_key_state (void)
{
    UcKeyState state;
    uc_key_state_init (&state);

    g_assert_cmpuint (uc_key_state_mods (&state), ==, 0);
    /* Shift_L press: the event itself reports no shift, the state does afterwards */
    g_assert_cmpuint (uc_key_state_update (&state, 0xffe1, TRUE, 0), ==, 0);
    g_assert_cmpuint (uc_key_state_mods (&state), ==, UC_WEBVIEW_MOD_SHIFT);
    /* 'A' while shift held */
    g_assert_cmpuint (uc_key_state_update (&state, 0x0041, TRUE, 0), ==, UC_WEBVIEW_MOD_SHIFT);
    g_assert_cmpuint (uc_key_state_update (&state, 0x0041, FALSE, 0), ==, UC_WEBVIEW_MOD_SHIFT);
    /* Shift_L release: no shift reported */
    g_assert_cmpuint (uc_key_state_update (&state, 0xffe1, FALSE, 0), ==, 0);
    g_assert_cmpuint (uc_key_state_mods (&state), ==, 0);
    /* Control_R + Alt_L + Super_L */
    uc_key_state_update (&state, 0xffe4, TRUE, 0);
    uc_key_state_update (&state, 0xffe9, TRUE, 0);
    uc_key_state_update (&state, 0xffeb, TRUE, 0);
    g_assert_cmpuint (uc_key_state_mods (&state), ==, UC_WEBVIEW_MOD_CONTROL | UC_WEBVIEW_MOD_ALT | UC_WEBVIEW_MOD_META);
    uc_key_state_update (&state, 0xffe4, FALSE, 0);
    uc_key_state_update (&state, 0xffe9, FALSE, 0);
    uc_key_state_update (&state, 0xffeb, FALSE, 0);
    g_assert_cmpuint (uc_key_state_mods (&state), ==, 0);
    /* Caps_Lock toggles on press only */
    uc_key_state_update (&state, 0xffe5, TRUE, 0);
    uc_key_state_update (&state, 0xffe5, FALSE, 0);
    g_assert_cmpuint (uc_key_state_mods (&state), ==, UC_WEBVIEW_MOD_CAPS_LOCK);
    g_assert_cmpuint (uc_key_state_update (&state, 0x0061, TRUE, 0), ==, UC_WEBVIEW_MOD_CAPS_LOCK);
    uc_key_state_update (&state, 0xffe5, TRUE, 0);
    g_assert_cmpuint (uc_key_state_mods (&state), ==, 0);
    /* host modifiers are ORed in (a host that tracks modifiers itself) */
    g_assert_cmpuint (uc_key_state_update (&state, 0x0061, TRUE, UC_WEBVIEW_MOD_CONTROL | (1u << 30)), ==, UC_WEBVIEW_MOD_CONTROL);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/events/touch-phase", test_touch_phase);
    g_test_add_func ("/events/drag-threshold", test_drag_threshold);
    g_test_add_func ("/events/display-drag-threshold", test_display_drag_threshold);
    g_test_add_func ("/events/buttons", test_buttons);
    g_test_add_func ("/events/mods", test_mods);
    g_test_add_func ("/events/time", test_time);
    g_test_add_func ("/events/pointer-state", test_pointer_state);
    g_test_add_func ("/events/key-state", test_key_state);
    return g_test_run ();
}
