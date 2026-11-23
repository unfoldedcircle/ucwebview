/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * UcWpeDisplay: the in-process WPEDisplay of libucwebview, modelled on the
 * upstream Qt 6 binding (UIProcess/API/wpe/qt6/WPEDisplayQtQuick.cpp) and the
 * headless platform (WPEPlatform/wpe/headless/WPEDisplayHeadless.cpp).
 *
 *  - connect: resolves the DRM nodes of the host EGLDisplay through
 *    EGL_EXT_device_query/EGL_EXT_device_drm(_render_node) like
 *    WPEDisplayQtQuick.cpp:67-95, but falls back to /dev/dri/renderD128 +
 *    /dev/dri/card0 and, when nothing is found, to no DRM device at all (the
 *    web process then renders into shared memory, AcceleratedSurface.cpp:594-607).
 *  - get_egl_display: the host's EGLDisplay; WPEBufferDMABuf imports every
 *    frame on it (WPEBufferDMABuf.cpp:117-178) and caches the EGLImage in the
 *    buffer until the buffer is disposed (WPEBufferDMABuf.cpp:71-88).
 *  - get_preferred_buffer_formats: ARGB8888/XRGB8888 with the modifiers the
 *    host display can import (uc_formats.c); WebKit intersects this list with
 *    the render device's own (AcceleratedSurface.cpp:270-298).
 *  - get_keymap: a UcWpeKeymap that answers get_modifiers() from the key
 *    events the host sends. Without this vfunc WPEDisplay falls back to
 *    wpe_keymap_xkb_new() (WPEDisplay.cpp:449-461), which compiles the
 *    evdev/pc105/us keymap from /usr/share/X11/xkb (WPEKeymapXKB.cpp:175-185)
 *    and crashes in xkb_state_new() when that data is missing, so this is
 *    what makes the library independent of xkeyboard-config.
 *  - one UcWpeScreen (WebKit's ScreenManagerWPE.cpp:43-49 reads them from the
 *    primary display) with the display refresh rate and a sync observer that
 *    ticks WebKit's display link at that rate (uc_wpe_screen.c).
 *  - create_input_method_context: UcWpeIme (uc_wpe_ime.c).
 *
 * Not a GIO module: created with g_object_new() and passed to the
 * WebKitWebView through its construct-only "display" property
 * (WebKitWebView.cpp:1368-1370), so WPE_PLATFORM is irrelevant and
 * wpe_display_get_default() is never called. The first display constructed
 * becomes the primary one (WPEDisplay.cpp:125-129).
 */
#include "uc_egl.h"
#include "uc_formats.h"
#include "uc_internal.h"
#include "uc_refresh.h"

/* ---- UcWpeKeymap ------------------------------------------------------------ */

#define UC_TYPE_WPE_KEYMAP (uc_wpe_keymap_get_type ())
G_DECLARE_FINAL_TYPE (UcWpeKeymap, uc_wpe_keymap, UC, WPE_KEYMAP, WPEKeymap)

struct _UcWpeKeymap {
    WPEKeymap parent;
    unsigned mods;
};

G_DEFINE_FINAL_TYPE (UcWpeKeymap, uc_wpe_keymap, WPE_TYPE_KEYMAP)

/* Only WebDriver (WebAutomationSessionWPE.cpp:235-246) asks for keycodes of a keysym; not supported. */
static gboolean
uc_wpe_keymap_get_entries_for_keyval (WPEKeymap *keymap, guint keyval, WPEKeymapEntry **entries, guint *n_entries)
{
    (void) keymap; (void) keyval;
    if (entries != NULL)
        *entries = NULL;
    if (n_entries != NULL)
        *n_entries = 0;
    return FALSE;
}

static gboolean
uc_wpe_keymap_translate_keyboard_state (WPEKeymap *keymap, guint keycode, WPEModifiers modifiers, int group,
                                        guint *keyval, int *effective_group, int *level, WPEModifiers *consumed)
{
    (void) keymap; (void) keycode; (void) modifiers; (void) group;
    if (keyval != NULL)
        *keyval = 0;
    if (effective_group != NULL)
        *effective_group = 0;
    if (level != NULL)
        *level = 0;
    if (consumed != NULL)
        *consumed = (WPEModifiers) 0;
    return FALSE;
}

static WPEModifiers
uc_wpe_keymap_get_modifiers (WPEKeymap *keymap)
{
    return (WPEModifiers) UC_WPE_KEYMAP (keymap)->mods;
}

static void
uc_wpe_keymap_class_init (UcWpeKeymapClass *klass)
{
    WPEKeymapClass *keymap_class = WPE_KEYMAP_CLASS (klass);
    /* All three vfuncs are called unconditionally by WPEKeymap.cpp, so all must exist. */
    keymap_class->get_entries_for_keyval = uc_wpe_keymap_get_entries_for_keyval;
    keymap_class->translate_keyboard_state = uc_wpe_keymap_translate_keyboard_state;
    keymap_class->get_modifiers = uc_wpe_keymap_get_modifiers;
}

static void
uc_wpe_keymap_init (UcWpeKeymap *keymap)
{
    keymap->mods = 0;
}

/* ---- UcWpeDisplay ----------------------------------------------------------- */

struct _UcWpeDisplay {
    WPEDisplay parent;
    gpointer egl_display;
    WPEDRMDevice *drm_device;      /* NULL: software rendering / SHM */
    UcWpeKeymap *keymap;
    UcWpeScreen *screen;
    int next_width, next_height;
    double next_scale;
};

G_DEFINE_FINAL_TYPE (UcWpeDisplay, uc_wpe_display, WPE_TYPE_DISPLAY)

static gboolean
path_exists (const char *path, gpointer user_data)
{
    (void) user_data;
    return g_file_test (path, G_FILE_TEST_EXISTS);
}

static gboolean
uc_wpe_display_connect (WPEDisplay *display, GError **error)
{
    UcWpeDisplay *self = UC_WPE_DISPLAY (display);
    char *egl_primary = NULL, *egl_render = NULL, *primary = NULL, *render = NULL;

    if (self->egl_display != NULL) {
        const char *ext = uc_egl_display_extensions (self->egl_display);
        if (!uc_egl_extension_in_string (ext, "EGL_KHR_image_base")) {
            g_set_error_literal (error, WPE_DISPLAY_ERROR, WPE_DISPLAY_ERROR_CONNECTION_FAILED,
                                 "the host EGLDisplay lacks EGL_KHR_image_base (not initialised?)");
            return FALSE;
        }
        if (!uc_egl_extension_in_string (ext, "EGL_EXT_image_dma_buf_import"))
            g_warning ("ucwebview: the host EGLDisplay lacks EGL_EXT_image_dma_buf_import, frames cannot be imported");
        if (!uc_egl_query_drm_nodes (self->egl_display, &egl_primary, &egl_render))
            g_debug ("ucwebview: EGL_EXT_device_query/EGL_EXT_device_drm unavailable, trying fixed DRM nodes");
    }

    if (uc_egl_choose_drm_nodes (egl_primary, egl_render, path_exists, NULL, &primary, &render)) {
        /* wpe_drm_device_new(primary, render): what the DRM/headless platforms hand to the web process
         * (WPEDisplayDRM.cpp:371-372, WPEDisplayHeadless.cpp:163-170); the web process opens the render node. */
        self->drm_device = wpe_drm_device_new (primary, render);
        g_message ("ucwebview: render device primary=%s render=%s", primary != NULL ? primary : "-",
                   render != NULL ? render : "-");
    } else {
        g_message ("ucwebview: no DRM render node, the web process will render into shared memory");
    }
    g_free (egl_primary);
    g_free (egl_render);
    g_free (primary);
    g_free (render);

    /* pointer: coarse / hover: none media queries and touch event support (WebProcessPoolGLib.cpp:118-120) */
    wpe_display_set_available_input_devices (display, WPE_AVAILABLE_INPUT_DEVICE_TOUCHSCREEN | WPE_AVAILABLE_INPUT_DEVICE_KEYBOARD);
    return TRUE;
}

static WPEView *
uc_wpe_display_create_view (WPEDisplay *display)
{
    /* The toplevel is attached by wpeDisplayCreateView() through create_toplevel below
     * (WPEDisplay.cpp:246-255, WPE_SETTING_CREATE_VIEWS_WITH_A_TOPLEVEL defaults to true, WPESettings.cpp:107). */
    return uc_wpe_view_new (UC_WPE_DISPLAY (display));
}

static WPEToplevel *
uc_wpe_display_create_toplevel (WPEDisplay *display, guint max_views)
{
    UcWpeDisplay *self = UC_WPE_DISPLAY (display);
    (void) max_views; /* one view per toplevel */
    return uc_wpe_toplevel_new (self, self->next_width, self->next_height, self->next_scale);
}

static gpointer
uc_wpe_display_get_egl_display (WPEDisplay *display, GError **error)
{
    UcWpeDisplay *self = UC_WPE_DISPLAY (display);
    if (self->egl_display == NULL) {
        g_set_error_literal (error, WPE_EGL_ERROR, WPE_EGL_ERROR_NOT_AVAILABLE, "no host EGLDisplay (shared memory mode)");
        return NULL;
    }
    return self->egl_display;
}

static WPEKeymap *
uc_wpe_display_get_keymap (WPEDisplay *display)
{
    UcWpeDisplay *self = UC_WPE_DISPLAY (display);
    /* WPEDisplay keeps the returned reference (WPEDisplay.cpp:454-461). */
    return WPE_KEYMAP (g_object_ref (self->keymap));
}

static WPEBufferFormats *
uc_wpe_display_get_preferred_buffer_formats (WPEDisplay *display)
{
    UcWpeDisplay *self = UC_WPE_DISPLAY (display);
    guint n_egl = 0;
    UcFormatEntry *egl_formats = uc_egl_query_dmabuf_formats (self->egl_display, &n_egl);
    GArray *selected = uc_formats_select (egl_formats, n_egl);
    WPEBufferFormatsBuilder *builder = wpe_buffer_formats_builder_new (self->drm_device);
    WPEBufferFormats *formats;

    /* one RENDERING group on the render device; NULL device = the builder's device (WPEBufferFormats.cpp) */
    wpe_buffer_formats_builder_append_group (builder, NULL, WPE_BUFFER_FORMAT_USAGE_RENDERING);
    for (guint i = 0; i < selected->len; i++) {
        const UcFormatEntry *entry = &g_array_index (selected, UcFormatEntry, i);
        char fourcc[5];
        for (guint m = 0; m < entry->n_modifiers; m++)
            wpe_buffer_formats_builder_append_format (builder, entry->fourcc, entry->modifiers[m]);
        g_debug ("ucwebview: advertising %s with %u modifier(s)", uc_fourcc_to_string (entry->fourcc, fourcc), entry->n_modifiers);
    }
    formats = wpe_buffer_formats_builder_end (builder);
    uc_formats_free (selected);
    uc_egl_formats_free (egl_formats, n_egl);
    return formats;
}

static guint
uc_wpe_display_get_n_screens (WPEDisplay *display)
{
    (void) display;
    return 1;
}

static WPEScreen *
uc_wpe_display_get_screen (WPEDisplay *display, guint index)
{
    UcWpeDisplay *self = UC_WPE_DISPLAY (display);
    return index == 0 ? WPE_SCREEN (self->screen) : NULL;
}

static WPEDRMDevice *
uc_wpe_display_get_drm_device (WPEDisplay *display)
{
    return UC_WPE_DISPLAY (display)->drm_device;
}

static WPEInputMethodContext *
uc_wpe_display_create_input_method_context (WPEDisplay *display, WPEView *view)
{
    WPEInputMethodContext *ime;
    (void) display;
    if (!UC_IS_WPE_VIEW (view))
        return NULL; /* WPEDisplay.cpp:270-272 substitutes the "none" context */
    ime = uc_wpe_ime_new (view);
    uc_wpe_view_set_ime (UC_WPE_VIEW (view), ime);
    return ime;
}

static void
uc_wpe_display_dispose (GObject *object)
{
    UcWpeDisplay *self = UC_WPE_DISPLAY (object);
    g_clear_pointer (&self->drm_device, wpe_drm_device_unref);
    g_clear_object (&self->keymap);
    g_clear_object (&self->screen);
    G_OBJECT_CLASS (uc_wpe_display_parent_class)->dispose (object);
}

static void
uc_wpe_display_class_init (UcWpeDisplayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    WPEDisplayClass *display_class = WPE_DISPLAY_CLASS (klass);

    object_class->dispose = uc_wpe_display_dispose;
    display_class->connect = uc_wpe_display_connect;
    display_class->create_view = uc_wpe_display_create_view;
    display_class->create_toplevel = uc_wpe_display_create_toplevel;
    display_class->get_egl_display = uc_wpe_display_get_egl_display;
    display_class->get_keymap = uc_wpe_display_get_keymap;
    display_class->get_preferred_buffer_formats = uc_wpe_display_get_preferred_buffer_formats;
    display_class->get_n_screens = uc_wpe_display_get_n_screens;
    display_class->get_screen = uc_wpe_display_get_screen;
    display_class->get_drm_device = uc_wpe_display_get_drm_device;
    display_class->create_input_method_context = uc_wpe_display_create_input_method_context;
    /* use_explicit_sync stays NULL (FALSE): the host samples the EGLImage after the rendering fence
     * monitor of the UI process fired (AcceleratedBackingStore.cpp:201-210). */
}

static void
uc_wpe_display_init (UcWpeDisplay *self)
{
    self->keymap = g_object_new (UC_TYPE_WPE_KEYMAP, NULL);
    self->screen = uc_wpe_screen_new (1);
    uc_wpe_screen_set_refresh_rate (self->screen, UC_REFRESH_DEFAULT_MHZ);
    self->next_width = 1;
    self->next_height = 1;
    self->next_scale = 1.0;
}

UcWpeDisplay *
uc_wpe_display_new (gpointer egl_display)
{
    UcWpeDisplay *self = g_object_new (UC_TYPE_WPE_DISPLAY, NULL);
    self->egl_display = egl_display;
    return self;
}

gpointer
uc_wpe_display_get_egl (UcWpeDisplay *display)
{
    return display->egl_display;
}

void
uc_wpe_display_set_next_view (UcWpeDisplay *display, int width, int height, double scale)
{
    display->next_width = width;
    display->next_height = height;
    display->next_scale = scale;
    /* The virtual screen takes the size of the view: window.screen matches the embedded view. */
    wpe_screen_set_size (WPE_SCREEN (display->screen), width, height);
    wpe_screen_set_physical_size (WPE_SCREEN (display->screen),
                                  (int) (width * 25.4 / 160.0 + 0.5), (int) (height * 25.4 / 160.0 + 0.5)); /* 160 dpi */
    wpe_screen_set_scale (WPE_SCREEN (display->screen), scale);
}

void
uc_wpe_display_set_refresh_rate (UcWpeDisplay *display, int refresh_mhz)
{
    uc_wpe_screen_set_refresh_rate (display->screen, refresh_mhz);
}

void
uc_wpe_display_set_vsync_trace (UcWpeDisplay *display, gboolean trace)
{
    uc_wpe_screen_set_vsync_trace (display->screen, trace);
}

void
uc_wpe_display_set_key_mods (UcWpeDisplay *display, unsigned mods)
{
    display->keymap->mods = mods & UC_EVENTS_KEYBOARD_MODS;
}

WPEScreen *
uc_wpe_display_get_screen0 (UcWpeDisplay *display)
{
    return WPE_SCREEN (display->screen);
}
