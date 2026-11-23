/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Device state for web pages (library 0.9.0), the web process side, in the library's web process extension
 * (uc_extension.c); uc_ext_protocol.h has the protocol, uc_device_wk.h the UI process side.
 *
 *  - Every document of a main frame gets UC_EXT_DEVICE_START on the window object of the library's script world
 *    (WebKitScriptWorld::window-object-cleared, which WebKit emits for every document of a frame for a world the
 *    extension made: WebKitWebPage.cpp:276-290 creates the world's window when the global objects become available,
 *    FrameLoader::receivedFirstData(), before the document is parsed).
 *  - The library's start script calls it at the document's start, while the view's device API is on. It evaluates
 *    device-host-shim.js (compiled in as bytes, uc_device_shim.h) in the page's own world with the JSC GLib API and
 *    calls it with the state, before any script of the page has run; the update function it returns is kept as
 *    UC_EXT_DEVICE_UPDATE on the window object of the library's world. The page has no reference to it.
 *  - UC_EXT_DEVICE_MESSAGE: the change goes to UC_EXT_DEVICE_UPDATE of the main frame's current document, as an
 *    object that JSON.parse() makes (uc_device_change_json(): only the known members with valid values, plain data
 *    properties, no setter of the page runs). The shim posts it to a MessageChannel of its own and returns; the page
 *    sees it in a task of its own.
 * JavaScriptCore runs no user gesture of its own: nothing here counts as one for the page.
 */
#ifndef UC_EXT_DEVICE_H
#define UC_EXT_DEVICE_H

#include <wpe/webkit-web-process-extension.h>

G_BEGIN_DECLS

/* host_world: the extension's WebKitScriptWorld named UC_EXT_HOST_WORLD, alive as long as the process */
void     uc_ext_device_init    (WebKitScriptWorld *host_world);

/* WebKitWebPage::user-message-received: TRUE when message is UC_EXT_DEVICE_MESSAGE (handled, no reply) */
gboolean uc_ext_device_message (WebKitWebPage *page, WebKitUserMessage *message);

/* page's main frame (transfer none) */
WebKitFrame *uc_ext_main_frame (WebKitWebPage *page);

G_END_DECLS

#endif /* UC_EXT_DEVICE_H */
