/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The requests of uc_webview_when_rendered(), see uc_rendered.h for the rules.
 */
#include "uc_rendered.h"

typedef enum {
    REQUEST_WAIT_UPDATE,    /* the script's rendering update has not been reported yet */
    REQUEST_WAIT_FRAME,     /* reported, no frame since the request: waiting for one (grace timer) */
    REQUEST_FRAME_ARRIVED,  /* the frame came, the idle ends the request */
} RequestState;

typedef struct {
    UcRendered *owner;
    guint id;
    UcRenderedFunc func;
    gpointer data;
    RequestState state;
    gint64 start_us;
    gint64 update_us;       /* -1 until the update is reported */
    guint64 frames_at_add;  /* owner->frames when the request was made */
    guint timeout_source;
    guint grace_source;
} Request;

struct UcRendered {
    guint timeout_ms, grace_ms;
    UcRenderedFinish finish;
    gpointer user_data;
    GList *requests;        /* Request *, oldest first */
    guint next_id;
    guint64 frames;         /* frames that reached the host */
    guint frame_idle;       /* ends the REQUEST_FRAME_ARRIVED requests */
    gboolean freeing;       /* inside uc_rendered_free(): no new requests */
};

UcRendered *
uc_rendered_new (guint timeout_ms, guint grace_ms, UcRenderedFinish finish, gpointer user_data)
{
    UcRendered *rendered = g_new0 (UcRendered, 1);
    rendered->timeout_ms = MAX (timeout_ms, 1u);
    rendered->grace_ms = grace_ms;
    rendered->finish = finish;
    rendered->user_data = user_data;
    rendered->next_id = 1;
    return rendered;
}

static void
request_clear_sources (Request *request)
{
    if (request->timeout_source != 0) {
        g_source_remove (request->timeout_source);
        request->timeout_source = 0;
    }
    if (request->grace_source != 0) {
        g_source_remove (request->grace_source);
        request->grace_source = 0;
    }
}

/* The request must already be out of the owner's list; it is freed after the finish function returned. */
static void
request_end (Request *request, UcRenderedStatus status, UcRenderedFrame frame, const char *reason)
{
    UcRendered *rendered = request->owner;
    UcRenderedResult result;

    request_clear_sources (request);
    result.id = request->id;
    result.status = status;
    result.elapsed_us = g_get_monotonic_time () - request->start_us;
    result.update_us = request->update_us;
    result.frame = frame;
    result.reason = status == UC_RENDERED_CANCELLED ? (reason != NULL ? reason : "cancelled") : NULL;
    if (rendered->finish != NULL)
        rendered->finish (request->func, request->data, &result, rendered->user_data);
    g_free (request);
}

static Request *
find_request (UcRendered *rendered, guint id)
{
    for (GList *l = rendered->requests; l != NULL; l = l->next) {
        Request *request = l->data;
        if (request->id == id)
            return request;
    }
    return NULL;
}

/* Takes the request out of the list and ends it. */
static void
request_finish (Request *request, UcRenderedStatus status, UcRenderedFrame frame, const char *reason)
{
    UcRendered *rendered = request->owner;
    rendered->requests = g_list_remove (rendered->requests, request);
    request_end (request, status, frame, reason);
}

static gboolean
on_timeout (gpointer user_data)
{
    Request *request = user_data;
    request->timeout_source = 0;
    request_finish (request, UC_RENDERED_TIMEOUT, UC_RENDERED_NO_FRAME, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
on_grace (gpointer user_data)
{
    Request *request = user_data;
    request->grace_source = 0;
    request_finish (request, UC_RENDERED_DONE, UC_RENDERED_NO_FRAME, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
on_frame_idle (gpointer user_data)
{
    UcRendered *rendered = user_data;
    rendered->frame_idle = 0;
    /* one at a time from the head: a finish function may add requests (or end others) meanwhile */
    for (;;) {
        Request *ready = NULL;
        for (GList *l = rendered->requests; l != NULL; l = l->next) {
            Request *request = l->data;
            if (request->state == REQUEST_FRAME_ARRIVED) {
                ready = request;
                break;
            }
        }
        if (ready == NULL)
            break;
        request_finish (ready, UC_RENDERED_DONE, UC_RENDERED_FRAME_AFTER_UPDATE, NULL);
    }
    return G_SOURCE_REMOVE;
}

guint
uc_rendered_add (UcRendered *rendered, UcRenderedFunc func, gpointer data)
{
    Request *request;

    g_return_val_if_fail (rendered != NULL, 0);
    if (rendered->freeing || g_list_length (rendered->requests) >= UC_RENDERED_MAX_PENDING)
        return 0;
    request = g_new0 (Request, 1);
    request->owner = rendered;
    /* unique among the pending ones: at most UC_RENDERED_MAX_PENDING of 2^32 - 1 ids are in use */
    do {
        request->id = rendered->next_id++;
        if (rendered->next_id == 0)
            rendered->next_id = 1;
    } while (find_request (rendered, request->id) != NULL);
    request->func = func;
    request->data = data;
    request->state = REQUEST_WAIT_UPDATE;
    request->start_us = g_get_monotonic_time ();
    request->update_us = -1;
    request->frames_at_add = rendered->frames;
    request->timeout_source = g_timeout_add (rendered->timeout_ms, on_timeout, request);
    rendered->requests = g_list_append (rendered->requests, request);
    return request->id;
}

gboolean
uc_rendered_update_done (UcRendered *rendered, guint id)
{
    Request *request;

    g_return_val_if_fail (rendered != NULL, FALSE);
    request = find_request (rendered, id);
    if (request == NULL || request->state != REQUEST_WAIT_UPDATE)
        return FALSE;
    request->update_us = g_get_monotonic_time () - request->start_us;
    if (rendered->frames > request->frames_at_add || rendered->grace_ms == 0) {
        request_finish (request, UC_RENDERED_DONE,
                        rendered->frames > request->frames_at_add ? UC_RENDERED_FRAME_BEFORE_UPDATE : UC_RENDERED_NO_FRAME,
                        NULL);
        return TRUE;
    }
    request->state = REQUEST_WAIT_FRAME;
    request->grace_source = g_timeout_add (rendered->grace_ms, on_grace, request);
    return TRUE;
}

void
uc_rendered_frame (UcRendered *rendered)
{
    gboolean any = FALSE;

    g_return_if_fail (rendered != NULL);
    rendered->frames++;
    for (GList *l = rendered->requests; l != NULL; l = l->next) {
        Request *request = l->data;
        if (request->state == REQUEST_WAIT_FRAME) {
            request->state = REQUEST_FRAME_ARRIVED;
            if (request->grace_source != 0) {
                g_source_remove (request->grace_source);
                request->grace_source = 0;
            }
            any = TRUE;
        }
    }
    /* not from inside the caller's frame path (WebKit's render_buffer): after it, like a deferred frame report */
    if (any && rendered->frame_idle == 0)
        rendered->frame_idle = g_idle_add_full (G_PRIORITY_HIGH, on_frame_idle, rendered, NULL);
}

gboolean
uc_rendered_cancel (UcRendered *rendered, guint id, const char *reason)
{
    Request *request;

    g_return_val_if_fail (rendered != NULL, FALSE);
    request = find_request (rendered, id);
    if (request == NULL)
        return FALSE;
    request_finish (request, UC_RENDERED_CANCELLED, UC_RENDERED_NO_FRAME, reason);
    return TRUE;
}

void
uc_rendered_cancel_all (UcRendered *rendered, const char *reason)
{
    GList *requests;

    g_return_if_fail (rendered != NULL);
    /* the ones pending now; a finish function's new requests go to the fresh list and stay */
    requests = rendered->requests;
    rendered->requests = NULL;
    /* no timer of a request still in this list may fire while an earlier one's finish function runs (it could
     * iterate the main context) */
    for (GList *l = requests; l != NULL; l = l->next)
        request_clear_sources (l->data);
    while (requests != NULL) {
        Request *request = requests->data;
        requests = g_list_delete_link (requests, requests);
        request_end (request, UC_RENDERED_CANCELLED, UC_RENDERED_NO_FRAME, reason);
    }
}

guint
uc_rendered_pending (UcRendered *rendered)
{
    g_return_val_if_fail (rendered != NULL, 0);
    return g_list_length (rendered->requests);
}

void
uc_rendered_free (UcRendered *rendered, const char *reason)
{
    if (rendered == NULL)
        return;
    /* a finish function cannot add requests from here on */
    rendered->freeing = TRUE;
    uc_rendered_cancel_all (rendered, reason);
    if (rendered->frame_idle != 0)
        g_source_remove (rendered->frame_idle);
    g_free (rendered);
}

/* No MessageChannel hop since 0.9.0: up to 0.8.x the probe ran with webkit_web_view_evaluate_javascript(), which forces
 * a user gesture, and requestAnimationFrame keeps the gesture of the script that registers it
 * (ScriptedAnimationController.cpp:120, 178-195), so the probe first left the gesture through a task of its own. The
 * extension runs it with JavaScriptCore, without a gesture. */
static const char probe[] =
    "(function (id) {"
    " requestAnimationFrame (function () { requestAnimationFrame (function () {"
    "  window.webkit.messageHandlers." UC_RENDERED_HANDLER ".postMessage (id); }); });"
    " return id;"
    "}) (%u)";

char *
uc_rendered_probe_script (guint id)
{
    return g_strdup_printf (probe, id);
}
