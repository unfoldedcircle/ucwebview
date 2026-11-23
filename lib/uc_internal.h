/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Private declarations shared by the libucwebview sources that include the WPE
 * headers (uc_webview.c, uc_wpe_display.c, uc_wpe_view.c, uc_wpe_ime.c).
 * Everything here runs on the UI thread.
 */
#ifndef UC_INTERNAL_H
#define UC_INTERNAL_H

#include "uc_events.h"
#include "uc_ime_state.h"
#include "uc_pinch.h"
#include "uc_webview.h"

#include <glib-object.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

#define UC_EXPORT __attribute__((visibility("default")))

/* ---- UcWpeScreen / UcWpeSyncObserver (uc_wpe_screen.c) ---------------------- */

#define UC_TYPE_WPE_SCREEN (uc_wpe_screen_get_type ())
G_DECLARE_FINAL_TYPE (UcWpeScreen, uc_wpe_screen, UC, WPE_SCREEN, WPEScreen)

/* A virtual screen with the given id (> 0) and a sync observer that ticks at its refresh rate + 1 % (uc_refresh.h). */
UcWpeScreen  *uc_wpe_screen_new              (guint32 id);
/* WPEScreen:refresh-rate in mHz (> 0); the sync observer ticks 1 % faster. */
void          uc_wpe_screen_set_refresh_rate (UcWpeScreen *screen, int refresh_mhz);
void          uc_wpe_screen_set_vsync_trace  (UcWpeScreen *screen, gboolean trace);

/* ---- UcWpeDisplay / UcWpeKeymap (uc_wpe_display.c) ----------------------------- */

#define UC_TYPE_WPE_DISPLAY (uc_wpe_display_get_type ())
G_DECLARE_FINAL_TYPE (UcWpeDisplay, uc_wpe_display, UC, WPE_DISPLAY, WPEDisplay)

/* egl_display may be NULL (shared memory path). Call wpe_display_connect() afterwards. */
UcWpeDisplay *uc_wpe_display_new             (gpointer egl_display);
gpointer      uc_wpe_display_get_egl         (UcWpeDisplay *display);
/* Size/scale the next create_view() (WebKitWebView construction) starts with. */
void          uc_wpe_display_set_next_view   (UcWpeDisplay *display, int width, int height, double scale);
/* Modifier state reported by wpe_display_get_keymap()->get_modifiers (WebPageProxyWPE.cpp:206-208). */
void          uc_wpe_display_set_key_mods    (UcWpeDisplay *display, unsigned mods);
/* Refresh rate of the screen in mHz (> 0), see uc_wpe_screen_set_refresh_rate() (frame pacing). */
void          uc_wpe_display_set_refresh_rate (UcWpeDisplay *display, int refresh_mhz);
/* g_message() lines about the vsync ticks (UC_WEBVIEW_TRACE=1). */
void          uc_wpe_display_set_vsync_trace (UcWpeDisplay *display, gboolean trace);
WPEScreen    *uc_wpe_display_get_screen0     (UcWpeDisplay *display);

/* ---- UcWpeView / UcWpeToplevel (uc_wpe_view.c) ---------------------------- */

#define UC_TYPE_WPE_VIEW (uc_wpe_view_get_type ())
G_DECLARE_FINAL_TYPE (UcWpeView, uc_wpe_view, UC, WPE_VIEW, WPEView)

#define UC_TYPE_WPE_TOPLEVEL (uc_wpe_toplevel_get_type ())
G_DECLARE_FINAL_TYPE (UcWpeToplevel, uc_wpe_toplevel, UC, WPE_TOPLEVEL, WPEToplevel)

WPEView     *uc_wpe_view_new          (UcWpeDisplay *display);
WPEToplevel *uc_wpe_toplevel_new      (UcWpeDisplay *display, int width, int height, double scale);
void         uc_wpe_view_set_owner    (UcWpeView *view, uc_webview *owner);
uc_webview  *uc_wpe_view_get_owner    (UcWpeView *view);
/* Weak pointer to the input method context WebKit created for this view (one per WebKitWebView). */
void         uc_wpe_view_set_ime      (UcWpeView *view, WPEInputMethodContext *ime);
WPEInputMethodContext *uc_wpe_view_get_ime (UcWpeView *view);

/* ---- UcWpeIme (uc_wpe_ime.c) ------------------------------------------------ */

#define UC_TYPE_WPE_IME (uc_wpe_ime_get_type ())
G_DECLARE_FINAL_TYPE (UcWpeIme, uc_wpe_ime, UC, WPE_IME, WPEInputMethodContext)

WPEInputMethodContext *uc_wpe_ime_new                (WPEView *view);
gboolean               uc_wpe_ime_commit             (UcWpeIme *ime, const char *utf8);
gboolean               uc_wpe_ime_preedit            (UcWpeIme *ime, const char *utf8, int cursor);
gboolean               uc_wpe_ime_delete_surrounding (UcWpeIme *ime, int offset, int n_chars);
gboolean               uc_wpe_ime_is_focused         (UcWpeIme *ime);
/* The web process of the page's document is gone (library 0.7.5): a focused element is reported gone, and WebKit's
 * input method state stays stale until the new document's first one (uc_ime_state.h, "Focus reports").
 * webkit_context: the view's WebKitInputMethodContext, which wraps ime (may be NULL). */
void                   uc_wpe_ime_web_process_gone   (UcWpeIme *ime, WebKitInputMethodContext *webkit_context);

/* ---- UcGestureController (uc_wpe_gesture.c, library 0.6.0) ------------------- */

#define UC_TYPE_GESTURE_CONTROLLER (uc_gesture_controller_get_type ())
G_DECLARE_FINAL_TYPE (UcGestureController, uc_gesture_controller, UC, GESTURE_CONTROLLER, GObject)

/* Replaces the view's gesture controller with one that wraps WebKit's default controller and reports two-finger
 * pinches through func (uc_pinch.h; threshold in px). Returns a reference for the caller (the view holds another),
 * NULL when the view has no controller. */
UcGestureController *uc_gesture_controller_install (WPEView *view, double threshold, UcPinchFunc func,
                                                    gpointer user_data);
/* A navigation committed: a running pinch ends with CANCEL (uc_pinch_abort()). */
void                 uc_gesture_controller_abort   (UcGestureController *controller);
/* No more reports (the view is freed; WebKit may still deliver touch replies). */
void                 uc_gesture_controller_detach  (UcGestureController *controller);

/* ---- callbacks from the WPE subclasses into the public object (uc_webview.c) */

gboolean uc_webview_internal_render_buffer   (uc_webview *wv, WPEBuffer *buffer, GError **error);
void     uc_webview_internal_cursor          (uc_webview *wv, const char *name);
void     uc_webview_internal_ime_focus       (uc_webview *wv, gboolean focused, unsigned hints, unsigned purpose,
                                              const char *surrounding, int cursor);
void     uc_webview_internal_ime_cursor_area (uc_webview *wv, int x, int y, int w, int h);
void     uc_webview_internal_ime_surrounding (uc_webview *wv, const char *text, int cursor, int anchor);

G_END_DECLS

#endif /* UC_INTERNAL_H */
