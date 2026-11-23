/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * UcWpeView / UcWpeToplevel: the WPEView that hands rendered buffers to the
 * host and the WPEToplevel that carries the view's size, scale and state.
 * Template: UIProcess/API/wpe/qt6/WPEViewQtQuick.cpp:204-213, 269-283 and
 * WPEToplevelQtQuick.cpp:612-634, plus WPEToplevelHeadless.cpp:414-419 for
 * the ACTIVE state (WebKit maps it to ActivityState::WindowIsActive,
 * WPEWebViewPlatform.cpp:69-73).
 */
#include "uc_internal.h"

/* The C API mirrors WPEPlatform values; keep them in lock step with the real headers. */
_Static_assert ((int) UC_WPE_EVENT_POINTER_DOWN == (int) WPE_EVENT_POINTER_DOWN, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_POINTER_UP == (int) WPE_EVENT_POINTER_UP, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_POINTER_MOVE == (int) WPE_EVENT_POINTER_MOVE, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_SCROLL == (int) WPE_EVENT_SCROLL, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_KEYBOARD_KEY_DOWN == (int) WPE_EVENT_KEYBOARD_KEY_DOWN, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_KEYBOARD_KEY_UP == (int) WPE_EVENT_KEYBOARD_KEY_UP, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_TOUCH_DOWN == (int) WPE_EVENT_TOUCH_DOWN, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_TOUCH_UP == (int) WPE_EVENT_TOUCH_UP, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_TOUCH_MOVE == (int) WPE_EVENT_TOUCH_MOVE, "WPEEventType");
_Static_assert ((int) UC_WPE_EVENT_TOUCH_CANCEL == (int) WPE_EVENT_TOUCH_CANCEL, "WPEEventType");
_Static_assert ((int) UC_WEBVIEW_MOD_CONTROL == (int) WPE_MODIFIER_KEYBOARD_CONTROL, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_SHIFT == (int) WPE_MODIFIER_KEYBOARD_SHIFT, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_ALT == (int) WPE_MODIFIER_KEYBOARD_ALT, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_META == (int) WPE_MODIFIER_KEYBOARD_META, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_CAPS_LOCK == (int) WPE_MODIFIER_KEYBOARD_CAPS_LOCK, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_BUTTON1 == (int) WPE_MODIFIER_POINTER_BUTTON1, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_BUTTON2 == (int) WPE_MODIFIER_POINTER_BUTTON2, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_BUTTON3 == (int) WPE_MODIFIER_POINTER_BUTTON3, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_MOD_BUTTON5 == (int) WPE_MODIFIER_POINTER_BUTTON5, "WPEModifiers");
_Static_assert ((int) UC_WEBVIEW_BUTTON_PRIMARY == (int) WPE_BUTTON_PRIMARY, "WPE_BUTTON");
_Static_assert ((int) UC_WEBVIEW_BUTTON_MIDDLE == (int) WPE_BUTTON_MIDDLE, "WPE_BUTTON");
_Static_assert ((int) UC_WEBVIEW_BUTTON_SECONDARY == (int) WPE_BUTTON_SECONDARY, "WPE_BUTTON");
_Static_assert ((int) UC_WEBVIEW_LOAD_STARTED == (int) WEBKIT_LOAD_STARTED, "WebKitLoadEvent");
_Static_assert ((int) UC_WEBVIEW_LOAD_REDIRECTED == (int) WEBKIT_LOAD_REDIRECTED, "WebKitLoadEvent");
_Static_assert ((int) UC_WEBVIEW_LOAD_COMMITTED == (int) WEBKIT_LOAD_COMMITTED, "WebKitLoadEvent");
_Static_assert ((int) UC_WEBVIEW_LOAD_FINISHED == (int) WEBKIT_LOAD_FINISHED, "WebKitLoadEvent");
_Static_assert ((int) UC_WEBVIEW_TERMINATED_CRASHED == (int) WEBKIT_WEB_PROCESS_CRASHED, "WebKitWebProcessTerminationReason");
_Static_assert ((int) UC_WEBVIEW_TERMINATED_EXCEEDED_MEMORY_LIMIT == (int) WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT, "WebKitWebProcessTerminationReason");
_Static_assert ((int) UC_WEBVIEW_TERMINATED_BY_API == (int) WEBKIT_WEB_PROCESS_TERMINATED_BY_API, "WebKitWebProcessTerminationReason");
_Static_assert ((int) UC_WEBVIEW_IME_PURPOSE_FREE_FORM == (int) WPE_INPUT_PURPOSE_FREE_FORM, "WPEInputPurpose");
_Static_assert ((int) UC_WEBVIEW_IME_PURPOSE_DIGITS == (int) WPE_INPUT_PURPOSE_DIGITS, "WPEInputPurpose");
_Static_assert ((int) UC_WEBVIEW_IME_PURPOSE_URL == (int) WPE_INPUT_PURPOSE_URL, "WPEInputPurpose");
_Static_assert ((int) UC_WEBVIEW_IME_PURPOSE_EMAIL == (int) WPE_INPUT_PURPOSE_EMAIL, "WPEInputPurpose");
_Static_assert ((int) UC_WEBVIEW_IME_PURPOSE_PASSWORD == (int) WPE_INPUT_PURPOSE_PASSWORD, "WPEInputPurpose");
_Static_assert ((int) UC_WEBVIEW_IME_PURPOSE_TERMINAL == (int) WPE_INPUT_PURPOSE_TERMINAL, "WPEInputPurpose");
_Static_assert ((int) UC_WEBVIEW_IME_PURPOSE_SEARCH == (int) WPE_INPUT_PURPOSE_SEARCH, "WPEInputPurpose");
_Static_assert ((int) UC_WEBVIEW_IME_HINT_SPELLCHECK == (int) WPE_INPUT_HINT_SPELLCHECK, "WPEInputHints");
_Static_assert ((int) UC_WEBVIEW_IME_HINT_LOWERCASE == (int) WPE_INPUT_HINT_LOWERCASE, "WPEInputHints");
_Static_assert ((int) UC_WEBVIEW_IME_HINT_INHIBIT_OSK == (int) WPE_INPUT_HINT_INHIBIT_OSK, "WPEInputHints");
_Static_assert ((int) UC_WEBVIEW_IME_HINT_PRIVATE == (int) WPE_INPUT_HINT_PRIVATE, "WPEInputHints");

/* ---- UcWpeView -------------------------------------------------------------- */

struct _UcWpeView {
    WPEView parent;
    uc_webview *owner;              /* set by uc_webview_new() after WebKit created the view */
    WPEInputMethodContext *ime;     /* weak */
};

G_DEFINE_FINAL_TYPE (UcWpeView, uc_wpe_view, WPE_TYPE_VIEW)

static gboolean
uc_wpe_view_render_buffer (WPEView *view, WPEBuffer *buffer, const WPERectangle *damage_rects, guint n_damage_rects, GError **error)
{
    UcWpeView *self = UC_WPE_VIEW (view);
    (void) damage_rects; (void) n_damage_rects; /* the host always samples the whole buffer */

    if (self->owner == NULL) {
        /* A frame before the owner is attached cannot happen in practice (the view is unmapped until then),
         * but keep WebKit going if it does: report it rendered and released right away. */
        wpe_view_buffer_rendered (view, buffer);
        wpe_view_buffer_released (view, buffer);
        return TRUE;
    }
    return uc_webview_internal_render_buffer (self->owner, buffer, error);
}

static void
uc_wpe_view_set_cursor_from_name (WPEView *view, const char *name)
{
    UcWpeView *self = UC_WPE_VIEW (view);
    if (self->owner != NULL)
        uc_webview_internal_cursor (self->owner, name);
}

static gboolean
uc_wpe_view_can_be_mapped (WPEView *view)
{
    (void) view;
    return TRUE;
}

static void
uc_wpe_view_dispose (GObject *object)
{
    UcWpeView *self = UC_WPE_VIEW (object);
    if (self->ime != NULL) {
        g_object_remove_weak_pointer (G_OBJECT (self->ime), (gpointer *) &self->ime);
        self->ime = NULL;
    }
    G_OBJECT_CLASS (uc_wpe_view_parent_class)->dispose (object);
}

static void
uc_wpe_view_class_init (UcWpeViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    WPEViewClass *view_class = WPE_VIEW_CLASS (klass);

    object_class->dispose = uc_wpe_view_dispose;
    view_class->render_buffer = uc_wpe_view_render_buffer;
    view_class->set_cursor_from_name = uc_wpe_view_set_cursor_from_name;
    view_class->can_be_mapped = uc_wpe_view_can_be_mapped;
}

static void
uc_wpe_view_init (UcWpeView *self)
{
    self->owner = NULL;
    self->ime = NULL;
}

WPEView *
uc_wpe_view_new (UcWpeDisplay *display)
{
    return WPE_VIEW (g_object_new (UC_TYPE_WPE_VIEW, "display", display, NULL));
}

void
uc_wpe_view_set_owner (UcWpeView *view, uc_webview *owner)
{
    view->owner = owner;
}

uc_webview *
uc_wpe_view_get_owner (UcWpeView *view)
{
    return view->owner;
}

void
uc_wpe_view_set_ime (UcWpeView *view, WPEInputMethodContext *ime)
{
    if (view->ime != NULL)
        g_object_remove_weak_pointer (G_OBJECT (view->ime), (gpointer *) &view->ime);
    view->ime = ime;
    if (ime != NULL)
        g_object_add_weak_pointer (G_OBJECT (ime), (gpointer *) &view->ime);
}

WPEInputMethodContext *
uc_wpe_view_get_ime (UcWpeView *view)
{
    return view->ime;
}

/* ---- UcWpeToplevel ---------------------------------------------------------- */

struct _UcWpeToplevel {
    WPEToplevel parent;
};

G_DEFINE_FINAL_TYPE (UcWpeToplevel, uc_wpe_toplevel, WPE_TYPE_TOPLEVEL)

static gboolean
resize_view (WPEToplevel *toplevel, WPEView *view, gpointer user_data)
{
    int width, height;
    (void) user_data;
    wpe_toplevel_get_size (toplevel, &width, &height);
    wpe_view_resized (view, width, height); /* emits WPEView::resized -> WebKit setSize (WPEWebViewPlatform.cpp:84-87) */
    return FALSE;
}

static gboolean
uc_wpe_toplevel_resize (WPEToplevel *toplevel, int width, int height)
{
    /* WPEToplevelQtQuick.cpp:612-622 */
    wpe_toplevel_resized (toplevel, width, height);
    wpe_toplevel_foreach_view (toplevel, resize_view, NULL);
    return TRUE;
}

static gboolean
uc_wpe_toplevel_set_fullscreen (WPEToplevel *toplevel, gboolean fullscreen)
{
    /* The host decides how big the view is; just reflect the state (WPEToplevelHeadless.cpp:433-442). */
    WPEToplevelState state = wpe_toplevel_get_state (toplevel);
    if (fullscreen)
        state |= WPE_TOPLEVEL_STATE_FULLSCREEN;
    else
        state &= ~WPE_TOPLEVEL_STATE_FULLSCREEN;
    wpe_toplevel_state_changed (toplevel, state);
    return TRUE;
}

static WPEScreen *
uc_wpe_toplevel_get_screen (WPEToplevel *toplevel)
{
    WPEDisplay *display = wpe_toplevel_get_display (toplevel);
    return UC_IS_WPE_DISPLAY (display) ? uc_wpe_display_get_screen0 (UC_WPE_DISPLAY (display)) : NULL;
}

static void
uc_wpe_toplevel_class_init (UcWpeToplevelClass *klass)
{
    WPEToplevelClass *toplevel_class = WPE_TOPLEVEL_CLASS (klass);
    toplevel_class->resize = uc_wpe_toplevel_resize;
    toplevel_class->set_fullscreen = uc_wpe_toplevel_set_fullscreen;
    toplevel_class->get_screen = uc_wpe_toplevel_get_screen;
    /* get_preferred_buffer_formats stays NULL: wpe_toplevel_get_preferred_buffer_formats() falls back to the
     * display's list (WPEToplevel.cpp:650-657). */
}

static void
uc_wpe_toplevel_init (UcWpeToplevel *self)
{
    (void) self;
}

WPEToplevel *
uc_wpe_toplevel_new (UcWpeDisplay *display, int width, int height, double scale)
{
    WPEToplevel *toplevel = WPE_TOPLEVEL (g_object_new (UC_TYPE_WPE_TOPLEVEL, "display", display, "max-views", 1, NULL));
    wpe_toplevel_resized (toplevel, width, height);
    wpe_toplevel_scale_changed (toplevel, scale);
    wpe_toplevel_state_changed (toplevel, WPE_TOPLEVEL_STATE_ACTIVE);
    return toplevel;
}
