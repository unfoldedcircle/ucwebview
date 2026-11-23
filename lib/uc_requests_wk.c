/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Requests of the page to the host (library 0.7.0), the WebKit side, see uc_requests_wk.h. WebKit facts it relies on
 * (WebKit 2.54.1, Source/WebKit/UIProcess/API/glib unless named otherwise):
 *  - A script dialog is a WebKitScriptDialog that WebKit unrefs right after emitting script-dialog; a handler that
 *    answers later keeps a reference and returns TRUE. The last unref closes it with the values set so far
 *    (WebKitWebView.cpp:2926-2972, WebKitScriptDialog.cpp:192-213). One dialog at a time per view
 *    (priv->currentScriptDialog, asserted empty, cleared by the completion handler).
 *  - Closing the dialog inside its own script-dialog emission clears priv->currentScriptDialog before WebKit's unref
 *    after the emission, which then unrefs NULL: a host that answers from inside script_dialog() gets its dialog
 *    closed from an idle instead.
 *  - Nothing closes a pending dialog when the web process terminates (WebKitWebView.cpp:5405-5421); dispose does
 *    (:1306-1309).
 *  - WebKit keeps the last authentication request only and finishes it before it emits the next load-changed STARTED
 *    or FINISHED or load-failed: cancel() for a 401/407/5xx main resource, else "authenticated" without completing
 *    the challenge (WebKitWebView.cpp:2686-2705, 2734-2780, 3190-3201). cancel() emits "cancelled"
 *    (WebKitAuthenticationRequest.cpp:489-501); authenticate(NULL) continues without credentials and the server's
 *    401 page loads (NetworkDataTaskSoup.cpp:685-720).
 *  - webkit_web_view_get_uri() is the active URI, the pending one during a load (PageLoadState.cpp:190-211): the
 *    origin comes from the URI at the last main-frame commit instead.
 *  - <a download> becomes a download at the navigation stage and reaches the host only as the session's
 *    download-started (WebPageProxy.cpp:5836-5844, WebKitNavigationClient.cpp:156-163).
 */
#include "uc_requests_wk.h"

#include "uc_trace.h"
#include "uc_uri_policy.h"

#include <string.h>

G_STATIC_ASSERT (UC_WEBVIEW_DIALOG_ALERT == WEBKIT_SCRIPT_DIALOG_ALERT);
G_STATIC_ASSERT (UC_WEBVIEW_DIALOG_CONFIRM == WEBKIT_SCRIPT_DIALOG_CONFIRM);
G_STATIC_ASSERT (UC_WEBVIEW_DIALOG_PROMPT == WEBKIT_SCRIPT_DIALOG_PROMPT);
G_STATIC_ASSERT (UC_WEBVIEW_DIALOG_BEFORE_UNLOAD == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_DEFAULT == WEBKIT_AUTHENTICATION_SCHEME_DEFAULT);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC == WEBKIT_AUTHENTICATION_SCHEME_HTTP_BASIC);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST == WEBKIT_AUTHENTICATION_SCHEME_HTTP_DIGEST);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_HTML_FORM == WEBKIT_AUTHENTICATION_SCHEME_HTML_FORM);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_NTLM == WEBKIT_AUTHENTICATION_SCHEME_NTLM);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_NEGOTIATE == WEBKIT_AUTHENTICATION_SCHEME_NEGOTIATE);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_SERVER_TRUST == WEBKIT_AUTHENTICATION_SCHEME_SERVER_TRUST_EVALUATION_REQUESTED);
G_STATIC_ASSERT (UC_WEBVIEW_AUTH_SCHEME_UNKNOWN == WEBKIT_AUTHENTICATION_SCHEME_UNKNOWN);

typedef enum {
    REPORT_NONE,    /* uc_webview_free(): no callback */
    REPORT_NOW,     /* from a WebKit signal: request_cancelled() at once, before the owner's own callback */
    REPORT_LATER,   /* inside a host call: request_cancelled() from the main context */
} Report;

struct _UcWkRequests {
    WebKitWebView *web_view;        /* borrowed: the owner frees the requests before the view */
    UcWkRequestsHost host;
    UcRequestTable *table;
    UcDialogGuard guard;
    char *origin;                   /* of the last committed main-frame URI, NULL without a host */
    int in_dialog_signal;           /* inside our script-dialog handler: no dialog may close now */
    GArray *cancelled_ids;          /* guint64: ended inside a host call, reported from an idle */
    GPtrArray *dialogs_to_close;    /* WebKitScriptDialog*, answered inside script_dialog(): closed from an idle */
    guint idle_source;
    gint64 navigating_until_us;     /* the host navigates away: dialogs get WebKit's default until then, 0 = no */
};

/* A host callback with the owner's depth counter (uc_webview_free() is refused inside one). */
#define HOST_CALLBACK(r, name, ...) \
    do { \
        if ((r)->host.cb->name != NULL) { \
            (*(r)->host.callback_depth)++; \
            (r)->host.cb->name ((r)->host.wv, __VA_ARGS__, (r)->host.user); \
            (*(r)->host.callback_depth)--; \
        } \
    } while (0)

static void schedule_idle (UcWkRequests *r);

/* ---- ending requests ----------------------------------------------------------------------------------------- */

static void
close_dialog (WebKitScriptDialog *dialog)
{
    webkit_script_dialog_close (dialog);
    webkit_script_dialog_unref (dialog);
}

/* The dialog's answer, then close it and drop our reference (from an idle while its own signal runs). */
static void
dialog_finish (UcWkRequests *r, UcRequest *request, gboolean accept, const char *text)
{
    WebKitScriptDialog *dialog = request->object;
    UcDialogAnswer answer = uc_dialog_answer (request->type, accept, text);

    if (answer.set_confirmed)
        webkit_script_dialog_confirm_set_confirmed (dialog, answer.confirmed);
    if (answer.set_text)
        webkit_script_dialog_prompt_set_text (dialog, answer.text);
    uc_dialog_guard_ended (&r->guard, request->type, g_get_monotonic_time ());
    /* "stay": the navigation that asked stops, the page goes on and may show dialogs again */
    if (request->type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD && !accept)
        r->navigating_until_us = 0;
    if (r->in_dialog_signal > 0) {
        g_ptr_array_add (r->dialogs_to_close, dialog);
        schedule_idle (r);
        return;
    }
    close_dialog (dialog);
}

/* An authentication request without credentials: continue (the 401 page) or cancel the load */
static void
auth_apply (WebKitAuthenticationRequest *auth, UcAuthAnswer answer)
{
    if (answer == UC_AUTH_CANCEL_LOAD)
        webkit_authentication_request_cancel (auth);
    else
        webkit_authentication_request_authenticate (auth, NULL);
}

static void
auth_finish (UcRequest *request, const char *user, const char *password, UcAuthAnswer without)
{
    WebKitAuthenticationRequest *auth = request->object;

    /* our "cancelled" handler goes first: the end is this one, not a second one through the signal */
    if (request->handler != 0)
        g_signal_handler_disconnect (auth, request->handler);
    request->handler = 0;
    if (user != NULL) {
        /* for the session only: WebKit keeps it in the network session's memory (NetworkDataTaskSoup.cpp:638-683);
         * can_save_credentials() is FALSE on this build anyway (no libsecret, WebKitAuthenticationRequest.cpp:211-251) */
        WebKitCredential *credential = webkit_credential_new (user, password != NULL ? password : "",
                                                              WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
        webkit_authentication_request_authenticate (auth, credential);
        webkit_credential_free (credential);
    } else {
        auth_apply (auth, without);
    }
    g_object_unref (auth);
}

static void
report_cancelled (UcWkRequests *r, guint64 id)
{
    HOST_CALLBACK (r, request_cancelled, id);
}

/* The library ends a pending request: its "no" for a dialog, a cancel for an authentication request. */
static void
end_request (UcWkRequests *r, UcRequest *request, const char *reason, Report report)
{
    guint64 id = request->id;

    if (request->kind == UC_REQUEST_DIALOG)
        dialog_finish (r, request, FALSE, NULL);
    else
        auth_finish (request, NULL, NULL, UC_AUTH_CANCEL_LOAD);
    uc_trace_request_end (r->host.wv, id, reason, report != REPORT_NONE);
    if (report == REPORT_NOW) {
        report_cancelled (r, id);
    } else if (report == REPORT_LATER) {
        g_array_append_val (r->cancelled_ids, id);
        schedule_idle (r);
    }
}

static void
end_requests (UcWkRequests *r, UcRequestKind kind, const char *reason, Report report)
{
    /* out of the table first: a host callback below may answer or end requests itself */
    GPtrArray *ended = uc_request_table_steal_kind (r->table, kind);

    for (guint i = 0; i < ended->len; i++)
        end_request (r, g_ptr_array_index (ended, i), reason, report);
    g_ptr_array_unref (ended);
}

/* Work left from inside a host call or a script-dialog emission: dialogs to close, request_cancelled() reports.
 * Runs from the idle and at the start of every WebKit signal of the view, so that the reports keep their order
 * with the owner's callbacks. */
static void
flush (UcWkRequests *r)
{
    if (r->in_dialog_signal == 0)
        while (r->dialogs_to_close->len > 0)
            close_dialog (g_ptr_array_steal_index (r->dialogs_to_close, 0));
    while (r->cancelled_ids->len > 0) {
        guint64 id = g_array_index (r->cancelled_ids, guint64, 0);
        g_array_remove_index (r->cancelled_ids, 0);
        report_cancelled (r, id);
    }
}

static gboolean
on_idle (gpointer user_data)
{
    UcWkRequests *r = user_data;

    r->idle_source = 0;
    flush (r);
    if (r->dialogs_to_close->len > 0 || r->cancelled_ids->len > 0)
        schedule_idle (r);
    return G_SOURCE_REMOVE;
}

static void
schedule_idle (UcWkRequests *r)
{
    /* before the IPC of the next load: request_cancelled() reaches the host before its load_changed() */
    if (r->idle_source == 0)
        r->idle_source = g_idle_add_full (G_PRIORITY_HIGH, on_idle, r, NULL);
}

/* ---- script dialogs ---------------------------------------------------------------------------------------- */

static gboolean
on_script_dialog (WebKitWebView *web_view, WebKitScriptDialog *dialog, gpointer user_data)
{
    UcWkRequests *r = user_data;
    int type = (int) webkit_script_dialog_get_dialog_type (dialog);
    const char *message, *default_text;
    UcRequest *request;
    guint64 id;

    (void) web_view;
    flush (r);
    if (r->host.cb->script_dialog == NULL) {
        if (type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD) {
            gboolean leave = uc_request_decisions.before_unload_without_callback == UC_BEFORE_UNLOAD_LEAVE;
            webkit_script_dialog_confirm_set_confirmed (dialog, leave);
            uc_trace_request_default (r->host.wv, leave ? "beforeunload left (no script_dialog callback)"
                                                        : "beforeunload stayed (no script_dialog callback)");
            return TRUE;    /* WebKit's unref after the signal closes it with this answer */
        }
        if (uc_trace_enabled ()) {
            char *what = g_strdup_printf ("%s closed by WebKit's default (no script_dialog callback)",
                                          uc_dialog_type_name (type));
            uc_trace_request_default (r->host.wv, what);
            g_free (what);
        }
        return FALSE;       /* WebKit's default: closed at once, confirm() false, prompt() null */
    }
    if (type != UC_WEBVIEW_DIALOG_BEFORE_UNLOAD && r->navigating_until_us != 0) {
        gint64 now = g_get_monotonic_time ();
        if (now < r->navigating_until_us) {
            /* the page's script went on after the dialog the host navigation closed */
            uc_dialog_guard_ended (&r->guard, type, now);
            if (uc_trace_enabled ()) {
                char *what = g_strdup_printf ("%s closed by WebKit's default (the host navigates away)",
                                              uc_dialog_type_name (type));
                uc_trace_request_default (r->host.wv, what);
                g_free (what);
            }
            return FALSE;
        }
        r->navigating_until_us = 0;
    }
    switch (uc_dialog_guard_check (&r->guard, type, g_get_monotonic_time ())) {
    case UC_GUARD_TRIPPED:
        g_message ("ucwebview: dialog loop guard: the page asked again less than %" G_GINT64_FORMAT " ms after the "
                   "answer %u times in a row; its further dialogs get WebKit's default answer until the user taps or "
                   "presses a key in the page, or the next page commits",
                   uc_request_decisions.guard_window_us / 1000, uc_request_decisions.guard_quick_dialogs);
        /* fall through */
    case UC_GUARD_SUPPRESS:
        uc_trace_request_guard (r->host.wv, type);
        return FALSE;
    case UC_GUARD_SHOW:
    default:
        break;
    }
    request = uc_request_table_add (r->table, UC_REQUEST_DIALOG, type, webkit_script_dialog_ref (dialog));
    id = request->id;   /* request itself may end inside the callback */
    message = type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD && !uc_request_decisions.before_unload_message
              ? NULL : webkit_script_dialog_get_message (dialog);
    default_text = type == UC_WEBVIEW_DIALOG_PROMPT ? webkit_script_dialog_prompt_get_default_text (dialog) : NULL;
    uc_trace_request (r->host.wv, id, UC_REQUEST_DIALOG, type);
    r->in_dialog_signal++;
    HOST_CALLBACK (r, script_dialog, id, type, r->origin, message, default_text);
    r->in_dialog_signal--;
    return TRUE;
}

/* ---- authentication ---------------------------------------------------------------------------------------- */

/* "scheme://host[:port]" of the server (or proxy) that asks */
static char *
auth_origin (WebKitAuthenticationRequest *auth)
{
    WebKitSecurityOrigin *origin = webkit_authentication_request_get_security_origin (auth);
    char *text;

    if (origin == NULL)
        return NULL;
    /* not webkit_security_origin_to_string(): its port comes from the protection space, also the default one */
    text = uc_request_origin_format (webkit_security_origin_get_protocol (origin),
                                     webkit_security_origin_get_host (origin),
                                     (int) webkit_security_origin_get_port (origin));
    webkit_security_origin_unref (origin);
    return text;
}

/* WebKit gave the request up: at the next load (WebKitWebView.cpp:2686-2705), or another way */
static void
on_auth_cancelled (WebKitAuthenticationRequest *auth, gpointer user_data)
{
    UcWkRequests *r = user_data;
    UcRequest *request = uc_request_table_find_object (r->table, auth);
    guint64 id;

    if (request == NULL)
        return;
    request = uc_request_table_steal (r->table, request->id);
    id = request->id;
    g_signal_handler_disconnect (auth, request->handler);
    g_free (request);
    uc_trace_request_end (r->host.wv, id, "webkit", TRUE);
    report_cancelled (r, id);
    g_object_unref (auth);
}

static gboolean
on_authenticate (WebKitWebView *web_view, WebKitAuthenticationRequest *auth, gpointer user_data)
{
    UcWkRequests *r = user_data;
    WebKitAuthenticationScheme scheme = webkit_authentication_request_get_scheme (auth);
    UcRequest *request;
    unsigned flags = 0;
    char *origin;
    guint64 id;

    (void) web_view;
    flush (r);
    if (scheme == WEBKIT_AUTHENTICATION_SCHEME_CLIENT_CERTIFICATE_REQUESTED
        || scheme == WEBKIT_AUTHENTICATION_SCHEME_CLIENT_CERTIFICATE_PIN_REQUESTED) {
        /* no certificate store on the remote: never shown, like WebKitGTK's default (WebKitWebViewGtk.cpp:43-65) */
        origin = auth_origin (auth);
        g_message ("ucwebview: %s asks for a client certificate%s: %s", origin != NULL ? origin : "(unknown)",
                   scheme == WEBKIT_AUTHENTICATION_SCHEME_CLIENT_CERTIFICATE_PIN_REQUESTED ? " PIN" : "",
                   uc_request_decisions.client_certificate == UC_AUTH_CANCEL_LOAD ? "load cancelled"
                                                                                  : "continuing without one");
        g_free (origin);
        auth_apply (auth, uc_request_decisions.client_certificate);
        uc_trace_request_default (r->host.wv, uc_request_decisions.client_certificate == UC_AUTH_CANCEL_LOAD
                                              ? "client certificate cancelled"
                                              : "client certificate continued without credentials");
        return TRUE;
    }
    if (r->host.cb->authenticate == NULL) {
        origin = auth_origin (auth);
        g_message ("ucwebview: %s asks for a login, the host takes none: %s", origin != NULL ? origin : "(unknown)",
                   uc_request_decisions.auth_without_callback == UC_AUTH_CANCEL_LOAD
                   ? "load cancelled" : "continuing without credentials");
        g_free (origin);
        auth_apply (auth, uc_request_decisions.auth_without_callback);
        uc_trace_request_default (r->host.wv, uc_request_decisions.auth_without_callback == UC_AUTH_CANCEL_LOAD
                                              ? "auth cancelled (no authenticate callback)"
                                              : "auth continued without credentials (no authenticate callback)");
        return TRUE;
    }
    /* WebKit keeps only the newest request (WebKitWebView.cpp:3190-3201); the table keeps them all */
    request = uc_request_table_add (r->table, UC_REQUEST_AUTH, (int) scheme, g_object_ref (auth));
    id = request->id;
    request->handler = g_signal_connect (auth, "cancelled", G_CALLBACK (on_auth_cancelled), r);
    if (webkit_authentication_request_is_retry (auth))
        flags |= UC_WEBVIEW_AUTH_RETRY;
    if (webkit_authentication_request_is_for_proxy (auth))
        flags |= UC_WEBVIEW_AUTH_PROXY;
    origin = auth_origin (auth);
    uc_trace_request (r->host.wv, id, UC_REQUEST_AUTH, (int) scheme);
    HOST_CALLBACK (r, authenticate, id, origin, webkit_authentication_request_get_realm (auth), (int) scheme, flags);
    g_free (origin);
    return TRUE;
}

/* ---- new windows and window.close() ------------------------------------------------------------------------- */

static WebKitWebView *
on_create (WebKitWebView *web_view, WebKitNavigationAction *action, gpointer user_data)
{
    UcWkRequests *r = user_data;
    WebKitURIRequest *request = webkit_navigation_action_get_request (action);
    const char *uri = request != NULL ? webkit_uri_request_get_uri (request) : NULL;
    UcNewWindowVerdict verdict = uc_request_new_window_verdict (uri, request != NULL
                                                                ? webkit_uri_request_get_http_method (request) : NULL);
    char *loggable = uc_uri_policy_loggable (uri, r->host.log_full_urls);

    flush (r);
    if (verdict == UC_NEW_WINDOW_LOAD) {
        g_message ("ucwebview: new window %s loads into this view", loggable);
        /* Cog's way (launcher/cog-launcher.c:223-228): the navigation's request with its headers, into this view.
         * NULL as the new view: a non-NULL one must be a new related view (WebKitWebView.cpp:2004-2027). */
        webkit_web_view_load_request (web_view, request);
        HOST_CALLBACK (r, new_window, uri, UC_WEBVIEW_NEW_WINDOW_LOADED);
    } else {
        g_message ("ucwebview: new window %s refused: %s", loggable,
                   verdict == UC_NEW_WINDOW_REFUSED_METHOD ? "a form posted to a new window"
                                                           : "only http and https open in this view");
        HOST_CALLBACK (r, new_window, uri != NULL ? uri : "", UC_WEBVIEW_NEW_WINDOW_REFUSED);
    }
    g_free (loggable);
    return NULL;
}

static void
on_close (WebKitWebView *web_view, gpointer user_data)
{
    UcWkRequests *r = user_data;

    (void) web_view;
    flush (r);
    g_message ("ucwebview: the page asked to close the view (window.close())");
    if (r->host.cb->close_requested != NULL) {
        (*r->host.callback_depth)++;
        r->host.cb->close_requested (r->host.wv, r->host.user);
        (*r->host.callback_depth)--;
    }
}

/* ---- responses ------------------------------------------------------------------------------------------------ */

/* Every navigation response, main frame and frames: downloads, PDFs while the viewer is off (since 0.6.1) and types
 * WebKit cannot show are ignored, the main frame's reported. Everything else is WebKit's default handler's (FALSE). */
static gboolean
on_decide_policy (WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type,
                  gpointer user_data)
{
    UcWkRequests *r = user_data;
    WebKitResponsePolicyDecision *response_decision;
    WebKitURIResponse *response;
    SoupMessageHeaders *headers;
    const char *mime_type, *uri, *disposition;
    UcResponseVerdict verdict;
    gboolean main_frame;
    char *loggable;

    (void) web_view;
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
        return FALSE;
    response_decision = WEBKIT_RESPONSE_POLICY_DECISION (decision);
    response = webkit_response_policy_decision_get_response (response_decision);
    mime_type = webkit_uri_response_get_mime_type (response);
    headers = webkit_uri_response_get_http_headers (response);
    disposition = headers != NULL ? soup_message_headers_get_one (headers, "Content-Disposition") : NULL;
    verdict = uc_request_response_verdict (mime_type, disposition,
                                           webkit_response_policy_decision_is_mime_type_supported (response_decision),
                                           webkit_uri_response_get_status_code (response), r->host.pdf_viewer);
    if (verdict == UC_RESPONSE_SHOW)
        return FALSE;
    main_frame = webkit_response_policy_decision_is_main_frame_main_resource (response_decision);
    uri = webkit_uri_response_get_uri (response);
    loggable = uc_uri_policy_loggable (uri, r->host.log_full_urls);
    switch (verdict) {
    case UC_RESPONSE_DOWNLOAD:
        g_message ("ucwebview: download%s %s (%s) refused", main_frame ? "" : " in a frame", loggable,
                   mime_type != NULL ? mime_type : "unknown type");
        break;
    case UC_RESPONSE_PDF:
        g_message ("ucwebview: %s %s (%s) not shown, the PDF viewer is off", main_frame ? "PDF" : "PDF in a frame",
                   loggable, mime_type);
        break;
    case UC_RESPONSE_UNSUPPORTED:
    default:
        g_message ("ucwebview: %s%s (%s) not shown, WebKit cannot show this type", main_frame ? "" : "frame ",
                   loggable, mime_type != NULL ? mime_type : "unknown type");
        break;
    }
    g_free (loggable);
    /* the main frame's load fails with FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE, which the owner does not report
     * (uc_wk_requests_load_failed()); a frame keeps its empty document */
    webkit_policy_decision_ignore (decision);
    if (main_frame && (verdict != UC_RESPONSE_PDF || uc_request_decisions.pdf_as_unsupported)) {
        flush (r);
        HOST_CALLBACK (r, response_refused, uri, mime_type,
                       verdict == UC_RESPONSE_DOWNLOAD ? UC_WEBVIEW_RESPONSE_DOWNLOAD : UC_WEBVIEW_RESPONSE_UNSUPPORTED);
    }
    return TRUE;
}

void
uc_wk_requests_download_started (UcWkRequests *r, WebKitDownload *download, gboolean log_full_urls)
{
    WebKitURIRequest *request = webkit_download_get_request (download);
    WebKitURIResponse *response = webkit_download_get_response (download);
    const char *uri = request != NULL ? webkit_uri_request_get_uri (request) : NULL;
    const char *mime_type = response != NULL ? webkit_uri_response_get_mime_type (response) : NULL;
    char *loggable = uc_uri_policy_loggable (uri, log_full_urls);

    /* <a download> (or anything else WebKit turned into a download): nothing is saved on the remote */
    webkit_download_cancel (download);
    g_message ("ucwebview: download %s (%s) refused", loggable, mime_type != NULL ? mime_type : "unknown type");
    g_free (loggable);
    if (r == NULL)
        return;
    flush (r);
    HOST_CALLBACK (r, response_refused, uri != NULL ? uri : "", mime_type, UC_WEBVIEW_RESPONSE_DOWNLOAD);
}

/* ---- the owner's hooks ---------------------------------------------------------------------------------------- */

UcWkRequests *
uc_wk_requests_new (WebKitWebView *web_view, const UcWkRequestsHost *host)
{
    UcWkRequests *r = g_new0 (UcWkRequests, 1);

    r->web_view = web_view;
    r->host = *host;
    r->table = uc_request_table_new ();
    uc_dialog_guard_reset (&r->guard);
    r->cancelled_ids = g_array_new (FALSE, FALSE, sizeof (guint64));
    r->dialogs_to_close = g_ptr_array_new ();
    g_signal_connect (web_view, "script-dialog", G_CALLBACK (on_script_dialog), r);
    g_signal_connect (web_view, "authenticate", G_CALLBACK (on_authenticate), r);
    g_signal_connect (web_view, "create", G_CALLBACK (on_create), r);
    g_signal_connect (web_view, "close", G_CALLBACK (on_close), r);
    g_signal_connect (web_view, "decide-policy", G_CALLBACK (on_decide_policy), r);
    return r;
}

void
uc_wk_requests_free (UcWkRequests *r)
{
    if (r == NULL)
        return;
    g_signal_handlers_disconnect_by_data (r->web_view, r);
    if (r->idle_source != 0)
        g_source_remove (r->idle_source);
    /* the view goes: no request_cancelled() from inside uc_webview_free() */
    g_array_set_size (r->cancelled_ids, 0);
    while (r->dialogs_to_close->len > 0)
        close_dialog (g_ptr_array_steal_index (r->dialogs_to_close, 0));
    end_requests (r, UC_REQUEST_ANY, "view freed", REPORT_NONE);
    uc_request_table_free (r->table);
    g_array_unref (r->cancelled_ids);
    g_ptr_array_unref (r->dialogs_to_close);
    g_free (r->origin);
    g_free (r);
}

void
uc_wk_requests_host_navigation (UcWkRequests *r, gboolean leaves_page)
{
    /* A dialog blocks the page's script and every load waits behind it (WebChromeClient.cpp:593-667); the
     * authentication request belongs to the load being replaced. request_cancelled() follows from the main context:
     * no callback runs inside a uc_webview_* call. */
    end_requests (r, UC_REQUEST_ANY, "navigation", REPORT_LATER);
    /* after the dialog's "no" the page's script goes on until the web process reads the load: its next dialogs would
     * block the load again (uc_requests.c, navigation_quiet_us) */
    if (leaves_page)
        r->navigating_until_us = g_get_monotonic_time () + uc_request_decisions.navigation_quiet_us;
}

void
uc_wk_requests_user_input (UcWkRequests *r)
{
    if (r == NULL)
        return;
    /* by design (uc_requests.c): a dialog after the user's input is no runaway loop */
    if (uc_dialog_guard_user_input (&r->guard)) {
        g_message ("ucwebview: dialog loop guard ended by user input");
        uc_trace_request_guard_ended (r->host.wv);
    }
}

void
uc_wk_requests_load_changed (UcWkRequests *r, WebKitLoadEvent event)
{
    flush (r);
    r->navigating_until_us = 0;     /* the web process took the load: the page's dialogs are the host's again */
    switch (event) {
    case WEBKIT_LOAD_STARTED:
    case WEBKIT_LOAD_FINISHED:
        /* WebKit finished its own request before this signal; the ones it replaced or counted as authenticated
         * would wait for an answer that never comes */
        end_requests (r, UC_REQUEST_AUTH, "load", REPORT_NOW);
        break;
    case WEBKIT_LOAD_COMMITTED:
        /* a new document: a dialog of the old one (left in a process WebKit swapped away from) ends, the loop guard
         * starts afresh, dialogs name the new origin */
        end_requests (r, UC_REQUEST_DIALOG, "commit", REPORT_NOW);
        uc_dialog_guard_reset (&r->guard);
        g_free (r->origin);
        r->origin = uc_request_origin_from_uri (webkit_web_view_get_uri (r->web_view));
        break;
    default:
        break;
    }
}

gboolean
uc_wk_requests_load_failed (UcWkRequests *r, const GError *error)
{
    flush (r);
    r->navigating_until_us = 0;
    end_requests (r, UC_REQUEST_AUTH, "load", REPORT_NOW);
    /* a refused response (download, PDF, unknown type): WebKit's own handler shows nothing for it either
     * (WebKitWebView.cpp:616-631) */
    return !uc_request_decisions.report_policy_interruption
           && g_error_matches (error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE);
}

void
uc_wk_requests_web_process_terminated (UcWkRequests *r)
{
    flush (r);
    r->navigating_until_us = 0;
    end_requests (r, UC_REQUEST_ANY, "web process terminated", REPORT_NOW);
    uc_dialog_guard_reset (&r->guard);
}

int
uc_wk_requests_reply (UcWkRequests *r, guint64 id, int accept, const char *text)
{
    UcRequest *request = uc_request_table_lookup (r->table, id);

    switch (uc_request_check_reply (request, FALSE, accept)) {
    case UC_REPLY_UNKNOWN:
        g_debug ("ucwebview: uc_webview_request_reply(%" G_GUINT64_FORMAT "): no such pending request", id);
        return -1;
    case UC_REPLY_WRONG_KIND:
        g_warning ("ucwebview: uc_webview_request_reply(%" G_GUINT64_FORMAT ", accept=1) refused: an authentication "
                   "request takes its credentials through uc_webview_request_auth()", id);
        return -1;
    case UC_REPLY_OK:
    default:
        break;
    }
    if (accept && text != NULL && !g_utf8_validate (text, -1, NULL)) {
        g_warning ("ucwebview: uc_webview_request_reply(%" G_GUINT64_FORMAT "): the text is not UTF-8, the request "
                   "stays pending", id);
        return -1;
    }
    request = uc_request_table_steal (r->table, id);
    if (request->kind == UC_REQUEST_AUTH) {
        uc_trace_request_auth (r->host.wv, id, NULL, NULL);
        auth_finish (request, NULL, NULL, UC_AUTH_WITHOUT_CREDENTIALS);
    } else {
        uc_trace_request_reply (r->host.wv, id, request->type, accept ? TRUE : FALSE, text);
        dialog_finish (r, request, accept ? TRUE : FALSE, text);
    }
    g_free (request);
    return 0;
}

int
uc_wk_requests_auth (UcWkRequests *r, guint64 id, const char *user, const char *password)
{
    UcRequest *request = uc_request_table_lookup (r->table, id);

    switch (uc_request_check_reply (request, TRUE, 0)) {
    case UC_REPLY_UNKNOWN:
        g_debug ("ucwebview: uc_webview_request_auth(%" G_GUINT64_FORMAT "): no such pending request", id);
        return -1;
    case UC_REPLY_WRONG_KIND:
        g_warning ("ucwebview: uc_webview_request_auth(%" G_GUINT64_FORMAT ") refused: a script dialog takes "
                   "uc_webview_request_reply()", id);
        return -1;
    case UC_REPLY_OK:
    default:
        break;
    }
    if (user != NULL && (!g_utf8_validate (user, -1, NULL) || (password != NULL && !g_utf8_validate (password, -1, NULL)))) {
        g_warning ("ucwebview: uc_webview_request_auth(%" G_GUINT64_FORMAT "): the credentials are not UTF-8, the "
                   "request stays pending", id);
        return -1;
    }
    request = uc_request_table_steal (r->table, id);
    uc_trace_request_auth (r->host.wv, id, user, password);
    auth_finish (request, user, password, UC_AUTH_WITHOUT_CREDENTIALS);
    g_free (request);
    return 0;
}
