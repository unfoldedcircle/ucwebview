/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --link-test of ucwebview-test, see ucwebview-test-links.h.
 *
 * Why it exists: taps in remote-ui ran a page's onclick handlers (script dialogs came up) but no tap on a link
 * navigated, window.open() and window.close() did nothing and a checkbox did not check, while a still tap of this tool
 * on a block link navigated. The test taps every kind of element the way remote-ui does and varies one thing at a
 * time, so that one run on the device tells which of the differences matters.
 * What it found: the taps were fine; the page's inline text had no height because no installed font matched the generic
 * family (the font element below, README "Fonts"), and the page called open() and close() from inline onclick handlers,
 * where they are document.open() and document.close() (JSHTMLElementCustom.cpp:125-136 puts the document into the
 * handler's scope; Document+HTML.idl:61-63).
 *
 * How a tap becomes a click (WPE WebKit 2.54.1): uc_webview_touch() emits WPEView::event; WebKit sends the touch to
 * the web process and, when the page did not handle it, hands it to the view's gesture controller
 * (PageClientImpl.cpp:248-266). At the release of a tap the controller reports WPE_GESTURE_TAP and WebKit synthesizes
 * a pointer move, press and release at the press position (WPEWebViewPlatform.cpp:433-473;
 * WPEGestureDetector.cpp:49-56,
 * 60-75: moves within the drag threshold keep the tap and its position). WebCore's release handler dispatches the
 * click under a user gesture (EventHandler.cpp:2573, 2627-2630), an anchor navigates from its default handler
 * (HTMLAnchorElement.cpp:189-191, 559-630), window.open() needs that gesture (LocalDOMWindow.cpp:401-415, 3003-3010),
 * window.close() needs none but a history of at most one entry (DOMWindow.cpp:127-153).
 *
 * What the tool checks, per element: the page's own log (title "uclink:<n> hash=.. checked=.. closed=.. log=..":
 * every click it saw with isTrusted and navigator.userActivation, the result of open() and close()), the load
 * callbacks (a link's page committed), and with library 0.7.0 the new_window and close_requested callbacks. Library
 * 0.6 has no new-window API: there a window.open() or target=_blank that reached the page is "not checkable".
 */
#include "ucwebview-test-links.h"

#include "uc_webview.h"

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the request API of library 0.7.0 (script dialogs, new windows, window.close()) */
#if defined(UC_WEBVIEW_NEW_WINDOW_LOADED) && defined(UC_WEBVIEW_DIALOG_ALERT)
#define LINK_TEST_REQUESTS 1
#else
#define LINK_TEST_REQUESTS 0
#endif

/* ---- options ----------------------------------------------------------------------------------------------- */

static gboolean opt_link_test;
static char *opt_link_variants;
static char *opt_link_elements;

const GOptionEntry link_test_entries[] = {
    { "link-test", 0, 0, G_OPTION_ARG_NONE, &opt_link_test,
      "Tap links, a checkbox and window.open()/window.close() buttons of a built-in page (served by the tool on "
      "127.0.0.1) through uc_webview_touch() like remote-ui, check what each tap did and print PASS/FAIL per element "
      "and variant", NULL },
    { "link-variants", 0, 0, G_OPTION_ARG_STRING, &opt_link_variants,
      "The --link-test variants, comma separated (default: all): still, moves, still-nopinch, moves-nopinch, "
      "still-norequests (library 0.7.0), focus, keys, dialog, loop, unfocused", "LIST" },
    { "link-elements", 0, 0, G_OPTION_ARG_STRING, &opt_link_elements,
      "The --link-test elements, comma separated (default: all): font, frag, link, block, check, label, blank, open, "
      "confirmopen, close", "LIST" },
    { NULL }
};

/* ---- plan -------------------------------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *what;
    gboolean moves;     /* the finger slides up to 3/4 of the drag threshold, like a user's taps (0..16 px) */
    gboolean pinch;     /* pinch callback set: the library's gesture controller wraps WebKit's (uc_wpe_gesture.c) */
    gboolean requests;  /* the request callbacks of library 0.7.0 set, like remote-ui */
    gboolean dialog;    /* an alert() before every tap, answered 300 ms later, the view's focus out and in around it */
    gboolean refocus;   /* the page loads in an unfocused view, which gets the focus 300 ms before the tap */
    gboolean keys;      /* a Down key press and release 300 ms before the tap, like the remote's DPAD */
    gboolean loop;      /* first, once per variant: a button that calls alert() six times, answered like remote-ui */
    gboolean unfocused; /* the view has no focus when tapped; WebKit focuses it inside the tap
                         * (WPEWebViewPlatform.cpp:468) */
} Variant;

static const Variant variants[] = {
    { "still", "still taps (down, up 80 ms later), pinch callback set (the library's gesture controller)",
      FALSE, TRUE, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE },
    { "moves", "taps that slide 3/4 of the drag threshold before the release, pinch callback set",
      TRUE, TRUE, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE },
    { "still-nopinch", "still taps, no pinch callback (WebKit's own gesture controller)",
      FALSE, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE },
    { "moves-nopinch", "sliding taps, no pinch callback (WebKit's own gesture controller)",
      TRUE, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE },
#if LINK_TEST_REQUESTS
    { "still-norequests", "still taps, pinch callback, none of the request callbacks of 0.7.0",
      FALSE, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE },
#endif
    { "focus", "still taps; the page loads while the view has no focus, the focus comes 300 ms before the tap "
      "(remote-ui's item gets the active focus after the address field)",
      FALSE, TRUE, TRUE, FALSE, TRUE, FALSE, FALSE, FALSE },
    { "keys", "still taps after a Down key press and release (the remote's DPAD) 300 ms before",
      FALSE, TRUE, TRUE, FALSE, FALSE, TRUE, FALSE, FALSE },
    { "dialog", "sliding taps after an alert() that the host answers 300 ms later, focus out and in around it like "
      "remote-ui's dialog", TRUE, TRUE, TRUE, TRUE, FALSE, FALSE, FALSE, FALSE },
    { "loop", "sliding taps after the page's \"6 alerts\" button once at the start (0.7.0: answered like remote-ui, "
      "the library's dialog loop guard closes the last ones)", TRUE, TRUE, TRUE, FALSE, FALSE, FALSE, TRUE, FALSE },
    { "unfocused", "still taps on a view without focus (set_focused(0) before every page); WebKit focuses the view "
      "between its synthesized press and release", FALSE, TRUE, TRUE, FALSE, FALSE, FALSE, FALSE, TRUE },
};

typedef enum {
    CHECK_FONT,     /* no tap: the page's generic font families have an ascent and a descent (0.6.3) */
    CHECK_HASH,     /* location.hash is #target */
    CHECK_LOAD,     /* the view committed the element's path */
    CHECK_CHECKED,  /* the checkbox is checked */
    CHECK_WINDOW,   /* WebKit asked the host for a new window (0.7.0: new_window, the page loads into the view) */
    CHECK_CLOSED,   /* window.close() went through (window.closed; 0.7.0: close_requested) */
} Check;

typedef struct {
    const char *id;     /* the element's id in the page */
    const char *what;
    Check check;
    const char *path;   /* CHECK_LOAD, CHECK_WINDOW: the URL path and query the effect loads */
    const char *rect;   /* the id whose rect is tapped (the label: its text) */
    gboolean fresh;     /* in a view of its own: WebKit closes a window that script did not open only while its
                         * history has one entry (DOMWindow.cpp:140-143); the other elements' pages add entries */
} Element;

/* font first: when the page's font has no metrics, inline text has no height and the taps on frag, link and label
 * cannot reach them (the firmware ships Poppins, Space Mono and Noto Color Emoji only, and WebKit's Skia font cache
 * takes a fontconfig match only when one of the font's family names is in the pattern,
 * SkFontMgr_fontconfig.cpp:876-913; without a match for sans-serif the primary font is SkTypeface::MakeEmpty(),
 * FontCacheSkia.cpp:180-200, ascent and descent 0, while the glyphs still draw through the per-character fallback,
 * FontCacheSkia.cpp:142; the package's 56-uc-webview-fonts.conf adds the aliases).
 * close last: a page that window.close() accepted stays closing (Page::setIsClosing(), DOMWindow.cpp:151) */
static const Element elements[] = {
    { "font", "the generic font families sans-serif, serif and monospace have metrics", CHECK_FONT, NULL, NULL, FALSE },
    { "frag", "same-document link #target", CHECK_HASH, NULL, "frag", FALSE },
    { "link", "inline text link to /second.html", CHECK_LOAD, "/second.html?link", "link", FALSE },
    { "block", "block link to /second.html", CHECK_LOAD, "/second.html?block", "block", FALSE },
    { "check", "checkbox", CHECK_CHECKED, NULL, "check", FALSE },
    { "label", "the checkbox's label text", CHECK_CHECKED, NULL, "labeltext", FALSE },
    { "blank", "target=_blank link", CHECK_WINDOW, "/popup.html?blank", "blank", FALSE },
    { "open", "button calling window.open()", CHECK_WINDOW, "/popup.html?open", "open", FALSE },
    /* confirm+open: in WebKit the click's user gesture lasts the whole handler, a dialog inside it included
     * (EventHandler.cpp:2573, UserGestureIndicator.cpp:182-250, LocalDOMWindow.cpp:401-415) */
    { "confirmopen", "button calling window.open() when confirm() returns true", CHECK_WINDOW,
      "/popup.html?confirmopen", "confirmopen", FALSE },
    { "close", "button calling window.close()", CHECK_CLOSED, NULL, "close", TRUE },
};

/* the loop variant's first page: its "6 alerts" button (the check is not used) */
static const Element prelude_element = { "alerts", "six alert() calls in a loop", CHECK_HASH, NULL, "alerts", FALSE };

#define N_VARIANTS G_N_ELEMENTS (variants)
#define N_ELEMENTS G_N_ELEMENTS (elements)

/* The test page. Body font and line height like the page that showed the problem, explicitly white. The page logs every
 * click it sees (capture listener on the window, before the element's own handler) with isTrusted and the transient
 * user activation (navigator.userActivation, set by the press, UserGestureIndicator.cpp:211-216), the hash change,
 * the checkbox change and what open() and close() returned, and puts its state into the title after each event.
 * 300 ms after the load, and after a resize, it reports the viewport, the ascent/descent of the generic font families
 * at 22 px (canvas measureText, fontBoundingBoxAscent is FontMetrics::intAscent() of the primary font,
 * CanvasRenderingContext2DBase.cpp:3094-3095) and the elements' rects ("ucready ?e=link&v=still 480x800
 * fonts:sans-serif=23/8,serif=23/8,monospace=25/8 frag:8,20,102,25 ..."), which the tool taps. No touch listeners:
 * WebKit sends the touches to the gesture controller unhandled, as on that page. */
static const char link_page_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>link-test</title>"
    "<style>body{margin:8px;background:#fff;color:#000;font:22px sans-serif;line-height:2.2}"
    "#block{display:block;height:96px;line-height:96px;text-align:center;background:#dde8f4}</style></head><body>"
    "<a id=frag href='#target'>fragment</a> <a id=link href='/second.html?link'>link</a> "
    "<a id=blank target=_blank href='/popup.html?blank'>_blank</a><br>"
    "<label id=label><input type=checkbox id=check> <span id=labeltext>ask before leaving</span></label><br>"
    "<button id=alert onclick=\"alert('link-test');__ucLog('alert:done')\">alert</button> "
    "<button id=open onclick='__ucOpen()'>open</button> "
    "<button id=confirmopen onclick='__ucConfirmOpen()'>confirm+open</button> "
    "<button id=close onclick='__ucClose()'>close</button> "
    "<button id=alerts onclick=\"for(var i=0;i<6;i++)alert('loop '+i);__ucLog('alerts:done')\">6 alerts</button><br>"
    "<a id=block href='/second.html?block'>block link</a><p id=target>target</p>"
    "<script>(function(){"
    "var log=[],seq=0,"
    "ids=['frag','link','blank','check','labeltext','alert','open','confirmopen','close','alerts','block'];"
    "function act(){var u=navigator.userActivation;"
    "return u?(u.isActive?'active':(u.hasBeenActive?'sticky':'none')):'unknown';}"
    "function state(){var c=document.getElementById('check');"
    "document.title='uclink:'+(++seq)+' hash='+(location.hash||'-')+' checked='+(c.checked?1:0)"
    "+' closed='+(window.closed?1:0)+' log='+log.join('|');}"
    "window.__ucLog=function(t){log.push(t);state();};"
    "addEventListener('click',function(e){var t=e.target;while(t&&!t.id)t=t.parentNode;"
    "__ucLog('click:'+(t&&t.id?t.id:'?')+':'+(e.isTrusted?'trusted':'untrusted')+':'+act()+':'"
    "+(e.pointerType||'-'));},true);"
    "addEventListener('hashchange',function(){__ucLog('hashchange');});"
    "document.getElementById('check').addEventListener('change',function(){__ucLog('change:'+this.checked);});"
    "window.__ucOpen=function(){var a=act(),w=null,x='';try{w=open('/popup.html?open');}catch(e){x=':'+e.name;}"
    "__ucLog('open:'+(w?'window':'null')+':'+a+x);};"
    "window.__ucConfirmOpen=function(){var a=act(),c=confirm('link-test confirm+open'),b=act(),w=null,x='';"
    "if(c)try{w=open('/popup.html?confirmopen');}catch(e){x=':'+e.name;}"
    "__ucLog('confirmopen:confirm='+c+':'+(w?'window':'null')+':'+a+'/'+b+x);};"
    "window.__ucClose=function(){var a=act();close();__ucLog('close:'+(window.closed?'closing':'refused')+':'+a);};"
    "function fonts(){var c=document.createElement('canvas').getContext('2d'),f=[];"
    "['sans-serif','serif','monospace'].forEach(function(n){c.font='22px '+n;var m=c.measureText('Hxg');"
    "f.push(n+'='+Math.round(m.fontBoundingBoxAscent)+'/'+Math.round(m.fontBoundingBoxDescent));});"
    "return 'fonts:'+f.join(',');}"
    "function ready(){var r=[];ids.forEach(function(id){"
    "var b=document.getElementById(id).getBoundingClientRect();"
    "r.push(id+':'+Math.round(b.left)+','+Math.round(b.top)+','+Math.round(b.width)+','+Math.round(b.height));});"
    "document.title='ucready '+location.search+' '+innerWidth+'x'+innerHeight+' '+fonts()+' '+r.join(' ');}"
    "addEventListener('load',function(){setTimeout(ready,300);});addEventListener('resize',ready);"
    "})();</script></body></html>";

static const char second_page_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>link-test second</title>"
    "</head><body style='background:#fff;font:22px sans-serif'>second page</body></html>";

static const char popup_page_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>link-test popup</title>"
    "</head><body style='background:#fff;font:22px sans-serif'>popup page</body></html>";

/* ---- state ------------------------------------------------------------------------------------------------- */

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_NA } Result;

typedef struct {
    double x, y, w, h;
    gboolean reported;  /* the page reported the rect */
    gboolean known;     /* and it has an area */
} Rect;

typedef struct {
    GMainLoop *loop;
    int width, height;
    double scale;
    int exit_code;
    gboolean finished;
    guint16 port;
    GSocketService *service;
    GArray *plan_variants;      /* guint indices into variants[] */
    GArray *plan_elements;      /* guint indices into elements[] */
    guint vi, ei;               /* positions in the plan */
    const Variant *variant;
    const Element *element;
    uc_webview *wv;
    gboolean wv_used;           /* the view loaded a page already */
    void *held;                 /* the frame shown, released when the next one comes */
    Result verdict;             /* decided, printed a moment later with what the page logged by then */
    char *verdict_why;
    guint generation;           /* bumped per element: timeouts of an earlier element do nothing */
    char *key;                  /* the page URL's query, "?e=link&v=still" */
    gboolean ready;             /* the page reported its rects */
    gboolean started;           /* the element's first touch went out */
    guint ready_source;         /* the tap 300 ms after the page's last rects report */
    Rect target, alert;
    char *fonts;                /* the page's font report, "sans-serif=23/8,serif=23/8,monospace=25/8" */
    /* the loop variant's prelude */
    gboolean in_prelude, prelude_done;
    int prelude_dialogs;
    /* the dialog step */
    gboolean alert_done;        /* the page logged alert:done (alert() returned) */
    gboolean dialog_seen;       /* the script_dialog callback came (0.7.0 with the request callbacks) */
    gboolean focus_cycle_done;  /* the view's focus went out and back in around the dialog */
    gboolean tapping;           /* the element's tap is under way or done, no verdict yet */
    /* what the element's tap did */
    char *hash;
    gboolean checked, closed;
    char *page_log;
    gboolean committed;         /* the element's path committed */
    gboolean server_hit;        /* the server got the element's path */
    gboolean new_window, close_requested;
    char *new_window_uri;
    int new_window_result;
    char *other_load;           /* another document that committed after the tap */
    /* touch */
    double tx, ty;
    int touch_step;
    gboolean touch_moves;
    GSourceFunc touch_done;
    /* totals */
    int pass, fail, na;         /* whole run */
    int vpass, vfail, vna;      /* this variant */
} LinkTest;

static LinkTest lt;

gboolean
link_test_mode (void)
{
    return opt_link_test;
}

/* names of a comma separated list -> indices into a table of n entries whose names name_of() gives; NULL list: all */
static GArray *
parse_list (const char *list, guint n, const char *(*name_of) (guint), const char *option)
{
    GArray *indices = g_array_new (FALSE, FALSE, sizeof (guint));
    char **names;

    if (list == NULL) {
        for (guint i = 0; i < n; i++)
            g_array_append_val (indices, i);
        return indices;
    }
    names = g_strsplit (list, ",", -1);
    for (char **name = names; *name != NULL; name++) {
        guint i;
        g_strstrip (*name);
        for (i = 0; i < n && g_strcmp0 (name_of (i), *name) != 0; i++)
            ;
        if (i == n) {
            g_printerr ("ucwebview-test: unknown %s \"%s\"\n", option, *name);
            g_array_unref (indices);
            g_strfreev (names);
            return NULL;
        }
        g_array_append_val (indices, i);
    }
    g_strfreev (names);
    if (indices->len == 0) {
        g_printerr ("ucwebview-test: empty %s list\n", option);
        g_array_unref (indices);
        return NULL;
    }
    return indices;
}

static const char *
variant_name (guint i)
{
    return variants[i].name;
}

static const char *
element_name (guint i)
{
    return elements[i].id;
}

gboolean
link_test_check_options (void)
{
    if (!opt_link_test) {
        if (opt_link_variants != NULL || opt_link_elements != NULL) {
            g_printerr ("ucwebview-test: --link-variants and --link-elements go with --link-test\n");
            return FALSE;
        }
        return TRUE;
    }
    lt.plan_variants = parse_list (opt_link_variants, N_VARIANTS, variant_name, "--link-variants variant");
    lt.plan_elements = parse_list (opt_link_elements, N_ELEMENTS, element_name, "--link-elements element");
    return lt.plan_variants != NULL && lt.plan_elements != NULL;
}

int
link_test_default_timeout (void)
{
    guint nv = lt.plan_variants != NULL ? lt.plan_variants->len : N_VARIANTS;
    guint ne = lt.plan_elements != NULL ? lt.plan_elements->len : N_ELEMENTS;
    /* a load, a dialog of about a second and the 2.5 s wait for an effect that does not come, per element */
    return 30 + (int) (nv * ne) * 8;
}

static void
finish (int code)
{
    if (lt.finished)
        return;
    lt.finished = TRUE;
    lt.exit_code = code;
    g_main_loop_quit (lt.loop);
}

/* ---- the server ---------------------------------------------------------------------------------------------- */

/* main thread: the server got a request */
static gboolean
server_report (gpointer user_data)
{
    char *path = user_data;
    if (lt.element != NULL && lt.element->path != NULL && g_str_has_prefix (path, lt.element->path))
        lt.server_hit = TRUE;
    if (!g_str_has_prefix (path, "/?") && g_strcmp0 (path, "/") != 0)
        g_print ("link-test: server: GET %s\n", path);
    g_free (path);
    return G_SOURCE_REMOVE;
}

/* a thread of the service: one request per connection ("Connection: close") */
static gboolean
server_run (GThreadedSocketService *service, GSocketConnection *connection, GObject *source, gpointer user_data)
{
    GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    char request[4096];
    gsize length = 0;
    const char *body = NULL, *status = "404 Not Found";
    char *path = NULL, *header;

    (void) service; (void) source; (void) user_data;
    g_socket_set_timeout (g_socket_connection_get_socket (connection), 5);
    while (length < sizeof request - 1) {
        gssize n = g_input_stream_read (in, request + length, sizeof request - 1 - length, NULL, NULL);
        if (n <= 0)
            break;
        length += (gsize) n;
        request[length] = '\0';
        if (strstr (request, "\r\n\r\n") != NULL)
            break;
    }
    request[length] = '\0';
    if (g_str_has_prefix (request, "GET ")) {
        const char *start = request + 4, *end = strchr (start, ' ');
        if (end != NULL)
            path = g_strndup (start, (gsize) (end - start));
    }
    if (path != NULL) {
        if (g_strcmp0 (path, "/") == 0 || g_str_has_prefix (path, "/?"))
            body = link_page_html;
        else if (g_str_has_prefix (path, "/second.html"))
            body = second_page_html;
        else if (g_str_has_prefix (path, "/popup.html"))
            body = popup_page_html;
        if (body != NULL)
            status = "200 OK";
    }
    if (body == NULL)
        body = "<!doctype html><title>not found</title>";
    header = g_strdup_printf ("HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %"
                              G_GSIZE_FORMAT "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                              status, strlen (body));
    if (g_output_stream_write_all (out, header, strlen (header), NULL, NULL, NULL))
        g_output_stream_write_all (out, body, strlen (body), NULL, NULL, NULL);
    g_free (header);
    g_io_stream_close (G_IO_STREAM (connection), NULL, NULL);
    if (path != NULL)
        g_idle_add (server_report, path);   /* thread-safe: the default main context */
    return TRUE;
}

static gboolean
server_start (void)
{
    GError *error = NULL;
    GInetAddress *loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new (loopback, 0);
    GSocketAddress *effective = NULL;
    gboolean ok;

    lt.service = g_threaded_socket_service_new (4);
    ok = g_socket_listener_add_address (G_SOCKET_LISTENER (lt.service), address, G_SOCKET_TYPE_STREAM,
                                        G_SOCKET_PROTOCOL_TCP, NULL, &effective, &error);
    g_object_unref (address);
    g_object_unref (loopback);
    if (!ok) {
        g_printerr ("link-test: cannot listen on 127.0.0.1: %s\n", error->message);
        g_clear_error (&error);
        return FALSE;
    }
    lt.port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
    g_object_unref (effective);
    g_signal_connect (lt.service, "run", G_CALLBACK (server_run), NULL);
    g_socket_service_start (lt.service);
    return TRUE;
}

/* ---- touches ----------------------------------------------------------------------------------------------- */

#define TOUCH_STEP_MS  16   /* a panel's report interval */
#define STILL_UP_MS    80
#define MOVE_STEPS     4

/* the offset of a sliding tap after step k of MOVE_STEPS: 3/4 of the drag threshold in the end, mostly sideways like
 * the recorded taps (12.4 px at 16 px; WebKit compares rounded positions with the threshold, WPEGestureDetector.cpp:45,
 * 63-69) */
static void
move_offset (int k, double *dx, double *dy)
{
    double threshold = uc_webview_global_get_drag_threshold ();
    *dx = 0.72 * threshold * k / MOVE_STEPS;
    *dy = 0.18 * threshold * k / MOVE_STEPS;
}

static gboolean
touch_tick (gpointer user_data)
{
    guint generation = GPOINTER_TO_UINT (user_data);
    int step;
    double dx = 0, dy = 0;

    if (generation != lt.generation || lt.wv == NULL)
        return G_SOURCE_REMOVE;
    step = lt.touch_step++;
    if (lt.touch_moves && step < MOVE_STEPS) {
        move_offset (step + 1, &dx, &dy);
        uc_webview_touch (lt.wv, 0, UC_WEBVIEW_TOUCH_MOVE, lt.tx + dx, lt.ty + dy, 0);
        return G_SOURCE_CONTINUE;
    }
    if (lt.touch_moves)
        move_offset (MOVE_STEPS, &dx, &dy);
    uc_webview_touch (lt.wv, 0, UC_WEBVIEW_TOUCH_UP, lt.tx + dx, lt.ty + dy, 0);
    if (lt.touch_done != NULL)
        g_idle_add (lt.touch_done, GUINT_TO_POINTER (lt.generation));
    return G_SOURCE_REMOVE;
}

/* slot 0 like remote-ui's first finger (libinput-like slot ids); done() (may be NULL) runs after the release */
static void
tap (const Rect *rect, gboolean moves, GSourceFunc done)
{
    lt.tx = rect->x + rect->w / 2;
    lt.ty = rect->y + rect->h / 2;
    lt.touch_step = 0;
    lt.touch_moves = moves;
    lt.touch_done = done;
    uc_webview_touch (lt.wv, 0, UC_WEBVIEW_TOUCH_DOWN, lt.tx, lt.ty, 0);
    g_timeout_add (moves ? TOUCH_STEP_MS : STILL_UP_MS, touch_tick, GUINT_TO_POINTER (lt.generation));
}

/* ---- results ----------------------------------------------------------------------------------------------- */

static void start_element (void);
static gboolean create_view (void);

/* the page's log entries about the tapped element ("click:link:trusted:active:mouse"), or what it logged at all */
static char *
clicks_seen (void)
{
    GString *seen = g_string_new (NULL);
    char **entries = g_strsplit (lt.page_log != NULL ? lt.page_log : "", "|", -1);

    for (char **entry = entries; *entry != NULL; entry++) {
        if (**entry == '\0' || g_str_has_prefix (*entry, "alert:"))
            continue;
        if (g_str_has_prefix (*entry, "click:alert:") || g_str_has_prefix (*entry, "click:alerts:"))
            continue;   /* the dialog step's and the prelude's own taps */
        g_string_append_printf (seen, "%s%s", seen->len > 0 ? ", " : "", *entry);
    }
    g_strfreev (entries);
    if (seen->len == 0)
        g_string_append (seen, "nothing (no click reached the page)");
    return g_string_free (seen, FALSE);
}

static gboolean
page_saw_click (const char *id)
{
    char *needle = g_strdup_printf ("click:%s:", id);
    gboolean seen = lt.page_log != NULL && strstr (lt.page_log, needle) != NULL;
    g_free (needle);
    return seen;
}

static gboolean
element_report (gpointer user_data)
{
    static const char *const names[] = { "PASS", "FAIL", "not checkable" };
    char *seen = clicks_seen ();

    (void) user_data;
    if (lt.element->check == CHECK_FONT)    /* no tap: nothing to log */
        g_print ("link-test: %s/%s (%s): %s: %s\n", lt.variant->name, lt.element->id, lt.element->what,
                 names[lt.verdict], lt.verdict_why);
    else
        g_print ("link-test: %s/%s (%s): %s: %s; the page logged %s%s\n", lt.variant->name, lt.element->id,
                 lt.element->what, names[lt.verdict], lt.verdict_why, seen,
                 lt.server_hit && lt.verdict != RESULT_PASS ? "; the server got the request" : "");
    g_free (seen);
    switch (lt.verdict) {
    case RESULT_PASS: lt.pass++; lt.vpass++; break;
    case RESULT_FAIL: lt.fail++; lt.vfail++; break;
    case RESULT_NA: lt.na++; lt.vna++; break;
    }
    lt.ei++;
    start_element ();
    return G_SOURCE_REMOVE;
}

/* the verdict; the report waits 250 ms for the page's log of the tap (its title comes after a host callback) */
static void
element_done (Result result, const char *why)
{
    lt.verdict = result;
    g_free (lt.verdict_why);
    lt.verdict_why = g_strdup (why);
    lt.generation++;    /* the element's pending timeouts end */
    lt.tapping = FALSE; /* no second verdict from events that still come */
    g_timeout_add (250, element_report, NULL);
}

/* called on every change; decides when the effect is there */
static void
evaluate (void)
{
    if (!lt.tapping || lt.finished)
        return;
    switch (lt.element->check) {
    case CHECK_FONT:    /* decided in page_ready(), never tapped */
        break;
    case CHECK_HASH:
        if (g_strcmp0 (lt.hash, "#target") == 0)
            element_done (RESULT_PASS, "location.hash is #target");
        break;
    case CHECK_LOAD:
        if (lt.committed)
            element_done (RESULT_PASS, "the linked page committed");
        break;
    case CHECK_CHECKED:
        if (lt.checked)
            element_done (RESULT_PASS, "the checkbox is checked");
        break;
    case CHECK_WINDOW:
        if (lt.page_log != NULL && strstr (lt.page_log, "confirmopen:confirm=false") != NULL) {
            /* no script_dialog callback (library 0.6, still-norequests): WebKit's default answer is "no" */
            element_done (RESULT_NA,
                          "confirm() returned false: without a script_dialog callback WebKit answers \"no\"");
            break;
        }
        if (lt.new_window) {
            char *why = g_strdup_printf ("new_window(%s, %s)", lt.new_window_uri != NULL ? lt.new_window_uri : "?",
                                         lt.new_window_result == 0 ? "loaded" : "refused");
            element_done (RESULT_PASS, why);
            g_free (why);
        } else if (lt.committed) {
            element_done (RESULT_PASS, "the new window's page committed in this view");
        }
        break;
    case CHECK_CLOSED:
        if (lt.close_requested)
            element_done (RESULT_PASS, "close_requested()");
        else if (lt.closed)
            element_done (RESULT_PASS, "window.closed is true (WebKit sends the close request to the UI process)");
        break;
    }
}

/* 2.5 s after the release: what did not come */
static gboolean
effect_timeout (gpointer user_data)
{
    char *why;

    if (GPOINTER_TO_UINT (user_data) != lt.generation || lt.finished)
        return G_SOURCE_REMOVE;
    switch (lt.element->check) {
    case CHECK_HASH:
        why = g_strdup_printf ("location.hash is %s, not #target", lt.hash != NULL ? lt.hash : "unchanged");
        break;
    case CHECK_LOAD:
        why = g_strdup_printf ("%s did not commit in 2.5 s%s%s", lt.element->path,
                               lt.other_load != NULL ? ", committed instead: " : "",
                               lt.other_load != NULL ? lt.other_load : "");
        break;
    case CHECK_CHECKED:
        why = g_strdup ("the checkbox stayed unchecked");
        break;
    case CHECK_WINDOW:
#if LINK_TEST_REQUESTS
        why = g_strdup_printf ("no new_window() and no load of the new window's page in 2.5 s (%s)",
                               page_saw_click (lt.element->rect) ? "the click reached the page"
                                                                 : "the page saw no click on it");
#else
        /* library 0.6: WebKit's create signal has no handler, a new window is refused without a trace */
        if (page_saw_click (lt.element->rect)) {
            element_done (RESULT_NA, "library 0.6 opens no windows and reports none (0.7.0: new_window callback)");
            return G_SOURCE_REMOVE;
        }
        why = g_strdup ("the page saw no click on it");
#endif
        break;
    case CHECK_CLOSED:
        why = g_strdup ("window.closed stayed false and no close request came");
        break;
    case CHECK_FONT:
    default:
        why = g_strdup ("no verdict");  /* not reached: the font check is decided before any tap */
        break;
    }
    element_done (RESULT_FAIL, why);
    g_free (why);
    return G_SOURCE_REMOVE;
}

static gboolean
element_tapped (gpointer user_data)
{
    if (GPOINTER_TO_UINT (user_data) != lt.generation)
        return G_SOURCE_REMOVE;
    g_timeout_add (2500, effect_timeout, GUINT_TO_POINTER (lt.generation));
    evaluate ();
    return G_SOURCE_REMOVE;
}

static gboolean
tap_element (gpointer user_data)
{
    if (GPOINTER_TO_UINT (user_data) != lt.generation)
        return G_SOURCE_REMOVE;
    if (!lt.target.known) {
        if (lt.target.reported) {
            /* an inline element of a font without metrics: nothing to tap (the font element says why) */
            char *why = g_strdup_printf ("its rect is %.0fx%.0f px at %.0f,%.0f, nothing to tap: inline text has no "
                                         "height when the page's font has no ascent and descent (see the font element)",
                                         lt.target.w, lt.target.h, lt.target.x, lt.target.y);
            element_done (RESULT_FAIL, why);
            g_free (why);
        } else {
            element_done (RESULT_FAIL, "the page reported no rect for it");
        }
        return G_SOURCE_REMOVE;
    }
    lt.tapping = TRUE;
    g_print ("link-test: %s/%s: %s tap at %.0f,%.0f\n", lt.variant->name, lt.element->id,
             lt.variant->moves ? "sliding" : "still", lt.target.x + lt.target.w / 2, lt.target.y + lt.target.h / 2);
    tap (&lt.target, lt.variant->moves, element_tapped);
    return G_SOURCE_REMOVE;
}

/* ---- the dialog step ---------------------------------------------------------------------------------------- */

static gboolean
dialog_settled (gpointer user_data)
{
    if (GPOINTER_TO_UINT (user_data) != lt.generation)
        return G_SOURCE_REMOVE;
    /* like the user's next tap after the dialog went away */
    g_timeout_add (300, tap_element, GUINT_TO_POINTER (lt.generation));
    return G_SOURCE_REMOVE;
}

static gboolean
focus_in (gpointer user_data)
{
    /* remote-ui: the dialog closed, the web view item has the active focus again (also when the element's verdict
     * came first: the next element starts with a focused view) */
    if (lt.wv != NULL)
        uc_webview_set_focused (lt.wv, 1);
    if (GPOINTER_TO_UINT (user_data) != lt.generation)
        return G_SOURCE_REMOVE;
    lt.focus_cycle_done = TRUE;
    if (lt.alert_done && !lt.tapping)   /* the dialog step's end; not the confirm() of the confirmopen element */
        dialog_settled (GUINT_TO_POINTER (lt.generation));
    return G_SOURCE_REMOVE;
}

#if LINK_TEST_REQUESTS
static guint64 pending_dialog;

static gboolean
answer_dialog (gpointer user_data)
{
    /* answered whatever happened meanwhile: the page waits for it (-1 when the request ended already) */
    (void) user_data;
    if (lt.wv == NULL)
        return G_SOURCE_REMOVE;
    if (uc_webview_request_reply (lt.wv, pending_dialog, 1, NULL) != 0)
        g_print ("link-test: uc_webview_request_reply(%" G_GUINT64_FORMAT ") refused\n", (guint64) pending_dialog);
    g_timeout_add (300, focus_in, GUINT_TO_POINTER (lt.generation));
    return G_SOURCE_REMOVE;
}

static void
on_script_dialog (uc_webview *wv, uint64_t request, int type, const char *origin, const char *message,
                  const char *default_text, void *user)
{
    (void) origin; (void) message; (void) default_text; (void) user;
    if (lt.in_prelude) {
        /* the "6 alerts" loop: every dialog that reaches the host is answered like remote-ui's user does */
        lt.prelude_dialogs++;
        pending_dialog = request;
        uc_webview_set_focused (wv, 0);
        g_timeout_add (300, answer_dialog, GUINT_TO_POINTER (lt.generation));
        return;
    }
    if (lt.tapping && lt.element->check == CHECK_WINDOW && g_strcmp0 (lt.element->id, "confirmopen") == 0) {
        /* the element's own confirm(): answered "yes" like remote-ui does, focus out, the answer 300 ms later,
         * focus in */
        g_print ("link-test: %s/%s: script dialog (type %d), answered in 300 ms\n", lt.variant->name, lt.element->id,
                 type);
        pending_dialog = request;
        uc_webview_set_focused (wv, 0);
        g_timeout_add (300, answer_dialog, GUINT_TO_POINTER (lt.generation));
        return;
    }
    if (!lt.variant->dialog || lt.tapping) {
        /* nothing on the page asks outside the dialog step; answer like a host without a dialog */
        uc_webview_request_reply (wv, request, 0, NULL);
        return;
    }
    g_print ("link-test: %s/%s: script dialog (type %d), answered in 300 ms\n", lt.variant->name, lt.element->id, type);
    lt.dialog_seen = TRUE;
    pending_dialog = request;
    /* remote-ui: the dialog popup takes the active focus from the web view item (uc_webview_set_focused(0)) */
    uc_webview_set_focused (wv, 0);
    g_timeout_add (300, answer_dialog, GUINT_TO_POINTER (lt.generation));
}

static void
on_authenticate (uc_webview *wv, uint64_t request, const char *origin, const char *realm, int scheme, unsigned flags,
                 void *user)
{
    (void) origin; (void) realm; (void) scheme; (void) flags; (void) user;
    uc_webview_request_reply (wv, request, 0, NULL);
}

static void
on_request_cancelled (uc_webview *wv, uint64_t request, void *user)
{
    (void) wv; (void) user;
    g_print ("link-test: request %" G_GUINT64_FORMAT " cancelled\n", (guint64) request);
}

static void
on_close_requested (uc_webview *wv, void *user)
{
    (void) wv; (void) user;
    lt.close_requested = TRUE;
    evaluate ();
}

static void
on_new_window (uc_webview *wv, const char *uri, int result, void *user)
{
    (void) wv; (void) user;
    lt.new_window = TRUE;
    g_free (lt.new_window_uri);
    lt.new_window_uri = g_strdup (uri);
    lt.new_window_result = result;
    evaluate ();
}

static void
on_response_refused (uc_webview *wv, const char *uri, const char *mime_type, int reason, void *user)
{
    (void) wv; (void) user;
    g_print ("link-test: response refused (%d): %s (%s)\n", reason, uri != NULL ? uri : "?",
             mime_type != NULL ? mime_type : "?");
}
#endif

/* the page logged alert:done: alert() returned */
static void
alert_returned (void)
{
    if (lt.dialog_seen) {
        /* 0.7.0: the host answered; its focus comes back 300 ms after the answer (focus_in) */
        if (lt.focus_cycle_done)
            dialog_settled (GUINT_TO_POINTER (lt.generation));
        return;
    }
    /* no script_dialog callback: WebKit closed the alert at once (WebKitWebViewWPE.cpp:39-46 returns FALSE, the
     * signal's last unref closes it); go through remote-ui's focus out and in all the same */
    uc_webview_set_focused (lt.wv, 0);
    g_timeout_add (300, focus_in, GUINT_TO_POINTER (lt.generation));
}

static gboolean
dialog_timeout (gpointer user_data)
{
    if (GPOINTER_TO_UINT (user_data) != lt.generation || lt.tapping)
        return G_SOURCE_REMOVE;
    element_done (RESULT_FAIL, "the alert button's tap brought no alert in 5 s (the dialog step failed)");
    return G_SOURCE_REMOVE;
}

/* ---- page events --------------------------------------------------------------------------------------------- */

/* after the variant's preparation: the dialog step or the tap */
static gboolean
page_go (gpointer user_data)
{
    if (GPOINTER_TO_UINT (user_data) != lt.generation)
        return G_SOURCE_REMOVE;
    if (lt.variant->dialog) {
        if (!lt.alert.known) {
            element_done (RESULT_FAIL, "the page reported no rect for the alert button");
            return G_SOURCE_REMOVE;
        }
        tap (&lt.alert, lt.variant->moves, NULL);   /* the page's alert:done goes on (alert_returned) */
        g_timeout_add (5000, dialog_timeout, GUINT_TO_POINTER (lt.generation));
        return G_SOURCE_REMOVE;
    }
    tap_element (GUINT_TO_POINTER (lt.generation));
    return G_SOURCE_REMOVE;
}

#define XKB_KEY_DOWN    0xff54
#define DPAD_DOWN_CODE  116     /* the scan code remote-ui forwards for the remote's DOWN (its input trace) */

static gboolean
prelude_timeout (gpointer user_data)
{
    if (GPOINTER_TO_UINT (user_data) != lt.generation || !lt.in_prelude)
        return G_SOURCE_REMOVE;
    g_print ("link-test: %s: the \"6 alerts\" loop did not end in 15 s (%d dialog(s) reached the host)\n",
             lt.variant->name, lt.prelude_dialogs);
    lt.fail++;
    lt.vfail++;
    lt.in_prelude = FALSE;
    lt.prelude_done = TRUE;
    lt.generation++;
    start_element ();
    return G_SOURCE_REMOVE;
}

static gboolean
prelude_next (gpointer user_data)
{
    (void) user_data;
    start_element ();
    return G_SOURCE_REMOVE;
}

/* the page logged alerts:done: the loop returned */
static void
prelude_finished (void)
{
    g_print ("link-test: %s: the \"6 alerts\" loop returned, %d of its dialogs reached the host%s\n", lt.variant->name,
             lt.prelude_dialogs,
             LINK_TEST_REQUESTS ? " (the library's dialog loop guard answers the others)" : " (WebKit closed them)");
    lt.in_prelude = FALSE;
    lt.prelude_done = TRUE;
    lt.generation++;
    /* like a user's next tap a few seconds later; the last answer's focus_in gives the view its focus back */
    g_timeout_add (1000, prelude_next, NULL);
}

/* the font element: the page's report "sans-serif=23/8,serif=23/8,monospace=25/8" (ascent/descent at 22 px) */
static void
font_verdict (void)
{
    GString *why = g_string_new (NULL);
    char **families;
    gboolean ok = TRUE;

    if (lt.fonts == NULL) {
        element_done (RESULT_FAIL, "the page reported no font metrics (canvas measureText failed)");
        g_string_free (why, TRUE);
        return;
    }
    families = g_strsplit (lt.fonts, ",", -1);
    for (char **family = families; *family != NULL; family++) {
        char *eq = strchr (*family, '=');
        int ascent = -1, descent = -1;
        if (eq == NULL || sscanf (eq + 1, "%d/%d", &ascent, &descent) != 2) {
            ok = FALSE;
            continue;
        }
        *eq = '\0';
        g_string_append_printf (why, "%s%s %d/%d", why->len > 0 ? ", " : "", *family, ascent, descent);
        if (ascent <= 0 || descent < 0)
            ok = FALSE;
    }
    g_strfreev (families);
    if (ok) {
        g_string_append (why, " px ascent/descent at 22 px");
    } else {
        /* the device's state before 0.6.3: fc-match names Poppins for every family, but no installed font carries a
         * family name of the pattern, so WebKit's Skia font cache ends with an empty typeface */
        g_string_append (why, " px ascent/descent at 22 px: a family without ascent has no installed font with a "
                              "family name in its fontconfig pattern, WebKit's primary font is an empty typeface "
                              "(FontCacheSkia.cpp:180-200) and inline text has no height; the uc-webview package's "
                              "/etc/fonts/conf.d/56-uc-webview-fonts.conf aliases the generic families to the "
                              "installed fonts");
    }
    element_done (ok ? RESULT_PASS : RESULT_FAIL, why->str);
    g_string_free (why, TRUE);
}

static gboolean
page_ready (gpointer user_data)
{
    lt.ready_source = 0;
    if (GPOINTER_TO_UINT (user_data) != lt.generation || lt.started)
        return G_SOURCE_REMOVE;
    lt.started = TRUE;
    if (!lt.in_prelude && lt.element->check == CHECK_FONT) {
        font_verdict ();
        return G_SOURCE_REMOVE;
    }
    if (lt.in_prelude) {
        if (!lt.target.known) {
            g_print ("link-test: %s: the page reported no rect for the \"6 alerts\" button\n", lt.variant->name);
            return prelude_timeout (user_data);
        }
        tap (&lt.target, lt.variant->moves, NULL);
        g_timeout_add (15000, prelude_timeout, user_data);
        return G_SOURCE_REMOVE;
    }
    if (!lt.variant->refocus && !lt.variant->keys)
        return page_go (user_data);
    if (lt.variant->refocus)
        uc_webview_set_focused (lt.wv, 1);
    if (lt.variant->keys) {
        uc_webview_key (lt.wv, XKB_KEY_DOWN, DPAD_DOWN_CODE, 1, 0, 0);
        uc_webview_key (lt.wv, XKB_KEY_DOWN, DPAD_DOWN_CODE, 0, 0, 0);
    }
    g_timeout_add (300, page_go, user_data);
    return G_SOURCE_REMOVE;
}

static gboolean
parse_rect (const char *token, const char *id, Rect *rect)
{
    size_t n;
    if (id == NULL)     /* the font element taps nothing */
        return FALSE;
    n = strlen (id);
    if (strncmp (token, id, n) != 0 || token[n] != ':')
        return FALSE;
    rect->reported = sscanf (token + n + 1, "%lf,%lf,%lf,%lf", &rect->x, &rect->y, &rect->w, &rect->h) == 4;
    rect->known = rect->reported && rect->w > 0 && rect->h > 0;
    return TRUE;
}

/* "ucready ?e=link&v=still 480x800 fonts:sans-serif=23/8,serif=23/8,monospace=25/8 frag:8,20,102,25 link:..." */
static void
on_ready_title (const char *title)
{
    char **tokens = g_strsplit (title, " ", -1);

    if (tokens[0] == NULL || tokens[1] == NULL || g_strcmp0 (tokens[1], lt.key) != 0 || lt.started) {
        g_strfreev (tokens);
        return;
    }
    lt.ready = TRUE;
    for (char **token = tokens + 2; *token != NULL; token++) {
        if (g_str_has_prefix (*token, "fonts:")) {
            g_free (lt.fonts);
            lt.fonts = g_strdup (*token + 6);
            continue;
        }
        parse_rect (*token, lt.element->rect, &lt.target);
        parse_rect (*token, "alert", &lt.alert);
    }
    g_strfreev (tokens);
    /* the page settles (frames, a late resize reports again) before the first touch */
    if (lt.ready_source != 0)
        g_source_remove (lt.ready_source);
    lt.ready_source = g_timeout_add (300, page_ready, GUINT_TO_POINTER (lt.generation));
}

/* "uclink:<n> hash=#target checked=0 closed=0 log=click:frag:trusted:active:mouse|hashchange" */
static void
on_state_title (const char *title)
{
    const char *hash = strstr (title, " hash="), *checked = strstr (title, " checked="),
               *closed = strstr (title, " closed="), *log = strstr (title, " log=");

    if (hash == NULL || checked == NULL || closed == NULL || log == NULL)
        return;
    g_free (lt.hash);
    lt.hash = g_strndup (hash + 6, strcspn (hash + 6, " "));
    if (g_strcmp0 (lt.hash, "-") == 0)
        g_clear_pointer (&lt.hash, g_free);
    lt.checked = checked[9] == '1';
    lt.closed = closed[8] == '1';
    g_free (lt.page_log);
    lt.page_log = g_strdup (log + 5);
    if (lt.in_prelude && strstr (lt.page_log, "alerts:done") != NULL) {
        prelude_finished ();
        return;
    }
    if (lt.variant->dialog && !lt.alert_done && strstr (lt.page_log, "alert:done") != NULL) {
        lt.alert_done = TRUE;
        alert_returned ();
    }
    evaluate ();
}

static void
on_title (uc_webview *wv, const char *title, void *user)
{
    (void) wv; (void) user;
    if (title == NULL || lt.element == NULL)
        return;
    if (g_str_has_prefix (title, "ucready "))
        on_ready_title (title);
    else if (g_str_has_prefix (title, "uclink:"))
        on_state_title (title);
}

static void
on_load (uc_webview *wv, int event, const char *uri, void *user)
{
    (void) wv; (void) user;
    if (lt.element == NULL || uri == NULL)
        return;
    if (event == UC_WEBVIEW_LOAD_FAILED && lt.tapping) {
        g_print ("link-test: %s/%s: load FAILED: %s\n", lt.variant->name, lt.element->id, uri);
        return;
    }
    if (event != UC_WEBVIEW_LOAD_COMMITTED || !lt.tapping)
        return;
    if (lt.element->path != NULL && strstr (uri, lt.element->path) != NULL) {
        lt.committed = TRUE;
        evaluate ();
    } else if (strstr (uri, lt.key) == NULL) {
        g_free (lt.other_load);
        lt.other_load = g_strdup (uri);
    }
}

static void
on_frame (uc_webview *wv, const uc_webview_frame *frame, void *user)
{
    (void) user;
    /* shown at once, the previous one released, like a scene graph; nothing is read back */
    uc_webview_frame_rendered (wv, frame->handle);
    if (lt.held != NULL)
        uc_webview_frame_release (wv, lt.held);
    lt.held = frame->handle;
}

static void
on_terminated (uc_webview *wv, int reason, void *user)
{
    (void) wv; (void) user;
    g_printerr ("link-test: web process terminated (reason %d)\n", reason);
    lt.fail++;
    finish (1);
}

static void
on_pinch (uc_webview *wv, int phase, double scale, double x, double y, void *user)
{
    (void) wv; (void) user;
    /* one finger never pinches; a report means the tracker took a tap for a second finger */
    g_print ("link-test: unexpected pinch report (phase %d, scale %.3f at %.0f,%.0f)\n", phase, scale, x, y);
}

/* ---- steps -------------------------------------------------------------------------------------------------- */

static void start_variant (void);

static gboolean
next_variant (gpointer user_data)
{
    (void) user_data;
    start_variant ();
    return G_SOURCE_REMOVE;
}

static gboolean
page_timeout (gpointer user_data)
{
    if (GPOINTER_TO_UINT (user_data) != lt.generation || lt.ready || lt.finished)
        return G_SOURCE_REMOVE;
    element_done (RESULT_FAIL, "the test page did not report ready in 10 s");
    return G_SOURCE_REMOVE;
}

static void
reset_element_state (void)
{
    g_clear_pointer (&lt.key, g_free);
    g_clear_pointer (&lt.hash, g_free);
    g_clear_pointer (&lt.page_log, g_free);
    g_clear_pointer (&lt.new_window_uri, g_free);
    g_clear_pointer (&lt.other_load, g_free);
    g_clear_pointer (&lt.fonts, g_free);
    if (lt.ready_source != 0)
        g_source_remove (lt.ready_source);
    lt.ready_source = 0;
    lt.ready = lt.started = lt.alert_done = lt.dialog_seen = lt.focus_cycle_done = lt.tapping = FALSE;
    lt.checked = lt.closed = lt.committed = lt.server_hit = lt.new_window = lt.close_requested = FALSE;
    lt.new_window_result = -1;
    memset (&lt.target, 0, sizeof lt.target);
    memset (&lt.alert, 0, sizeof lt.alert);
}

static void
free_view (void)
{
    if (lt.wv == NULL)
        return;
    if (lt.held != NULL)
        uc_webview_frame_release (lt.wv, lt.held);
    lt.held = NULL;
    uc_webview_free (lt.wv);
    lt.wv = NULL;
}

static void
start_element (void)
{
    char *uri;

    if (lt.finished)
        return;
    reset_element_state ();
    if (lt.ei >= lt.plan_elements->len) {
        g_print ("link-test: %s: %d passed, %d failed, %d not checkable\n", lt.variant->name, lt.vpass, lt.vfail,
                 lt.vna);
        free_view ();
        lt.element = NULL;
        lt.vi++;
        g_idle_add (next_variant, NULL);
        return;
    }
    lt.element = &elements[g_array_index (lt.plan_elements, guint, lt.ei)];
    if (lt.variant->loop && !lt.prelude_done) {
        /* once per variant, before the first element: the "6 alerts" button */
        lt.in_prelude = TRUE;
        lt.prelude_dialogs = 0;
        lt.element = &prelude_element;
    }
    if (lt.element->fresh && lt.wv_used) {
        free_view ();
        if (!create_view ())
            return;
    }
    lt.wv_used = TRUE;
    lt.generation++;
    /* every element on a fresh document of the same view: the variant's web process keeps its history */
    lt.key = g_strdup_printf ("?e=%s&v=%s", lt.in_prelude ? "prelude" : lt.element->id, lt.variant->name);
    if (lt.variant->refocus || lt.variant->unfocused)
        uc_webview_set_focused (lt.wv, 0);
    uri = g_strdup_printf ("http://127.0.0.1:%u/%s", lt.port, lt.key);
    uc_webview_load_uri (lt.wv, uri);
    g_free (uri);
    g_timeout_add (10000, page_timeout, GUINT_TO_POINTER (lt.generation));
}

/* a view like remote-ui's for the variant */
static gboolean
create_view (void)
{
    uc_webview_callbacks cb;

    memset (&cb, 0, sizeof cb);
    cb.frame_ready = on_frame;
    cb.load_changed = on_load;
    cb.title_changed = on_title;
    cb.web_process_terminated = on_terminated;
    /* the switch of library 0.6.0: the library's gesture controller only for a host that takes pinch reports */
    if (lt.variant->pinch)
        cb.pinch = on_pinch;
#if LINK_TEST_REQUESTS
    if (lt.variant->requests) {
        cb.script_dialog = on_script_dialog;
        cb.authenticate = on_authenticate;
        cb.request_cancelled = on_request_cancelled;
        cb.close_requested = on_close_requested;
        cb.new_window = on_new_window;
        cb.response_refused = on_response_refused;
    }
#endif
    lt.wv = uc_webview_new_sized (lt.width, lt.height, &cb, sizeof cb, &lt);
    if (lt.wv == NULL) {
        g_printerr ("link-test: uc_webview_new_sized failed\n");
        finish (2);
        return FALSE;
    }
    lt.wv_used = FALSE;
    /* what remote-ui does for a new view (webviewitem.cpp attachWindow) */
    uc_webview_set_scale (lt.wv, lt.scale);
    uc_webview_set_mapped (lt.wv, 1);
    uc_webview_set_focused (lt.wv, 1);
    uc_webview_set_background_rgba (lt.wv, 1, 1, 1, 1);
    return TRUE;
}

static void
start_variant (void)
{
    if (lt.finished)
        return;
    if (lt.vi >= lt.plan_variants->len) {
        g_print ("link-test: %d passed, %d failed, %d not checkable: %s\n", lt.pass, lt.fail, lt.na,
                 lt.fail == 0 ? "ok" : "FAILED");
        finish (lt.fail == 0 ? 0 : 1);
        return;
    }
    lt.variant = &variants[g_array_index (lt.plan_variants, guint, lt.vi)];
    lt.ei = 0;
    lt.vpass = lt.vfail = lt.vna = 0;
    lt.in_prelude = lt.prelude_done = FALSE;
    g_print ("link-test: variant %s: %s%s\n", lt.variant->name, lt.variant->what,
             LINK_TEST_REQUESTS ? (lt.variant->requests ? "; request callbacks set" : "")
                                : "; library 0.6: no request callbacks");
    if (create_view ())
        start_element ();
}

static guint run_timeout_source;

static gboolean
run_timeout (gpointer user_data)
{
    int timeout_s = GPOINTER_TO_INT (user_data);
    run_timeout_source = 0;
    g_printerr ("link-test: timeout after %d s (%d passed, %d failed so far)\n", timeout_s, lt.pass, lt.fail);
    finish (2);
    return G_SOURCE_REMOVE;
}

int
link_test_run (int width, int height, double scale, int timeout_s)
{
    lt.width = width;
    lt.height = height;
    lt.scale = scale;
    lt.loop = g_main_loop_new (NULL, FALSE);
    if (lt.plan_variants == NULL && !link_test_check_options ())
        return 3;
    if (!server_start ())
        return 2;
    g_print ("link-test: pages on http://127.0.0.1:%u/, %u variant(s) x %u element(s), drag threshold %d px, "
             "request API %s\n", lt.port, lt.plan_variants->len, lt.plan_elements->len,
             uc_webview_global_get_drag_threshold (), LINK_TEST_REQUESTS ? "0.7.0" : "none (library 0.6)");
    run_timeout_source = g_timeout_add_seconds ((guint) timeout_s, run_timeout, GINT_TO_POINTER (timeout_s));
    g_idle_add (next_variant, NULL);
    g_main_loop_run (lt.loop);

    if (run_timeout_source != 0)
        g_source_remove (run_timeout_source);
    run_timeout_source = 0;
    free_view ();
    g_socket_service_stop (lt.service);
    g_socket_listener_close (G_SOCKET_LISTENER (lt.service));
    g_clear_object (&lt.service);
    reset_element_state ();
    g_clear_pointer (&lt.verdict_why, g_free);
    g_array_unref (lt.plan_variants);
    g_array_unref (lt.plan_elements);
    g_main_loop_unref (lt.loop);
    return lt.exit_code;
}
