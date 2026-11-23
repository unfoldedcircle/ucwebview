/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Device state for web pages (library 0.9.0), the part without WebKit: the host's state JSON, its validation and the
 * state a view keeps, the start script with the state in it, the changes the library sends to its web process
 * extension (uc_device_wk.c sends them, uc_ext_protocol.h has the protocol) and the JSON the extension makes of a
 * change for the shim (uc_ext_device.c). GLib only, host testable (tests/test-device.c); the library and the extension
 * both compile it in.
 *
 * State JSON (uc_webview_set_device_state()), every member optional, unknown members ignored:
 *   {"screen": "on" | "dim" | "off", "powerSource": "ac" | "battery",
 *    "powerProfile": "power-saver" | "balanced" | "performance", "level": 0..100}
 * level is the battery level in percent, an integer. A member with an invalid value is ignored (the view keeps its
 * value); input that is not one JSON object of at most UC_DEVICE_JSON_MAX bytes is refused as a whole. The members
 * are those of the shim's change (device-host-shim.js), where the page's battery is charging while powerSource is "ac".
 *
 * Also here: the WebKit features of the hidden page throttling every view gets since 0.9.0 (uc_webview.c applies
 * them, tests/test-device.c finds them in WebKitGTK's list).
 */
#ifndef UC_DEVICE_H
#define UC_DEVICE_H

#include <glib.h>

G_BEGIN_DECLS

/* WebKit feature identifiers (webkit_feature_get_identifier(): the preference key without "Enabled",
 * WebKitFeature.cpp:126-135) of HiddenPageDOMTimerThrottlingEnabled and HiddenPageCSSAnimationSuspensionEnabled
 * (UnifiedWebPreferences.yaml: on by default for GTK and Cocoa only). A hidden page's timers then fire at most once a
 * second and its CSS animations stop. */
#define UC_DEVICE_FEATURE_TIMER_THROTTLING   "HiddenPageDOMTimerThrottling"
#define UC_DEVICE_FEATURE_ANIMATION_SUSPEND  "HiddenPageCSSAnimationSuspension"

/* the longest state JSON taken (the state is about 150 bytes) */
#define UC_DEVICE_JSON_MAX 4096

typedef enum { UC_DEVICE_SCREEN_ON, UC_DEVICE_SCREEN_DIM, UC_DEVICE_SCREEN_OFF } UcDeviceScreen;
typedef enum { UC_DEVICE_SOURCE_AC, UC_DEVICE_SOURCE_BATTERY } UcDeviceSource;
typedef enum {
    UC_DEVICE_PROFILE_POWER_SAVER,
    UC_DEVICE_PROFILE_BALANCED,
    UC_DEVICE_PROFILE_PERFORMANCE,
} UcDeviceProfile;
typedef enum { UC_DEVICE_EVENT_SUSPEND, UC_DEVICE_EVENT_RESUME } UcDeviceEvent;

typedef struct {
    UcDeviceScreen screen;
    UcDeviceSource source;
    UcDeviceProfile profile;
    int level;              /* percent, 0..100 */
} UcDeviceState;

/* The state of a view before the host's first one: screen on, AC, balanced, level 100 (the shim's defaults; the page
 * sees a charging battery at full level, the Battery Status API's values for "no battery information"). */
void      uc_device_state_init     (UcDeviceState *state);
gboolean  uc_device_state_equal    (const UcDeviceState *a, const UcDeviceState *b);

/* Merges the members of json into state. FALSE (state unchanged, *error a static reason) when json is NULL, longer
 * than UC_DEVICE_JSON_MAX, not UTF-8, not valid JSON (RFC 8259; no duplicate member names, at most 16 levels deep) or
 * not an object. TRUE otherwise; the names of members with an invalid value, which keep their value, go to ignored
 * (comma separated, may be NULL). */
gboolean  uc_device_state_merge_json (UcDeviceState *state, const char *json, GString *ignored, const char **error);

/* "screen dim, battery, balanced, 42 %" for the log */
char     *uc_device_state_describe (const UcDeviceState *state);

/* The user script that starts the device API in a document while it is on: top frame, document start, the library's
 * script world (UC_EXT_HOST_WORLD), where it calls the extension's UC_EXT_DEVICE_START with state, written as a JSON
 * object of names of the tables and an integer (never text of the host's input). */
char     *uc_device_start_script   (const UcDeviceState *state);

/* A change for the shown document (UC_EXT_DEVICE_MESSAGE), a floating a{sv}: screen, powerSource, powerProfile (s),
 * level (i) of state; for an event also event ("suspend" | "resume") and, for resume, sleptMs (d, slept_ms < 0 counts
 * as 0). The shim applies the state before freeze and suspend, and after both resume events. */
GVariant *uc_device_state_change   (const UcDeviceState *state);
GVariant *uc_device_event_change   (const UcDeviceState *state, UcDeviceEvent event, gint64 slept_ms);

/* The extension's side: change (an a{sv} as above) as a JSON object for the shim's update function, with only the
 * members it knows and only valid values (a name of the tables, a level 0..100, a finite sleptMs >= 0); other members
 * are left out. NULL when change is not an a{sv}. */
char     *uc_device_change_json    (GVariant *change);

/* "suspend" or "resume"; FALSE for anything else */
gboolean  uc_device_event_parse    (const char *type, UcDeviceEvent *event);

G_END_DECLS

#endif /* UC_DEVICE_H */
