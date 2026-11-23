/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Event translation helpers for libucwebview: C API values -> WPEEvent values,
 * pointer button and keyboard modifier state. GLib only, so that the logic is
 * unit-testable on a host without WPE. The UC_WPE_* mirrors of the WPEPlatform
 * enums are checked against the real headers with _Static_assert in
 * uc_wpe_view.c (WPEEvent.h:61-79, 118-130, 138-153).
 */
#ifndef UC_EVENTS_H
#define UC_EVENTS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* WPEEventType (WPEEvent.h:61-79) */
enum {
    UC_WPE_EVENT_NONE = 0,
    UC_WPE_EVENT_POINTER_DOWN = 1,
    UC_WPE_EVENT_POINTER_UP = 2,
    UC_WPE_EVENT_POINTER_MOVE = 3,
    UC_WPE_EVENT_POINTER_ENTER = 4,
    UC_WPE_EVENT_POINTER_LEAVE = 5,
    UC_WPE_EVENT_SCROLL = 6,
    UC_WPE_EVENT_KEYBOARD_KEY_DOWN = 7,
    UC_WPE_EVENT_KEYBOARD_KEY_UP = 8,
    UC_WPE_EVENT_TOUCH_DOWN = 9,
    UC_WPE_EVENT_TOUCH_UP = 10,
    UC_WPE_EVENT_TOUCH_MOVE = 11,
    UC_WPE_EVENT_TOUCH_CANCEL = 12,
};

/* All modifier bits the C API accepts (UC_WEBVIEW_MOD_* == WPEModifiers). */
#define UC_EVENTS_KEYBOARD_MODS 0x1fu
#define UC_EVENTS_POINTER_MODS  0x1f00u
#define UC_EVENTS_ALL_MODS      (UC_EVENTS_KEYBOARD_MODS | UC_EVENTS_POINTER_MODS)

/* UC_WEBVIEW_TOUCH_* -> UC_WPE_EVENT_TOUCH_*, or -1 for an unknown phase. */
int      uc_events_touch_phase_to_wpe (int phase);
/* UC_WEBVIEW_BUTTON_* -> the UC_WEBVIEW_MOD_BUTTONn bit WebKit reads as the pressed-buttons mask
 * (WebEventFactoryWPE.cpp buttonForWPEModifiers/pressedMouseButtons), 0 for an unknown button. */
unsigned uc_events_button_to_mod      (int button);
gboolean uc_events_button_valid       (int button);
/* Masks unknown bits off host-supplied modifiers. */
unsigned uc_events_sanitize_mods      (unsigned mods);
/* 0 -> milliseconds of the monotonic clock, else the value unchanged. */
uint32_t uc_events_time               (uint32_t ms);

/* Touch drag threshold (px) for WPE_SETTING_DRAG_THRESHOLD from the UC_WEBVIEW_DRAG_THRESHOLD value: a decimal
 * 1..200; NULL, empty or anything else returns fallback. WebKit's gesture controller turns a touch sequence into
 * a drag (no click) once a move is farther than this from the press position (WPEGestureDetector.cpp:63-69). */
uint32_t uc_events_drag_threshold     (const char *value, uint32_t fallback);

/* The drag threshold the library sets on its display instead of WPE's 8 px (WPESettings.cpp:106). A remote-ui input
 * trace (Remote 3, Qt evdev touch, 117 one-finger presses into a web view) had 61 taps within 7.1 px, 16 presses of
 * 8.5..13.9 px that WebKit turned into drags and so lost their click (21 % of the 77 short presses), one press of
 * 17.5 px, and 39 scrolls of 77 px and more. At 16 px every one of the 16 is a click and every scroll stays a scroll.
 * On the Remote 3's 480x800 panel of 44x75 mm (about 275 dpi) 16 px are about 1.5 mm, close to Android's touch slop
 * of 8 dp (about 14 px at this density). A scroll starts later and loses no distance: WebKit measures the first
 * scroll delta from the press position (WPEGestureDetector.cpp:66-75).
 * README.md, "Touch drag threshold", has the comparison with the kiosk. */
#define UC_EVENTS_DEFAULT_DRAG_THRESHOLD 16

/* The display's drag threshold: a valid UC_WEBVIEW_DRAG_THRESHOLD value (env_value, a decimal 1..200) or
 * UC_EVENTS_DEFAULT_DRAG_THRESHOLD. *env_invalid (may be NULL) is set to whether a non-empty env_value was
 * refused (the caller logs it). */
uint32_t uc_events_display_drag_threshold (const char *env_value, gboolean *env_invalid);

/* Pointer state: pressed buttons (for the modifier mask of move/release events) and the last position
 * (for the movement delta WPE_EVENT_POINTER_MOVE carries, WPEEvent.h:184-192). */
typedef struct {
    unsigned buttons;        /* UC_WEBVIEW_MOD_BUTTONn bits currently held */
    double   last_x, last_y;
    gboolean has_last;
} UcPointerState;

void     uc_pointer_state_init    (UcPointerState *state);
/* Updates the held buttons; returns the modifiers to put on the event: host mods (keyboard bits) plus
 * the buttons held after this event (press) or before it (release), as libinput based platforms do. */
unsigned uc_pointer_state_button  (UcPointerState *state, int button, gboolean pressed, unsigned host_mods);
/* Movement delta against the last position (0,0 for the first event), then remembers x/y. */
void     uc_pointer_state_move    (UcPointerState *state, double x, double y, double *dx, double *dy);
unsigned uc_pointer_state_mods    (const UcPointerState *state, unsigned host_mods);

/* Keyboard modifier state derived from the key events the host sends, so that WebKit's
 * wpe_keymap_get_modifiers() (WebPageProxyWPE.cpp:206-208) answers without an XKB keymap. */
typedef struct {
    unsigned held;      /* UC_WEBVIEW_MOD_CONTROL/SHIFT/ALT/META from keys currently down */
    gboolean caps_lock; /* toggled by Caps_Lock presses */
} UcKeyState;

void     uc_key_state_init   (UcKeyState *state);
/* Updates the state for keysym (XKB keysym) and returns the effective modifiers: host mods OR the
 * tracked ones. A modifier key's own press does not report itself (like libinput/GTK). */
unsigned uc_key_state_update (UcKeyState *state, uint32_t keysym, gboolean pressed, unsigned host_mods);
unsigned uc_key_state_mods   (const UcKeyState *state);

G_END_DECLS

#endif /* UC_EVENTS_H */
