/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * What the library and its web process extension (uc_extension.c) agree on besides the origin headers (uc_headers.h):
 * the library's script world and the messages to the page (library 0.9.0). Header only, no WebKit types.
 *
 * Why through the extension: every script of WebKit's UI process API (webkit_web_view_evaluate_javascript(),
 * _call_async_javascript_function(), _run_javascript()) runs with ForceUserGesture::Yes (WebKitWebView.cpp:4500, 4648,
 * 4921), and ScriptController::executeScriptInWorld() then sets a user gesture on the frame's document whatever the
 * script world (ScriptController.cpp:654-662). WebKit consumes the transient activation afterwards, but the page keeps
 * its sticky activation (navigator.userActivation.hasBeenActive), Page::userDidInteractWithPage (which arms the
 * "leave this page?" question of a beforeunload handler, FrameLoader.cpp:4064-4078) and Frame::hasHadUserInteraction:
 * a page that never saw a finger would act as if it had. The extension calls JavaScriptCore directly (the JSC GLib API
 * of WebKitFrame), which knows nothing of user gestures, and a user script runs without one
 * (LocalFrame::injectUserScriptImmediately(), ScriptController::evaluateInWorld()).
 *
 * The script world: the library's user content (the rendered probe's message handler, the device API's start script)
 * names it UC_EXT_HOST_WORLD, and the extension creates a WebKitScriptWorld of that name when its process starts,
 * before the first page. The web process then maps the user content's world to the extension's
 * (WebUserContentController::addContentWorldIfNecessary(): "The GLib API doesn't allow to create script worlds from
 * the UI process. We need to use the existing world created by the web extension if any. The world name is used as
 * the identifier.", WebUserContentController.cpp:139-158), so the user content and the extension share the world's
 * window objects. Page scripts never see that world (WebKit keeps every world's JavaScript objects apart).
 *
 * The messages go from the view to its page (webkit_web_view_send_message_to_page() ->
 * WebKitWebPage::user-message-received, WebPageGLib.cpp:99-123): over the connection to the web process of the
 * committed page, in order with WebKit's own messages to that page, and dispatched synchronously in the web process.
 * A page of a process swap is a new WebKitWebPage in another process with another page id (ProvisionalPageProxy.cpp:89,
 * WebPageProxy.cpp:1608-1614); messages from it to the view are dropped until it commits ("Unhandled message from
 * provisional process", ProvisionalPageProxy.cpp:713-877), and the view's messages reach it only after the commit,
 * when its document has started. What every page of a view has from its creation on, in whatever process, is the
 * view's user content (WebPageCreationParameters): that is why a document gets the device state from a user script and
 * not from a message.
 */
#ifndef UC_EXT_PROTOCOL_H
#define UC_EXT_PROTOCOL_H

/* the library's script world: uc_webview_when_rendered()'s probe (0.5.0), the magnification scripts (0.6.0), the
 * device API's start (0.9.0); since 0.9.0 all of them through the extension */
#define UC_EXT_HOST_WORLD           "ucwebview-host"

/* ---- device state for web pages (library 0.9.0) ----------------------------------------------------------------- */

/* The function the extension defines on the window object of UC_EXT_HOST_WORLD in every document of a main frame,
 * and which the library's user script (uc_device_start_script(): top frame, document start, UC_EXT_HOST_WORLD, only
 * while the view's device API is on) calls with the view's state, a plain object {screen, powerSource, powerProfile,
 * level}. The extension then runs device-host-shim.js in the page's own world with that state and keeps the update
 * function the shim returns on the same window object as UC_EXT_DEVICE_UPDATE (not writable, not configurable): one
 * per document, which also lives in the back/forward cache with its document. A second call in the same document
 * (document.open() starts the user script again) does nothing. */
#define UC_EXT_DEVICE_START         "ucwebviewDeviceStart"
#define UC_EXT_DEVICE_UPDATE        "ucwebviewDeviceUpdate"

/* A change for the shown document, from the view to its page; no reply. Parameters a{sv}: screen, powerSource,
 * powerProfile (s), level (i, percent), for an event also event (s, "suspend" or "resume") and sleptMs (d), as
 * uc_device_state_change() and uc_device_event_change() build it. The extension calls the document's
 * UC_EXT_DEVICE_UPDATE with that change; a document without one (the device API was off when it started) ignores it. */
#define UC_EXT_DEVICE_MESSAGE       "ucwebview.device.1"
#define UC_EXT_DEVICE_VARIANT_TYPE  "a{sv}"

/* ---- the library's scripts (library 0.9.0) --------------------------------------------------------------------- */

/* A script of the library for its world in the main frame of the shown page: uc_webview_when_rendered()'s probe and
 * the two magnification scripts. Parameters (s): the script. The extension evaluates it in UC_EXT_HOST_WORLD with
 * JavaScriptCore and replies (bd): whether it ran without an exception, and its result when that is a number (NaN
 * otherwise). Up to 0.8.x the library ran them with webkit_web_view_evaluate_javascript(), which made every probe a
 * user gesture for the page. */
#define UC_EXT_SCRIPT_MESSAGE       "ucwebview.host-script.1"
#define UC_EXT_SCRIPT_VARIANT_TYPE  "(s)"
#define UC_EXT_SCRIPT_REPLY_TYPE    "(bd)"

#endif /* UC_EXT_PROTOCOL_H */
