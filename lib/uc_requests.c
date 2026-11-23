/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Requests of the page to the host (library 0.7.0), the part without WebKit, see uc_requests.h.
 */
#include "uc_requests.h"

#include "uc_pdf_policy.h"
#include "uc_webview.h"

#include <string.h>

/*
 * The library's design decisions (package README "Requests (0.7.0)"). Each is one line; the WebKit side
 * (uc_requests_wk.c) reads them from here only.
 */
const UcRequestDecisions uc_request_decisions = {
    .before_unload_without_callback = UC_BEFORE_UNLOAD_LEAVE,     /* WebKit: stay, the navigation stops silently */
    .auth_without_callback = UC_AUTH_WITHOUT_CREDENTIALS,         /* WebKit: the load hangs until the next one */
    .client_certificate = UC_AUTH_WITHOUT_CREDENTIALS,            /* no certificate store on the remote */
    .before_unload_message = FALSE,                               /* the host shows a fixed text */
    .guard_quick_dialogs = 3,                                     /* loop guard: three quick dialogs (four shown), */
    .guard_window_us = G_USEC_PER_SEC,                            /* each less than 1 s after the previous answer */
    /* By design, a dialog that follows the user's tap or key press in the page is no runaway loop: that input ends
     * the guard. Without it, five taps on a button that asks confirm() got no question 16 s after the "6 alerts"
     * test, because the tripped guard answered every dialog of the page until the next page. */
    .guard_ends_on_input = TRUE,
    .report_policy_interruption = FALSE,                          /* refused responses are no failed loads */
    .pdf_as_unsupported = TRUE,                                   /* 0.6.1's refused PDFs: response_refused() */
    /* By design: once the host navigates away from a page with a pending dialog, the page's script goes on after the
     * dialog's "no" and can open the next dialog before the web process reads the load, which would block the
     * navigation again (WebChromeClient.cpp:593-667). Dialogs (not beforeunload) get WebKit's default until the next
     * load event of the view, at most this long. */
    .navigation_quiet_us = 2 * G_USEC_PER_SEC,
};

/* ---- request table ----------------------------------------------------------------------------------------- */

/* UI thread only (every uc_webview_* call is), so no atomics */
static guint64 last_id;

guint64
uc_request_next_id (void)
{
    return ++last_id;
}

struct _UcRequestTable {
    GPtrArray *pending;     /* UcRequest*, oldest first (ids grow) */
};

UcRequestTable *
uc_request_table_new (void)
{
    UcRequestTable *table = g_new0 (UcRequestTable, 1);
    table->pending = g_ptr_array_new_with_free_func (g_free);
    return table;
}

void
uc_request_table_free (UcRequestTable *table)
{
    if (table == NULL)
        return;
    if (table->pending->len > 0)
        g_critical ("ucwebview: %u request(s) still pending when the table is freed", table->pending->len);
    g_ptr_array_unref (table->pending);
    g_free (table);
}

UcRequest *
uc_request_table_add (UcRequestTable *table, UcRequestKind kind, int type, gpointer object)
{
    UcRequest *request = g_new0 (UcRequest, 1);

    request->id = uc_request_next_id ();
    request->kind = kind;
    request->type = type;
    request->object = object;
    g_ptr_array_add (table->pending, request);
    return request;
}

static gboolean
index_of (UcRequestTable *table, guint64 id, guint *index)
{
    if (id == 0)
        return FALSE;
    for (guint i = 0; i < table->pending->len; i++)
        if (((UcRequest *) g_ptr_array_index (table->pending, i))->id == id) {
            *index = i;
            return TRUE;
        }
    return FALSE;
}

UcRequest *
uc_request_table_lookup (UcRequestTable *table, guint64 id)
{
    guint index;
    return index_of (table, id, &index) ? g_ptr_array_index (table->pending, index) : NULL;
}

UcRequest *
uc_request_table_find_object (UcRequestTable *table, gconstpointer object)
{
    for (guint i = 0; i < table->pending->len; i++) {
        UcRequest *request = g_ptr_array_index (table->pending, i);
        if (request->object == object)
            return request;
    }
    return NULL;
}

UcRequest *
uc_request_table_steal (UcRequestTable *table, guint64 id)
{
    guint index;
    return index_of (table, id, &index) ? g_ptr_array_steal_index (table->pending, index) : NULL;
}

GPtrArray *
uc_request_table_steal_kind (UcRequestTable *table, UcRequestKind kind)
{
    GPtrArray *stolen = g_ptr_array_new_with_free_func (g_free);
    guint i = 0;

    while (i < table->pending->len) {
        UcRequest *request = g_ptr_array_index (table->pending, i);
        if (kind == UC_REQUEST_ANY || request->kind == kind)
            g_ptr_array_add (stolen, g_ptr_array_steal_index (table->pending, i));   /* keeps the order */
        else
            i++;
    }
    return stolen;
}

guint
uc_request_table_count (UcRequestTable *table, UcRequestKind kind)
{
    guint n = 0;

    for (guint i = 0; i < table->pending->len; i++)
        if (kind == UC_REQUEST_ANY || ((UcRequest *) g_ptr_array_index (table->pending, i))->kind == kind)
            n++;
    return n;
}

UcReplyCheck
uc_request_check_reply (const UcRequest *request, gboolean is_auth_call, int accept)
{
    if (request == NULL)
        return UC_REPLY_UNKNOWN;
    if (is_auth_call)
        return request->kind == UC_REQUEST_AUTH ? UC_REPLY_OK : UC_REPLY_WRONG_KIND;
    /* uc_webview_request_reply(): any answer for a dialog; for authentication only the "no" (credentials need
     * uc_webview_request_auth()) */
    if (request->kind == UC_REQUEST_AUTH)
        return accept ? UC_REPLY_WRONG_KIND : UC_REPLY_OK;
    return UC_REPLY_OK;
}

/* ---- script dialogs ------------------------------------------------------------------------------------------ */

UcDialogAnswer
uc_dialog_answer (int type, gboolean accept, const char *text)
{
    UcDialogAnswer answer = { FALSE, FALSE, FALSE, NULL };

    switch (type) {
    case UC_WEBVIEW_DIALOG_CONFIRM:
    case UC_WEBVIEW_DIALOG_BEFORE_UNLOAD:
        /* WebKitGTK's dialog: OK and "Leave Page" set TRUE, Cancel and "Stay on Page" FALSE
         * (WebKitScriptDialogImpl.cpp:346-374); WebKit's beforeunload completion takes TRUE as "leave" */
        answer.set_confirmed = TRUE;
        answer.confirmed = accept ? TRUE : FALSE;
        break;
    case UC_WEBVIEW_DIALOG_PROMPT:
        /* an unset text closes the prompt with a null string, prompt() returns null (WebKitScriptDialog.cpp:206-208,
         * LocalDOMWindow.cpp:1269-1290); "" returns "" */
        if (accept) {
            answer.set_text = TRUE;
            answer.text = text != NULL ? text : "";
        }
        break;
    case UC_WEBVIEW_DIALOG_ALERT:
    default:
        break;
    }
    return answer;
}

const char *
uc_dialog_type_name (int type)
{
    switch (type) {
    case UC_WEBVIEW_DIALOG_ALERT:
        return "alert";
    case UC_WEBVIEW_DIALOG_CONFIRM:
        return "confirm";
    case UC_WEBVIEW_DIALOG_PROMPT:
        return "prompt";
    case UC_WEBVIEW_DIALOG_BEFORE_UNLOAD:
        return "beforeunload";
    default:
        return "dialog";
    }
}

void
uc_dialog_guard_reset (UcDialogGuard *guard)
{
    guard->last_end_us = 0;
    guard->quick = 0;
    guard->tripped = FALSE;
}

UcGuardVerdict
uc_dialog_guard_check (UcDialogGuard *guard, int type, gint64 now_us)
{
    if (type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD)
        return UC_GUARD_SHOW;
    if (guard->tripped)
        return UC_GUARD_SUPPRESS;
    if (guard->last_end_us != 0 && now_us - guard->last_end_us < uc_request_decisions.guard_window_us)
        guard->quick++;
    else
        guard->quick = 0;
    if (guard->quick > uc_request_decisions.guard_quick_dialogs) {
        guard->tripped = TRUE;
        return UC_GUARD_TRIPPED;
    }
    return UC_GUARD_SHOW;
}

void
uc_dialog_guard_ended (UcDialogGuard *guard, int type, gint64 now_us)
{
    if (type != UC_WEBVIEW_DIALOG_BEFORE_UNLOAD)
        guard->last_end_us = now_us != 0 ? now_us : 1;
}

gboolean
uc_dialog_guard_user_input (UcDialogGuard *guard)
{
    gboolean tripped = guard->tripped;

    if (!uc_request_decisions.guard_ends_on_input)
        return FALSE;
    /* all of it, not only the trip: a dialog right after the input is not quick, whenever the last answer was */
    uc_dialog_guard_reset (guard);
    return tripped;
}

/* ---- origins ------------------------------------------------------------------------------------------------- */

static int
default_port (const char *scheme)
{
    static const struct {
        const char *scheme;
        int port;
    } ports[] = { { "http", 80 }, { "https", 443 }, { "ws", 80 }, { "wss", 443 }, { "ftp", 21 } };

    for (gsize i = 0; i < G_N_ELEMENTS (ports); i++)
        if (strcmp (scheme, ports[i].scheme) == 0)
            return ports[i].port;
    return -1;
}

char *
uc_request_origin_format (const char *scheme, const char *host, int port)
{
    char *lower_scheme, *lower_host, *origin;
    gboolean ipv6;

    if (scheme == NULL || *scheme == '\0' || host == NULL || *host == '\0')
        return NULL;
    lower_scheme = g_ascii_strdown (scheme, -1);
    lower_host = g_ascii_strdown (host, -1);
    ipv6 = strchr (lower_host, ':') != NULL && lower_host[0] != '[';
    if (port > 0 && port != default_port (lower_scheme))
        origin = g_strdup_printf ("%s://%s%s%s:%d", lower_scheme, ipv6 ? "[" : "", lower_host, ipv6 ? "]" : "", port);
    else
        origin = g_strdup_printf ("%s://%s%s%s", lower_scheme, ipv6 ? "[" : "", lower_host, ipv6 ? "]" : "");
    g_free (lower_scheme);
    g_free (lower_host);
    return origin;
}

char *
uc_request_origin_from_uri (const char *uri)
{
    const char *scheme;
    GUri *parsed;
    char *origin;

    if (uri == NULL)
        return NULL;
    scheme = g_uri_peek_scheme (uri);   /* lower case */
    if (scheme == NULL)
        return NULL;
    if (strcmp (scheme, "file") == 0)
        return g_strdup ("file://");
    if (strcmp (scheme, "blob") == 0) {
        /* blob:https://example.com/uuid: the origin of the document that made it */
        const char *inner = uri + strlen ("blob:");
        const char *inner_scheme = g_uri_peek_scheme (inner);
        return inner_scheme != NULL && strcmp (inner_scheme, "blob") != 0 ? uc_request_origin_from_uri (inner) : NULL;
    }
    parsed = g_uri_parse (uri, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_ENCODED, NULL);
    if (parsed == NULL)
        return NULL;
    origin = uc_request_origin_format (g_uri_get_scheme (parsed), g_uri_get_host (parsed), g_uri_get_port (parsed));
    g_uri_unref (parsed);
    return origin;
}

/* ---- new windows --------------------------------------------------------------------------------------------- */

UcNewWindowVerdict
uc_request_new_window_verdict (const char *uri, const char *method)
{
    const char *scheme = uri != NULL ? g_uri_peek_scheme (uri) : NULL;
    GUri *parsed;
    gboolean has_host;

    /* Only what a page could also open in its own frame without trouble: the library loads it with an API load,
     * which skips WebCore's checks for page-initiated loads (FrameLoader.cpp:1513-1516, see uc_uri_policy.h). */
    if (scheme == NULL || (strcmp (scheme, "http") != 0 && strcmp (scheme, "https") != 0))
        return UC_NEW_WINDOW_REFUSED_SCHEME;
    parsed = g_uri_parse (uri, G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_ENCODED, NULL);
    has_host = parsed != NULL && g_uri_get_host (parsed) != NULL && *g_uri_get_host (parsed) != '\0';
    if (parsed != NULL)
        g_uri_unref (parsed);
    if (!has_host)
        return UC_NEW_WINDOW_REFUSED_SCHEME;
    /* A form posted to a new window: whether WebKit hands the body over with the navigation action is not verified, and
     * a POST without it, or the same URL as a GET, is a different request. */
    if (method != NULL && g_ascii_strcasecmp (method, "GET") != 0)
        return UC_NEW_WINDOW_REFUSED_METHOD;
    return UC_NEW_WINDOW_LOAD;
}

/* ---- responses ----------------------------------------------------------------------------------------------- */

static gboolean
is_blank (char c)
{
    /* isASCIIWhitespace: space, tab, line feed, form feed, carriage return (and vertical tab, as WebCore's
     * isUnicodeCompatibleASCIIWhitespace) */
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == '\v';
}

gboolean
uc_request_is_attachment (const char *content_disposition)
{
    const char *start, *end;
    static const char attachment[] = "attachment";

    if (content_disposition == NULL)
        return FALSE;
    start = content_disposition;
    end = strchr (start, ';');
    if (end == NULL)
        end = start + strlen (start);
    while (start < end && is_blank (*start))
        start++;
    while (end > start && is_blank (end[-1]))
        end--;
    return (gsize) (end - start) == strlen (attachment) && g_ascii_strncasecmp (start, attachment, strlen (attachment)) == 0;
}

UcResponseVerdict
uc_request_response_verdict (const char *mime_type, const char *content_disposition, gboolean mime_type_supported,
                             guint status_code, gboolean pdf_viewer)
{
    if (uc_request_is_attachment (content_disposition))
        return UC_RESPONSE_DOWNLOAD;
    if (uc_pdf_policy_check_response (mime_type, pdf_viewer) == UC_PDF_POLICY_IGNORE)
        return UC_RESPONSE_PDF;
    if (!mime_type_supported && status_code != 204)
        return UC_RESPONSE_UNSUPPORTED;
    return UC_RESPONSE_SHOW;
}
