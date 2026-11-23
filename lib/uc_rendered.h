/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The requests of uc_webview_when_rendered() (library 0.5.0) of one view: ids, the timeout, and when a request is
 * done, and the probe script. GLib only, so that the rules are unit-tested on a host without WPE
 * (tests/test-rendered.c); uc_webview.c runs the script in the page and reports its rendering update and the frames
 * here.
 *
 * A request is done once both happened:
 *  1. The rendering update it waits for has run (uc_rendered_update_done(), the script's message: two
 *     requestAnimationFrame callbacks after the request, see uc_webview.c).
 *  2. A frame reached the host after the request. If one did before the update was reported, the request is done
 *     at the report. Otherwise it waits for the next frame (uc_rendered_frame()), at most grace_ms: WebKit hands a
 *     frame to the host only when its rendering fence has signalled (AcceleratedBackingStore.cpp:194-210), which can
 *     be after the script's message, and an update that changed nothing visible produces no frame at all.
 * Every request ends exactly once: DONE, TIMEOUT (timeout_ms after the request) or CANCELLED (uc_rendered_cancel(),
 * uc_rendered_cancel_all(), uc_rendered_free()). The finish function runs from the main context (a timeout, an idle
 * after a frame) or inside the uc_rendered_* call that ends the request; it may add new requests.
 */
#ifndef UC_RENDERED_H
#define UC_RENDERED_H

#include <glib.h>

G_BEGIN_DECLS

/* the values of UC_WEBVIEW_RENDERED_* (uc_webview.h, asserted in uc_webview.c) */
typedef enum {
    UC_RENDERED_DONE = 0,
    UC_RENDERED_TIMEOUT = 1,
    UC_RENDERED_CANCELLED = 2,
} UcRenderedStatus;

/* how a DONE request met condition 2 */
typedef enum {
    UC_RENDERED_FRAME_BEFORE_UPDATE,    /* a frame had reached the host when the update was reported */
    UC_RENDERED_FRAME_AFTER_UPDATE,     /* done at the first frame after the update */
    UC_RENDERED_NO_FRAME,               /* no frame within grace_ms of the update */
} UcRenderedFrame;

typedef struct {
    guint id;
    UcRenderedStatus status;
    gint64 elapsed_us;      /* from the request to its end */
    gint64 update_us;       /* from the request to the update report, -1 without one */
    UcRenderedFrame frame;  /* DONE only */
    const char *reason;     /* CANCELLED only: why, a static string ("navigation", ...) */
} UcRenderedResult;

typedef struct UcRendered UcRendered;

/* A request's callback as a generic function pointer: the caller casts its own callback type to it and back. */
typedef void (*UcRenderedFunc) (void);

/* func/data are the request's, as given to uc_rendered_add(); user_data is the queue's (uc_rendered_new()). */
typedef void (*UcRenderedFinish) (UcRenderedFunc func, gpointer data, const UcRenderedResult *result, gpointer user_data);

/* The most requests a view keeps; uc_rendered_add() refuses more. */
#define UC_RENDERED_MAX_PENDING 32

/* The script message handler the probe reports through, registered for the library's script world only
 * (UC_EXT_HOST_WORLD), and the probe of request id: run in that world of the main frame (since 0.9.0 by the web process
 * extension, UC_EXT_SCRIPT_MESSAGE), it returns id at once and posts id to the handler at the second
 * requestAnimationFrame callback after it, see uc_webview.c. Free with g_free(). */
#define UC_RENDERED_HANDLER     "ucwebviewRendered"
char       *uc_rendered_probe_script (guint id);

/* timeout_ms > 0; grace_ms may be 0 (done at the update report when no frame came since the request). */
UcRendered *uc_rendered_new        (guint timeout_ms, guint grace_ms, UcRenderedFinish finish, gpointer user_data);
/* Ends every pending request with CANCELLED and reason (inside this call), then frees the queue. NULL is ignored.
 * Not from a finish function. */
void        uc_rendered_free       (UcRendered *rendered, const char *reason);

/* A new request with the caller's callback and data. Returns its id (> 0, unique among the pending ones), or 0 when
 * UC_RENDERED_MAX_PENDING requests are pending or uc_rendered_free() runs (nothing is called for a refused
 * request). */
guint       uc_rendered_add        (UcRendered *rendered, UcRenderedFunc func, gpointer data);
/* The rendering update of request id has run. FALSE for an id that is not pending (ended already, never issued). */
gboolean    uc_rendered_update_done (UcRendered *rendered, guint id);
/* A frame reached the host. Requests waiting for it end from an idle of the default main context, after the
 * caller's frame callback. */
void        uc_rendered_frame      (UcRendered *rendered);
/* Ends request id with CANCELLED; FALSE when it is not pending. */
gboolean    uc_rendered_cancel     (UcRendered *rendered, guint id, const char *reason);
/* Ends every pending request with CANCELLED. Requests the finish function adds meanwhile stay pending. */
void        uc_rendered_cancel_all (UcRendered *rendered, const char *reason);
guint       uc_rendered_pending    (UcRendered *rendered);

G_END_DECLS

#endif /* UC_RENDERED_H */
