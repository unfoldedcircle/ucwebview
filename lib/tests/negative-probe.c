/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Deliberately wrong code for `make syntax-check-negative`: every statement
 * must be rejected by the compiler against the generated 2.54.1 headers,
 * which proves that `make syntax-check` really checks the WPE/WebKit API use
 * of the library (functions that do not exist, wrong argument types, vfuncs
 * that are not in the class struct).
 */
#include "uc_internal.h"

void
probe (WebKitWebView *web_view, WPEView *view, WPEDisplayClass *display_class, WPEScreenSyncObserverClass *observer_class)
{
    webkit_web_view_load_uri (web_view, 1);                 /* int instead of const char * */
    wpe_toplevel_fullscreen (view);                          /* WPEView * instead of WPEToplevel * */
    webkit_web_view_new_bogus (web_view);                    /* does not exist */
    wpe_view_buffer_rendered (view);                         /* missing argument */
    display_class->get_frame_formats = NULL;                 /* vfunc not in WPEDisplayClass */
    wpe_buffer_import_to_egl_image (view, NULL);             /* WPEView * instead of WPEBuffer * */
    observer_class->tick = NULL;                             /* vfunc not in WPEScreenSyncObserverClass */
    wpe_screen_set_refresh_rate (view, 50000);               /* WPEView * instead of WPEScreen * */
}
