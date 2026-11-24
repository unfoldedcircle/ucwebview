/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * WebKit cache model of libucwebview (library 0.10.4). GLib only, so that the choice is unit-testable on a host
 * without WPE; uc_webview.c maps it to WebKitCacheModel and sets it on the default WebKitWebContext.
 *
 * Without a call to webkit_web_context_set_cache_model() WebKit uses WEB_BROWSER (CacheModel::PrimaryWebBrowser,
 * LegacyGlobalSettings.h:65; the DocumentBrowser default of LegacyGlobalSettings.cpp:42-44 is Cocoa only). What the
 * three models change in WPE WebKit 2.54.3:
 *
 * - Web process cache: only WEB_BROWSER gives it a capacity (WebProcessCache.cpp:321-353): the process of a closed
 *   page, or of a page that navigated to another site, is kept for 5 minutes (WebProcessCache.cpp:47-53) for a later
 *   load of the same site; ramSize() / 256 MB processes, at most 32, none below 2 GB of RAM. The WPE context turns
 *   the cache and process swap on navigation on (WebKitWebContext.cpp:441-446). With another model a process without
 *   pages, suspended pages or other users shuts down (WebProcessProxy.cpp:1805-1823).
 * - Back/forward cache: VIEWER has none (CacheModel.cpp:41-43, UI process WebProcessPool.cpp:1729-1741 and web
 *   process WebProcess.cpp:1026-1031); BROWSER and WEB_BROWSER keep 2 pages with 512 MB of RAM or more
 *   (CacheModel.cpp:62-69, 88-95).
 * - Memory cache of the web process: VIEWER disables it (WebProcessPoolGLib.cpp:221); BROWSER has 96 MB total with
 *   2 GB of RAM, 12..24 MB dead (CacheModel.cpp:71-85); WEB_BROWSER 128 MB, 32..64 MB dead and drops decoded data
 *   after 60 s (CacheModel.cpp:97-119).
 * - Disk cache of the network process: VIEWER none, BROWSER 20..75 MB, WEB_BROWSER 100 MB..1 GB, by free disk space
 *   (CacheModel.cpp:128-173, NetworkCache.cpp:81).
 *
 * The spare ("prewarmed") web process does not depend on the cache model: WebKit turns automatic prewarming on at the
 * first process swap of a navigation (WebProcessPool.cpp:2266-2272), starts one spare process when a main-frame load
 * finishes (WebPageProxy.cpp:9232-9233, WebProcessPool.cpp:1533-1546, 1116-1148) and ends it only under memory
 * pressure (WebProcessPool.cpp:1560-1572) or for an ephemeral session's store with the sandbox on
 * (WebProcessPool.cpp:1462-1495). The WebKit GLib API has no switch for it.
 *
 * The library uses BROWSER: the last view's web process ends with the view, the back/forward cache stays as it was.
 * UC_WEBVIEW_CACHE_MODEL=viewer|browser|web-browser, read once by uc_webview_global_init(), picks another model.
 */
#ifndef UC_CACHE_MODEL_H
#define UC_CACHE_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

#define UC_CACHE_MODEL_ENV "UC_WEBVIEW_CACHE_MODEL"

typedef enum {
    UC_CACHE_MODEL_VIEWER = 0,     /* WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER */
    UC_CACHE_MODEL_BROWSER,        /* WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER, the library's default */
    UC_CACHE_MODEL_WEB_BROWSER,    /* WEBKIT_CACHE_MODEL_WEB_BROWSER, WebKit's default */
} UcCacheModel;

#define UC_CACHE_MODEL_DEFAULT UC_CACHE_MODEL_BROWSER

typedef enum {
    UC_CACHE_MODEL_SOURCE_DEFAULT = 0,  /* the variable is unset, empty or blank */
    UC_CACHE_MODEL_SOURCE_ENV,          /* the variable names a model */
    UC_CACHE_MODEL_SOURCE_INVALID,      /* the variable has another value: the default applies */
} UcCacheModelSource;

/* The model for the value of UC_WEBVIEW_CACHE_MODEL: "viewer", "browser" or "web-browser", case-insensitive, blanks
 * around it ignored. NULL, "", blanks and other values give UC_CACHE_MODEL_DEFAULT. *source (may be NULL) tells
 * which case it was. */
UcCacheModel uc_cache_model_parse (const char *value, UcCacheModelSource *source);
/* The value UC_WEBVIEW_CACHE_MODEL takes for the model ("viewer", "browser", "web-browser"), for the log. */
const char  *uc_cache_model_name  (UcCacheModel model);

G_END_DECLS

#endif /* UC_CACHE_MODEL_H */
