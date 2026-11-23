/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Device state for web pages (library 0.9.0), the UI process side: per view, the start script in the view's user
 * content manager and the changes sent to the shown page. uc_webview.c calls it, and so does tests/webkitgtk-host.c on
 * WebKitGTK (the same API in both ports). The web process side is uc_ext_device.c in the library's web process
 * extension; uc_ext_protocol.h has the protocol and why it goes through the extension (no script of WebKit's UI
 * process API, which would count as a user gesture for the page).
 *
 *  - The start script (uc_device_start_script(): the view's state) runs at the start of every document of the top
 *    frame (WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, no allow or block list)
 *    while the device API is on, in the library's script world (UC_EXT_HOST_WORLD), where the extension's
 *    UC_EXT_DEVICE_START puts device-host-shim.js into the page's world. A user script reaches every page of the view
 *    from its creation, also one that a process swap creates in another web process (uc_ext_protocol.h). WebKit adds
 *    a user script for the documents that start after the call (InjectUserScriptImmediately::No,
 *    WebKitUserContentManager.cpp:235-240): a page that is shown already gets the API at its next load.
 *  - A state change replaces the script (the new one is added before the old one goes, so that no document starts
 *    without; the extension starts the shim once per document) and goes to the shown page as UC_EXT_DEVICE_MESSAGE
 *    (uc_device_state_change()); so do the suspend and resume events (uc_device_event_change()), and the state after
 *    every finished load, which brings a page from the back/forward cache up to date (its document kept the state of
 *    its first load). Messages go only while the page is reachable (uc_page_wk.h): none before the view's first load
 *    or after its web process terminated, until it loads again.
 * Everything on the UI thread.
 */
#ifndef UC_DEVICE_WK_H
#define UC_DEVICE_WK_H

#include "uc_device.h"
#include "uc_page_wk.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _UcDeviceWk UcDeviceWk;

/* The device API of web_view, off, with uc_device_state_init()'s state; changes go through page. web_view and page
 * must outlive it. */
UcDeviceWk *uc_device_wk_new           (WebKitWebView *web_view, UcPageWk *page);
/* Removes the script from the view's user content manager. NULL is ignored. */
void        uc_device_wk_free          (UcDeviceWk *device);

gboolean    uc_device_wk_enabled       (UcDeviceWk *device);
/* On: the script with the current state, and the state to the shown page (a page that has the shim from an earlier
 * time catches up). Off: the script goes, nothing is sent any more; a page that is shown keeps its API without
 * updates until it is left. */
void        uc_device_wk_set_enabled   (UcDeviceWk *device, gboolean enabled);
/* Merges json (uc_device_state_merge_json()); a change replaces the script and goes to the page while the API is on.
 * FALSE when json is refused as a whole (*error). */
gboolean    uc_device_wk_set_state     (UcDeviceWk *device, const char *json, GString *ignored, const char **error);
const UcDeviceState *uc_device_wk_state (UcDeviceWk *device);
/* suspend, or resume with the time slept (< 0 counts as 0); to the shown page while the API is on */
void        uc_device_wk_event         (UcDeviceWk *device, UcDeviceEvent event, gint64 slept_ms);
/* The view's main frame finished a load (WEBKIT_LOAD_FINISHED): the state to the page while the API is on. */
void        uc_device_wk_load_finished (UcDeviceWk *device);

G_END_DECLS

#endif /* UC_DEVICE_WK_H */
