/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Requests of the page to the host (library 0.7.0), the part without WebKit: GLib only, so that it is unit-tested on
 * a host without WPE (tests/test-requests.c, also under ASan). The WebKit side is uc_requests_wk.c.
 *
 *  - The request table: script dialogs and HTTP authentication that the host answers later by id
 *    (uc_webview_request_reply(), uc_webview_request_auth()). Ids are process-wide, from 1, never 0, never reused,
 *    so a late or second answer finds nothing (-1) instead of a freed WebKit object. Every request ends exactly once:
 *    it leaves the table with uc_request_table_steal*() before anything else happens to it, so a callback that
 *    answers or navigates from inside another callback cannot end it a second time.
 *  - The answer a dialog gets for accept/reject, the dialog loop guard, the origin shown with a request, the scheme
 *    filter for new windows and the verdict for responses WebKit cannot or should not show.
 *  - The library's design decisions (uc_request_decisions), one line each.
 */
#ifndef UC_REQUESTS_H
#define UC_REQUESTS_H

#include <glib.h>

G_BEGIN_DECLS

/* ---- design decisions, package README "Requests (0.7.0)" ------------------------------------------------------ */

typedef enum {
    UC_BEFORE_UNLOAD_LEAVE,     /* the navigation goes on */
    UC_BEFORE_UNLOAD_STAY,      /* WebKit's own default without a handler: the navigation stops silently */
} UcBeforeUnloadAnswer;

typedef enum {
    UC_AUTH_WITHOUT_CREDENTIALS,  /* authenticate(NULL): the server's 401 page loads (WebKitGTK's "Cancel") */
    UC_AUTH_CANCEL_LOAD,          /* webkit_authentication_request_cancel(): the load ends, nothing is shown */
} UcAuthAnswer;

typedef struct {
    UcBeforeUnloadAnswer before_unload_without_callback; /* host without script_dialog: LEAVE */
    UcAuthAnswer auth_without_callback;          /* host without authenticate: WITHOUT_CREDENTIALS (WebKit: hangs) */
    UcAuthAnswer client_certificate;             /* certificate and PIN requests, never shown: WITHOUT_CREDENTIALS */
    gboolean before_unload_message;              /* FALSE: the host gets NULL and shows a fixed text */
    guint guard_quick_dialogs;                   /* 3: quick dialogs before WebKit's default answers (per document,
                                                  * counted again after the user's input) */
    gint64 guard_window_us;                      /* 1 s: "quick" = less than this after the previous answer */
    gboolean guard_ends_on_input;                /* TRUE: the user's input in the page ends the guard */
    gboolean report_policy_interruption;         /* FALSE: no FAILED for FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE */
    gboolean pdf_as_unsupported;                 /* TRUE: a refused PDF is response_refused(UNSUPPORTED) */
    gint64 navigation_quiet_us;                  /* 2 s: dialogs after a host navigation get WebKit's default */
} UcRequestDecisions;

/* the decisions in force (uc_requests.c) */
extern const UcRequestDecisions uc_request_decisions;

/* ---- request table ----------------------------------------------------------------------------------------- */

/* uc_webview_request_reply() / uc_webview_request_auth() kinds; the values are not ABI */
typedef enum {
    UC_REQUEST_ANY = 0,     /* only as a filter of uc_request_table_steal_kind()/count() */
    UC_REQUEST_DIALOG,      /* a script dialog (alert, confirm, prompt, beforeunload) */
    UC_REQUEST_AUTH,        /* an HTTP authentication request */
} UcRequestKind;

typedef struct {
    guint64 id;
    UcRequestKind kind;
    int type;               /* dialog: UC_WEBVIEW_DIALOG_*; auth: the scheme (UC_WEBVIEW_AUTH_SCHEME_*) */
    gpointer object;        /* the WebKit object; the table's owner holds a reference */
    gulong handler;         /* the owner's signal handler on object (auth: "cancelled"), 0 for none */
} UcRequest;

typedef struct _UcRequestTable UcRequestTable;

/* The next request id of the process: 1, 2, 3, ... (UI thread only, like every library call). */
guint64         uc_request_next_id          (void);

UcRequestTable *uc_request_table_new        (void);
/* The table must be empty: requests end through uc_request_table_steal*(), whose caller frees them with g_free(). */
void            uc_request_table_free       (UcRequestTable *table);
/* A new pending request with a fresh id; the table owns the UcRequest, not object. */
UcRequest      *uc_request_table_add        (UcRequestTable *table, UcRequestKind kind, int type, gpointer object);
/* NULL for 0, unknown and ended ids. */
UcRequest      *uc_request_table_lookup     (UcRequestTable *table, guint64 id);
/* The pending request that holds object, NULL if none. */
UcRequest      *uc_request_table_find_object (UcRequestTable *table, gconstpointer object);
/* Removes and returns the request (free it with g_free()), NULL for unknown and ended ids. */
UcRequest      *uc_request_table_steal      (UcRequestTable *table, guint64 id);
/* Removes and returns the pending requests of kind (UC_REQUEST_ANY: all), oldest first; the array frees them. */
GPtrArray      *uc_request_table_steal_kind (UcRequestTable *table, UcRequestKind kind);
guint           uc_request_table_count      (UcRequestTable *table, UcRequestKind kind);

/* Which public call may answer a request. */
typedef enum {
    UC_REPLY_OK = 0,
    UC_REPLY_UNKNOWN,       /* no such pending request (0, never issued, already ended) */
    UC_REPLY_WRONG_KIND,    /* uc_webview_request_auth() for a dialog, or accept=1 for an authentication request */
} UcReplyCheck;

/* is_auth_call: uc_webview_request_auth(); else uc_webview_request_reply() with accept. request may be NULL. */
UcReplyCheck    uc_request_check_reply      (const UcRequest *request, gboolean is_auth_call, int accept);

/* ---- script dialogs ------------------------------------------------------------------------------------------ */

/* What to set on a WebKitScriptDialog before it closes. WebKit closes a dialog with the values set so far
 * (WebKitScriptDialog.cpp:192-213): confirmed FALSE and no text unless set. */
typedef struct {
    gboolean set_confirmed;     /* confirm and beforeunload: webkit_script_dialog_confirm_set_confirmed() */
    gboolean confirmed;         /* confirm: OK; beforeunload: leave */
    gboolean set_text;          /* prompt: webkit_script_dialog_prompt_set_text(); unset = prompt() returns null */
    const char *text;           /* the host's text, "" for NULL; valid as long as the host's string */
} UcDialogAnswer;

/* accept = the host's yes: alert closed, confirm true, prompt text (NULL = ""), beforeunload leave. Its no: alert
 * closed, confirm false, prompt null, beforeunload stay. type is UC_WEBVIEW_DIALOG_*. */
UcDialogAnswer  uc_dialog_answer            (int type, gboolean accept, const char *text);
/* "alert", "confirm", "prompt", "beforeunload", "dialog" for others */
const char     *uc_dialog_type_name         (int type);

/* The dialog loop guard: a dialog that arrives less than guard_window_us after the end of the previous one is quick;
 * after guard_quick_dialogs quick dialogs in a row in one document, every further dialog of that document gets WebKit's
 * default answer at once (closed, false, null), until the user taps or presses a key in the page
 * (uc_dialog_guard_user_input()) or the next main-frame commit. With 3 and 1 s, `for (;;) alert(1)` shows four alerts.
 * beforeunload neither counts nor is suppressed: it comes once per navigation, and WebKit's default ("stay") would keep
 * the user on the page. */
typedef struct {
    gint64 last_end_us;     /* end of the previous dialog of the document, 0 = none yet */
    guint quick;            /* quick dialogs in a row */
    gboolean tripped;
} UcDialogGuard;

typedef enum {
    UC_GUARD_SHOW,          /* pass the dialog to the host */
    UC_GUARD_TRIPPED,       /* the first suppressed dialog of the document: log it once */
    UC_GUARD_SUPPRESS,      /* WebKit's default answer */
} UcGuardVerdict;

void            uc_dialog_guard_reset       (UcDialogGuard *guard);
UcGuardVerdict  uc_dialog_guard_check       (UcDialogGuard *guard, int type, gint64 now_us);
/* A dialog the host saw ended (answer, or the library ended it). */
void            uc_dialog_guard_ended       (UcDialogGuard *guard, int type, gint64 now_us);
/* The user's input reached the page (a touch down, a button or key press, text input): by design, a dialog that follows
 * it is no runaway loop, so the guard starts afresh, as at a commit: not tripped, no quick dialog counted, the next
 * dialog not quick. A page that loops by itself trips it again after the same number of dialogs. Returns TRUE when the
 * guard had tripped (the caller logs its end once). */
gboolean        uc_dialog_guard_user_input  (UcDialogGuard *guard);

/* ---- origins ------------------------------------------------------------------------------------------------- */

/* "scheme://host[:port]" with scheme and host in lower case, the port only when it is not the scheme's default
 * (http 80, https 443, ws 80, wss 443, ftp 21) and > 0, an IPv6 host in brackets. NULL without scheme or host. */
char           *uc_request_origin_format    (const char *scheme, const char *host, int port);
/* The origin of a document at uri: as above for a URI with a host, "file://" for file:, the inner URI's origin for
 * blob:; NULL for URIs without a host (about:blank, data:) and for NULL. No user info, path, query or fragment. */
char           *uc_request_origin_from_uri  (const char *uri);

/* ---- new windows --------------------------------------------------------------------------------------------- */

typedef enum {
    UC_NEW_WINDOW_LOAD = 0,         /* load it into this view */
    UC_NEW_WINDOW_REFUSED_SCHEME,   /* not http/https: data:, javascript:, blob:, file:, about: */
    UC_NEW_WINDOW_REFUSED_METHOD,   /* not a GET (a form posted to a new window): the body would not come along */
} UcNewWindowVerdict;

/* uri and the HTTP method of the request a page wants a new window for (method NULL = GET). */
UcNewWindowVerdict uc_request_new_window_verdict (const char *uri, const char *method);

/* ---- responses ----------------------------------------------------------------------------------------------- */

typedef enum {
    UC_RESPONSE_SHOW = 0,           /* WebKit decides as usual */
    UC_RESPONSE_DOWNLOAD,           /* Content-Disposition: attachment: WebKit would download it */
    UC_RESPONSE_PDF,                /* a PDF while the PDF viewer is off (uc_pdf_policy.h) */
    UC_RESPONSE_UNSUPPORTED,        /* a type WebKit cannot show (WebKit ignores it) */
} UcResponseVerdict;

/* TRUE when a Content-Disposition value is an attachment: its type before ';', without ASCII blanks, is
 * "attachment" in any case (WebCore's ResourceResponseBase::isAttachment(), ResourceResponseBase.cpp:834-840). */
gboolean          uc_request_is_attachment       (const char *content_disposition);
/* The verdict for a navigation response, in WebKit's order (WebKitWebView.cpp:651-664): an attachment is a download
 * whatever its type; then a PDF while the viewer is off; then a type WebKit cannot show, unless the status is 204
 * (WebKit uses those). */
UcResponseVerdict uc_request_response_verdict    (const char *mime_type, const char *content_disposition,
                                                  gboolean mime_type_supported, guint status_code,
                                                  gboolean pdf_viewer);

G_END_DECLS

#endif /* UC_REQUESTS_H */
