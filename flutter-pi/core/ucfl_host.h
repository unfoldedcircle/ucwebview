/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The host-neutral part of the plugin: everything between the uc.webview channels (PROTOCOL.md) and libucwebview,
 * except the pixels. It loads the library (ucfl_loader), initialises it, keeps the views by id (ucfl_views), turns
 * method calls into library calls and the library's callbacks into events, tracks the page's requests (script
 * dialogs, logins) and the calls whose reply waits for the library (whenRendered, setCookies, clearData).
 *
 * A host adapter (flutter-pi: flutterpi/uc_webview_plugin.c; a GTK runner later) brings the channels, the main loop
 * and the surfaces: it passes the raw bytes of each platform message to ucfl_host_handle_control()/_events() with a
 * token for the reply, and implements struct ucfl_host_ops. The frames of a view go to the adapter
 * (surface_frame); the adapter reports them rendered and releases them through the library itself.
 *
 * Rules taken over from remote-ui's Qt host (src/webview/webviewlibrary.cpp, webviewrequest.cpp, webviewitem.cpp):
 *  - required and optional functions and the features they make up (ucfl_loader.h);
 *  - views are created with uc_webview_new_sized() when the library has it, the pinch callback only with the
 *    pinchZoom feature and the request callbacks only with the requests feature (create may switch either off);
 *  - a script dialog of a type the protocol does not know is answered "no" at once, and a login of a scheme that takes
 *    no user name (Negotiate, HTML form, server trust, unknown) without credentials, without an event;
 *  - a request that arrives while no one listens on the event channel gets the library's default at once: "no" for
 *    alert, confirm and prompt, "leave" for the leave-page question, no credentials for a login; so do the pending
 *    requests when the listener cancels, and a request whose event could not be sent;
 *  - before loadUri, loadHtml, reload, goBack and goForward a pending leave-page question is answered "leave", before
 *    stop "stay" (the library would answer "stay" to every navigation call), with a requestCancelled event;
 *  - uc_webview_free() ends a view's requests without a callback, so dispose sends no event for them;
 *  - log lines carry the origin of a URL at most, never header values, cookie values, passwords, typed text or
 *    dialog texts.
 *
 * Thread: everything on the adapter's platform thread, the one that iterates GLib's default main context.
 */
#ifndef UCFL_HOST_H
#define UCFL_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uc_webview.h"
#include "ucfl_loader.h"
#include "ucfl_protocol.h"

struct ucfl_host;

struct ucfl_host_ops {
    /* Sends an event, a complete message, on UCFL_CHANNEL_EVENTS. Called only while a listener is attached. */
    void (*send_event)(void *ctx, const uint8_t *data, size_t size);
    /* Answers the call of token with a complete reply; data NULL and size 0 is the empty reply (Dart:
     * MissingPluginException). Called exactly once per call, from inside ucfl_host_handle_*() or later (a reply that
     * waits for the library), never after ucfl_host_free(). */
    void (*respond)(void *ctx, void *token, const uint8_t *data, size_t size);
    /* Optional: one log line without the newline. NULL: stderr with the prefix "[uc_webview] ". */
    void (*log)(void *ctx, const char *line);
    /* Optional: before uc_webview_global_init(), sets up the iteration of GLib's default main context (flutter-pi:
     * ucfp_glib_loop). 0, or -1 with a reason. */
    int (*prepare_init)(void *ctx, char *reason, size_t reason_size);
    /* A view was created: make its surface (flutter-pi: a texture). 0 with *texture_id and *surface, or -1 (the view
     * is freed again and create fails). refresh_rate is create's (0 when not given). */
    int (*surface_new)(void *ctx, int64_t view_id, uc_webview *wv, double refresh_rate, int64_t *texture_id,
                       void **surface);
    /* A frame of the view (frame_ready). The adapter owns it until it calls uc_webview_frame_release() for it, and
     * reports it rendered (uc_webview_frame_rendered()) when it is shown. NULL: released at once (tests). */
    void (*surface_frame)(void *ctx, void *surface, const uc_webview_frame *frame);
    /* setFrameHold: while held, the surface keeps showing its current frame; new frames are reported rendered at once
     * and the newest waits until the hold ends (remote-ui FrameQueue::setHeld()). */
    void (*surface_hold)(void *ctx, void *surface, bool hold);
    /* The view goes: called right before uc_webview_free(). The adapter drops the surface, releases the frames it
     * holds and calls nothing of the library for this view afterwards. */
    void (*surface_free)(void *ctx, void *surface);
    /* Optional: after the host called into the library from outside a library callback (flutter-pi re-arms GLib). */
    void (*after_call)(void *ctx);
    /* Optional: the adapter's counters for the stats reply, at most max; returns the count. */
    size_t (*stats)(void *ctx, struct ucfl_stat *out, size_t max);
};

struct ucfl_host_config {
    const char *library_path; /* the library to dlopen(); NULL: UC_WEBVIEW_SONAME */
    void *egl_display;        /* the EGLDisplay frames are imported on (uc_webview_global_init()); NULL: shared memory */
    const char *data_dir;     /* website data directory; NULL: an ephemeral session */
    const char *unavailable;  /* not NULL: this host cannot show web views (the reason hello reports) */
};

/* The strings of config are copied. NULL without memory. */
struct ucfl_host *ucfl_host_new(const struct ucfl_host_ops *ops, void *ctx, const struct ucfl_host_config *config);

/* Disposes every view (surface_free, uc_webview_free()), drops the replies still waiting (their tokens are never
 * answered: the engine is going) and frees the host. Library callbacks of requests made before (setCookies,
 * clearData) that come afterwards are ignored. */
void ucfl_host_free(struct ucfl_host *host);

/* A message of UCFL_CHANNEL_CONTROL / UCFL_CHANNEL_EVENTS; token goes back to ops->respond(). */
void ucfl_host_handle_control(struct ucfl_host *host, const uint8_t *msg, size_t size, void *token);
void ucfl_host_handle_events(struct ucfl_host *host, const uint8_t *msg, size_t size, void *token);

/* The loaded library, NULL before it was loaded or when it is unavailable. */
const struct ucfl_lib *ucfl_host_lib(const struct ucfl_host *host);

/* Sends an event the adapter built (the frame event); counted as dropped while nobody listens. Frees the writer.
 * false when it was not sent (nobody listens, or the writer ran out of memory). */
bool ucfl_host_send_event(struct ucfl_host *host, struct ucfl_writer *w);

/* printf-like log line through ops->log */
void ucfl_host_log(struct ucfl_host *host, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* UCFL_HOST_H */
