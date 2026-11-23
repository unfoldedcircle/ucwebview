/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --device-test of ucwebview-test (library 0.9.0), see ucwebview-test-device.h. The page reports itself in its title
 * whenever its snapshot changes (an interval of 100 ms compares): "snap:<document>:<n>:<snapshot> #<ticks>,<hidden>",
 * with a random id per document and a counter. The snapshot is one line: with the API "1 <version> <screen>
 * <powerSource> <powerProfile> <battery.level> <battery.charging> <battery.chargingTime> <data-screen>
 * <data-power-profile>", without it "0 <typeof getBattery>", then "early:" (deviceHost was there before the page's
 * first script), "frame:" (an iframe has deviceHost), "act:" (navigator.userActivation.hasBeenActive), "entry:" (how
 * many of the names of the library's internal entry points are on window: __ucDeviceHostUpdate of 0.9.0's first draft,
 * the extension's two, and window.webkit, which only a message handler of the page's world would bring) and after " |"
 * the events the page saw, in order (a title loses white space at its end). "#<ticks>,<hidden>": the runs of a 100 ms
 * interval while the page was hidden, and whether it ever was. The page also asks "leave this page?" in beforeunload;
 * WebKit puts that question only to a page the user interacted with (FrameLoader.cpp:4064-4078), and the tool counts
 * the questions that reach its script_dialog() callback. The rendered steps wait, as a host may after a load, with
 * uc_webview_when_rendered() for the page's first rendering, whose probe must not count as a user gesture either.
 */
#include "ucwebview-test-device.h"
#include "ucwebview-test-requests.h"

#include "uc_webview.h"

#include <stdio.h>
#include <string.h>

/* ---- options ----------------------------------------------------------------------------------------------- */

static gboolean opt_device_test;

const GOptionEntry device_test_entries[] = {
    { "device-test", 0, 0, G_OPTION_ARG_NONE, &opt_device_test,
      "Check the device state API of a page (library 0.9.0): the state it starts with, state changes, suspend and "
      "resume, invalid input, reload, another site, the hidden page throttling, a killed web process, the API off and "
      "on, and that none of it counts as a user gesture for the page (a page of the tool's own, no network)", NULL },
    { NULL }
};

gboolean
device_test_mode (void)
{
    return opt_device_test;
}

int
device_test_default_timeout (void)
{
    /* nine loads, a 4 s throttling step and about 1 s per other step; the remote at its low power clock is slow */
    return 120;
}

/* ---- the page ----------------------------------------------------------------------------------------------- */

#define BASE_URI "http://device-test.invalid/"
/* another site: WebKit moves the page to another web process (WPE always, the WebKitGTK harness unless
 * UC_HARNESS_PSON=0) */
#define OTHER_URI "http://device-test-other.invalid/"

static const char page_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>device-test</title>"
    "<script>"
    "var log = [], early = ('deviceHost' in window) ? 1 : 0, hidden = 0, frameHas = -1, ticks = 0, seq = 0,"
    "    last = '', doc = Math.random().toString(36).slice(2, 10);"
    "if (window.deviceHost) {"
    " ['suspend', 'resume', 'on-ac', 'on-battery', 'screenchange', 'powerprofilechange'].forEach(function (t) {"
    "  deviceHost.addEventListener(t, function (e) {"
    "   var d = e.detail;"
    "   log.push('host:' + t + (d ? '=' + (d.state || d.profile || d.sleptMs) : '')"
    "            + ((e instanceof CustomEvent) === !!d ? '' : '(wrong class)'));"
    "  });"
    " });"
    " navigator.getBattery().then(function (b) {"
    "  window.bat = b;"
    "  ['chargingchange', 'levelchange', 'chargingtimechange', 'dischargingtimechange'].forEach(function (t) {"
    "   b.addEventListener(t, function () { log.push('battery:' + t); });"
    "  });"
    " });"
    " ['freeze', 'resume'].forEach(function (t) {"
    "  document.addEventListener(t, function () { log.push('document:' + t); });"
    " });"
    "}"
    "document.addEventListener('visibilitychange', function () { if (document.hidden) hidden = 1; });"
    "addEventListener('beforeunload', function (e) { e.preventDefault(); e.returnValue = 'device-test'; });"
    "setInterval(function () { if (document.hidden) ticks++; }, 100);"
    "function snapshot () {"
    " var h = window.deviceHost, b = window.bat, r = document.documentElement, u = navigator.userActivation;"
    " var head = h ? [1, h.version, h.screen, h.powerSource, h.powerProfile, b ? b.level : '-', b ? b.charging : '-',"
    "                 b ? b.chargingTime : '-', r.getAttribute('data-screen'), r.getAttribute('data-power-profile')]"
    "                .join(' ')"
    "              : '0 ' + typeof navigator.getBattery;"
    " var entry = ['__ucDeviceHostUpdate', 'ucwebviewDeviceUpdate', 'ucwebviewDeviceStart', 'webkit']"
    "             .filter(function (n) { return n in window; }).length;"
    " return head + ' early:' + early + ' frame:' + frameHas + ' act:' + (u ? (u.hasBeenActive ? 1 : 0) : '-')"
    "        + ' entry:' + entry + ' |' + (log.length > 0 ? ' ' + log.join(',') : '');"
    "}"
    "function report () {"
    " var s = snapshot () + ' #' + ticks + ',' + hidden;"
    " if (s !== last) { last = s; document.title = 'snap:' + doc + ':' + (++seq) + ':' + s; }"
    "}"
    "setInterval(report, 100);"
    "</script></head><body style='background:#fff'><p>device-test</p>"
    "<iframe srcdoc=\"<script>parent.frameHas = ('deviceHost' in window) ? 1 : 0;</script>\"></iframe>"
    "</body></html>";

/* ---- steps -------------------------------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *what;
    void (*act) (void);
    gboolean load;          /* act starts a new document: only its reports count */
    gboolean quiet;         /* nothing may change: the report after QUIET_MS */
    int dialogs;            /* "leave this page?" questions of the page the step leaves */
    const char *expect;     /* NULL: the throttling step */
    gboolean rendered;      /* uc_webview_when_rendered() at the step's FINISHED; the step waits for its DONE */
} Step;

#define QUIET_MS 800
#define STEP_MS 8000
#define VISIBLE_MS 1000
#define HIDDEN_MS 3000
#define SETTLE_MS 600
/* a 100 ms interval in 3 s hidden: about 3 with the 1 s alignment, up to 30 without */
#define HIDDEN_MAX_TICKS 6

static struct {
    GMainLoop *loop;
    uc_webview *wv;
    void *held;
    guint step;
    gboolean step_done;
    char *doc;              /* the reporting document */
    char *step_doc;         /* the reporting document when the step started */
    char *latest;           /* its latest snapshot, without " #<ticks>,<hidden>" */
    guint ticks, hidden;
    guint ticks_at_hide;
    gint64 step_start;
    guint source;           /* the step's timer */
    guint aux_source;       /* the throttling and crash phases */
    int dialogs;            /* beforeunload questions in this step */
    GString *other_dialogs; /* other dialogs in this step */
    gboolean expect_terminated;
    int processes_while_gone;
    gboolean want_rendered; /* the step asks at its next FINISHED */
    int rendered_status;    /* -1: no result yet */
    gboolean in_call;       /* inside uc_webview_when_rendered() for the dead page */
    int gone_rendered;      /* its result, -1: none yet, -2: from inside the call */
    int pass, fail;
    gboolean finished;
    int exit_code;
    GString *calls;         /* what went wrong besides the snapshot: return values, processes, terminations */
} st;

static void
call_rc (const char *what, int rc, int expected)
{
    if (rc != expected)
        g_string_append_printf (st.calls, "%s%s returned %d, expected %d", st.calls->len > 0 ? "; " : "", what, rc,
                                expected);
}

static void
note (const char *text)
{
    g_string_append_printf (st.calls, "%s%s", st.calls->len > 0 ? "; " : "", text);
}

#define FULL_STATE "{\"screen\":\"dim\",\"powerSource\":\"battery\",\"powerProfile\":\"power-saver\",\"level\":42}"

static void
act_first_load (void)
{
    call_rc ("set_device_api(1)", uc_webview_set_device_api (st.wv, 1), 0);
    call_rc ("set_device_state(full)", uc_webview_set_device_state (st.wv, FULL_STATE), 0);
    uc_webview_load_html (st.wv, page_html, BASE_URI);
}

static void
act_screen_off (void)
{
    call_rc ("set_device_state", uc_webview_set_device_state (st.wv, "{\"screen\":\"off\"}"), 0);
}

static void
act_same_state (void)
{
    call_rc ("set_device_state", uc_webview_set_device_state (st.wv, "{\"screen\":\"off\",\"level\":42}"), 0);
}

static void
act_suspend (void)
{
    /* like remote-ui: the state of the standby, then the event */
    call_rc ("set_device_state",
             uc_webview_set_device_state (st.wv, "{\"screen\":\"off\",\"powerProfile\":\"power-saver\"}"), 0);
    call_rc ("device_event(suspend)", uc_webview_device_event (st.wv, "suspend", 0), 0);
}

static void
act_resume (void)
{
    /* like remote-ui: the event, then the state after the wake */
    call_rc ("device_event(resume)", uc_webview_device_event (st.wv, "resume", 61234), 0);
    call_rc ("set_device_state", uc_webview_set_device_state (st.wv, "{\"screen\":\"on\",\"powerProfile\":\"balanced\"}"),
             0);
}

static void
act_power (void)
{
    call_rc ("set_device_state", uc_webview_set_device_state (st.wv, "{\"powerSource\":\"ac\",\"level\":100}"), 0);
}

static void
act_invalid (void)
{
    call_rc ("set_device_state(invalid values)",
             uc_webview_set_device_state (st.wv, "{\"screen\":\"bright\",\"level\":250,\"battery\":{\"level\":1}}"), 0);
    call_rc ("set_device_state(not JSON)", uc_webview_set_device_state (st.wv, "screen=off"), -1);
    call_rc ("set_device_state(an array)", uc_webview_set_device_state (st.wv, "[{\"screen\":\"off\"}]"), -1);
    call_rc ("set_device_state(NULL)", uc_webview_set_device_state (st.wv, NULL), -1);
    call_rc ("device_event(standby)", uc_webview_device_event (st.wv, "standby", 0), -1);
    call_rc ("device_event(NULL)", uc_webview_device_event (st.wv, NULL, 0), -1);
    call_rc ("device_event(resume, -1)", uc_webview_device_event (st.wv, "resume", -1), -1);
}

/* a new document of the same page (a reload would fetch the base URI, which does not exist) */
static void
act_reload (void)
{
    uc_webview_load_html (st.wv, page_html, BASE_URI);
}

static void
act_other_site (void)
{
    uc_webview_load_html (st.wv, page_html, OTHER_URI);
}

static void
act_screen_dim (void)
{
    call_rc ("set_device_state", uc_webview_set_device_state (st.wv, "{\"screen\":\"dim\"}"), 0);
}

static gboolean unhide (gpointer user_data);

static gboolean
hide (gpointer user_data)
{
    (void) user_data;
    st.ticks_at_hide = st.ticks;
    uc_webview_set_mapped (st.wv, 0);
    st.aux_source = g_timeout_add (HIDDEN_MS, unhide, NULL);
    return G_SOURCE_REMOVE;
}

/* The page's interval runs since its load: WebKit aligns a timer to the hidden page's interval only once it reached
 * the maximum nesting level, after its fifth run (Document::domTimerAlignmentInterval(), DOMTimer.cpp). Only the
 * ticks while the page is hidden count (hiding and showing take a moment). */
static void
act_throttle (void)
{
    st.aux_source = g_timeout_add (VISIBLE_MS, hide, NULL);
}

/* the killed web process: the state changes while the page is gone, a rendered request ends CANCELLED from the main
 * context, no web process may start for any of it, the next load starts with the state */
static void
on_gone_rendered (uc_webview *wv, int status, void *user)
{
    (void) wv; (void) user;
    st.gone_rendered = st.in_call ? -2 : status;
}

static gboolean
crash_reload (gpointer user_data)
{
    int now = request_test_web_processes (FALSE);

    (void) user_data;
    st.aux_source = 0;
    if (st.gone_rendered != UC_WEBVIEW_RENDERED_CANCELLED) {
        char *text = g_strdup_printf ("uc_webview_when_rendered() of the dead page: %s, expected CANCELLED from the "
                                      "main context", st.gone_rendered == -1 ? "no result" : st.gone_rendered == -2
                                      ? "a result inside the call" : st.gone_rendered == 0 ? "DONE" : "TIMEOUT");
        note (text);
        g_free (text);
    }
    if (now > st.processes_while_gone) {
        char *text = g_strdup_printf ("%d web process(es) after the changes of a dead page, %d before: one started "
                                      "for it", now, st.processes_while_gone);
        note (text);
        g_free (text);
    }
    uc_webview_load_html (st.wv, page_html, BASE_URI);
    return G_SOURCE_REMOVE;
}

static gboolean
crash_while_gone (gpointer user_data)
{
    (void) user_data;
    st.processes_while_gone = request_test_web_processes (FALSE);
    call_rc ("set_device_state(dead page)", uc_webview_set_device_state (st.wv, "{\"screen\":\"off\"}"), 0);
    call_rc ("device_event(suspend, dead page)", uc_webview_device_event (st.wv, "suspend", 0), 0);
    st.gone_rendered = -1;
    st.in_call = TRUE;
    call_rc ("when_rendered(dead page)", uc_webview_when_rendered (st.wv, on_gone_rendered, NULL), 0);
    st.in_call = FALSE;
    st.aux_source = g_timeout_add (SETTLE_MS, crash_reload, NULL);
    return G_SOURCE_REMOVE;
}

static void
act_crash (void)
{
    st.expect_terminated = TRUE;
    if (request_test_web_processes (TRUE) == 0)
        note ("no web process to kill");
    /* on_terminated() goes on */
}

static void
act_off (void)
{
    call_rc ("set_device_api(0)", uc_webview_set_device_api (st.wv, 0), 0);
    call_rc ("set_device_api(0) again", uc_webview_set_device_api (st.wv, 0), 0);
    /* kept for later, not sent: the shown page keeps its API without updates */
    call_rc ("set_device_state", uc_webview_set_device_state (st.wv, "{\"screen\":\"dim\"}"), 0);
    call_rc ("device_event(suspend)", uc_webview_device_event (st.wv, "suspend", 0), 0);
}

static void
act_on_shown (void)
{
    call_rc ("set_device_api(1)", uc_webview_set_device_api (st.wv, 1), 0);
}

/* a page without the device API: only uc_webview_when_rendered() at its FINISHED, as a host may wait after a load */
static void
act_plain_load (void)
{
    call_rc ("set_device_api(0)", uc_webview_set_device_api (st.wv, 0), 0);
    uc_webview_load_html (st.wv, page_html, BASE_URI);
}

/* the control: a script of WebKit's API is a user gesture (uc_webview.h, uc_webview_run_javascript()) */
static void
act_control_script (void)
{
    uc_webview_run_javascript (st.wv, "void 0;");
}

#define TAIL " early:1 frame:0 act:0 entry:0 |"

static const Step steps[] = {
    { "injected", "the state the page starts with, before its first script; no frame gets the API; no event",
      act_first_load, TRUE, FALSE, 0, "1 1 dim battery power-saver 0.42 false Infinity dim power-saver" TAIL, FALSE },
    { "screen-off", "a state change: screenchange, the attribute",
      act_screen_off, FALSE, FALSE, 0, "1 1 off battery power-saver 0.42 false Infinity off power-saver" TAIL
      " host:screenchange=off", FALSE },
    { "same-state", "the same state again: no event",
      act_same_state, FALSE, TRUE, 0, "1 1 off battery power-saver 0.42 false Infinity off power-saver" TAIL
      " host:screenchange=off", FALSE },
    { "suspend", "the state, then freeze and suspend",
      act_suspend, FALSE, FALSE, 0, "1 1 off battery power-saver 0.42 false Infinity off power-saver" TAIL
      " host:screenchange=off,document:freeze,host:suspend", FALSE },
    { "resume", "resume on both with the time slept, then the new state",
      act_resume, FALSE, FALSE, 0, "1 1 on battery balanced 0.42 false Infinity on balanced" TAIL
      " host:screenchange=off,document:freeze,host:suspend,document:resume,host:resume=61234,host:screenchange=on,"
      "host:powerprofilechange=balanced", FALSE },
    { "power", "on power at full level: chargingchange and on-ac together, levelchange, chargingTime 0",
      act_power, FALSE, FALSE, 0, "1 1 on ac balanced 1 true 0 on balanced" TAIL
      " host:screenchange=off,document:freeze,host:suspend,document:resume,host:resume=61234,host:screenchange=on,"
      "host:powerprofilechange=balanced,battery:chargingchange,host:on-ac,battery:levelchange", FALSE },
    { "invalid", "invalid values ignored, invalid input refused: no event",
      act_invalid, FALSE, TRUE, 0, "1 1 on ac balanced 1 true 0 on balanced" TAIL
      " host:screenchange=off,document:freeze,host:suspend,document:resume,host:resume=61234,host:screenchange=on,"
      "host:powerprofilechange=balanced,battery:chargingchange,host:on-ac,battery:levelchange", FALSE },
    { "reload", "leaving the page after all those changes and events asks nothing (no user activation); the new "
      "document starts with the current state, without events",
      act_reload, TRUE, FALSE, 0, "1 1 on ac balanced 1 true 0 on balanced" TAIL, FALSE },
    { "other-site", "a page of another site, in another web process (a process swap), starts with the current state",
      act_other_site, TRUE, FALSE, 0, "1 1 on ac balanced 1 true 0 on balanced" TAIL, FALSE },
    { "other-site-change", "a change reaches the page in its new web process",
      act_screen_dim, FALSE, FALSE, 0, "1 1 dim ac balanced 1 true 0 dim balanced" TAIL " host:screenchange=dim",
      FALSE },
    { "home", "back to the first site (a process swap again): the current state, nothing asked",
      act_reload, TRUE, FALSE, 0, "1 1 dim ac balanced 1 true 0 dim balanced" TAIL, FALSE },
    { "throttle", "a 100 ms interval of the unmapped view's hidden page runs about once a second",
      act_throttle, FALSE, FALSE, 0, NULL, FALSE },
    { "crash", "the web process killed: changes and a rendered request while the page is gone start no web process "
      "(the request ends CANCELLED); the next load starts with the state set meanwhile, without events",
      act_crash, TRUE, FALSE, 0, "1 1 off ac balanced 1 true 0 off balanced" TAIL, FALSE },
    { "off-shown", "API off: the shown page keeps its API, without updates",
      act_off, FALSE, TRUE, 0, "1 1 off ac balanced 1 true 0 off balanced" TAIL, FALSE },
    { "off-reload", "API off: the next page has no deviceHost and no getBattery",
      act_reload, TRUE, FALSE, 0, "0 undefined early:0 frame:0 act:0 entry:0 |", FALSE },
    { "on-shown", "API on: the shown page gets it at its next load only",
      act_on_shown, FALSE, TRUE, 0, "0 undefined early:0 frame:0 act:0 entry:0 |", FALSE },
    { "on-reload", "API on: the next page starts with the state set while it was off",
      act_reload, TRUE, FALSE, 0, "1 1 dim ac balanced 1 true 0 dim balanced" TAIL, FALSE },
    { "control-gesture", "control: uc_webview_run_javascript() gives the page user activation",
      act_control_script, FALSE, FALSE, 0, "1 1 dim ac balanced 1 true 0 dim balanced early:1 frame:0 act:1 entry:0 |",
      FALSE },
    { "control-leave", "control: leaving that page asks \"leave this page?\" once",
      act_reload, TRUE, FALSE, 1, "1 1 dim ac balanced 1 true 0 dim balanced" TAIL, FALSE },
    { "rendered", "a page without the device API, rendered (uc_webview_when_rendered() at FINISHED: DONE): no user "
      "activation",
      act_plain_load, TRUE, FALSE, 0, "0 undefined early:0 frame:0 act:0 entry:0 |", TRUE },
    { "rendered-leave", "leaving it asks nothing; the next page, rendered again, has no activation either",
      act_reload, TRUE, FALSE, 0, "0 undefined early:0 frame:0 act:0 entry:0 |", TRUE },
};

/* ---- the run -------------------------------------------------------------------------------------------------- */

static void
finish (int code)
{
    if (st.finished)
        return;
    st.finished = TRUE;
    st.exit_code = code;
    g_main_loop_quit (st.loop);
}

static void start_step (void);

static gboolean
next_step (gpointer user_data)
{
    (void) user_data;
    st.source = 0;
    st.step++;
    start_step ();
    return G_SOURCE_REMOVE;
}

static void
end_step (gboolean ok, const char *got)
{
    const Step *step = &steps[st.step];

    if (st.step_done)
        return;
    st.step_done = TRUE;
    if (st.dialogs != step->dialogs) {
        char *text = g_strdup_printf ("%d \"leave this page?\" question(s), expected %d", st.dialogs, step->dialogs);
        note (text);
        g_free (text);
    }
    if (st.other_dialogs->len > 0)
        note (st.other_dialogs->str);
    if (step->rendered && st.rendered_status != UC_WEBVIEW_RENDERED_DONE) {
        char *text = g_strdup_printf ("uc_webview_when_rendered(): %s", st.rendered_status < 0 ? "no result"
                                      : st.rendered_status == UC_WEBVIEW_RENDERED_TIMEOUT ? "TIMEOUT" : "CANCELLED");
        note (text);
        g_free (text);
    }
    if (st.calls->len > 0)
        ok = FALSE;
    if (ok) {
        st.pass++;
        g_print ("device-test: PASS %s\n", step->name);
    } else {
        st.fail++;
        g_print ("device-test: FAIL %s: %s\n", step->name, step->what);
        if (st.calls->len > 0)
            g_print ("device-test:   %s\n", st.calls->str);
        if (step->expect != NULL)
            g_print ("device-test:   expected \"%s\"\n", step->expect);
        g_print ("device-test:   got      \"%s\"\n", got != NULL ? got : "(no report)");
    }
    if (st.source != 0)
        g_source_remove (st.source);
    if (st.aux_source != 0)
        g_source_remove (st.aux_source);
    st.aux_source = 0;
    st.expect_terminated = FALSE;
    st.source = g_idle_add (next_step, NULL);
}

/* the report of the step's document: a load step waits for the next document's */
static gboolean
current_document (void)
{
    return st.latest != NULL && (!steps[st.step].load || g_strcmp0 (st.doc, st.step_doc) != 0);
}

/* a report came: a step that waits for its snapshot ends when it matches */
static void
check (void)
{
    const Step *step;

    if (st.finished || st.step_done || st.step >= G_N_ELEMENTS (steps))
        return;
    step = &steps[st.step];
    if (step->expect == NULL || step->quiet || !current_document ())
        return;
    if (step->rendered && st.rendered_status < 0)
        return;
    if (strcmp (st.latest, step->expect) == 0)
        end_step (TRUE, st.latest);
}

static void
on_rendered (uc_webview *wv, int status, void *user)
{
    (void) wv; (void) user;
    if (st.finished || st.step_done)
        return;
    st.rendered_status = status;
    g_print ("device-test:   rendered: %s\n", status == UC_WEBVIEW_RENDERED_DONE ? "done"
             : status == UC_WEBVIEW_RENDERED_TIMEOUT ? "timeout" : "cancelled");
    check ();
}

static gboolean
step_timeout (gpointer user_data)
{
    (void) user_data;
    st.source = 0;
    end_step (FALSE, current_document () ? st.latest : NULL);
    return G_SOURCE_REMOVE;
}

/* a quiet step: nothing may have changed */
static gboolean
quiet_done (gpointer user_data)
{
    const char *expect = steps[st.step].expect;

    (void) user_data;
    st.source = 0;
    end_step (current_document () && strcmp (st.latest, expect) == 0, current_document () ? st.latest : NULL);
    return G_SOURCE_REMOVE;
}

/* the throttling step's answer, SETTLE_MS after the view is shown again */
static gboolean
throttle_done (gpointer user_data)
{
    guint ticks = st.ticks - st.ticks_at_hide;
    char got[64];

    (void) user_data;
    st.aux_source = 0;
    g_print ("device-test: throttle: %u ticks of a 100 ms interval while hidden, of %d ms unmapped (at most %d expected, "
             "about %d without throttling), page hidden: %s\n", ticks, HIDDEN_MS, HIDDEN_MAX_TICKS, HIDDEN_MS / 100,
             st.hidden ? "yes" : "no");
    g_snprintf (got, sizeof got, "ticks %u hidden %u", ticks, st.hidden);
    end_step (st.hidden == 1 && ticks <= HIDDEN_MAX_TICKS, got);
    return G_SOURCE_REMOVE;
}

static gboolean
unhide (gpointer user_data)
{
    (void) user_data;
    uc_webview_set_mapped (st.wv, 1);
    st.aux_source = g_timeout_add (SETTLE_MS, throttle_done, NULL);
    return G_SOURCE_REMOVE;
}

/* "snap:<document>:<n>:<snapshot> #<ticks>,<hidden>" */
static void
on_title (uc_webview *wv, const char *title, void *user)
{
    char **parts;
    const char *hash;

    (void) wv; (void) user;
    if (st.finished || title == NULL || !g_str_has_prefix (title, "snap:"))
        return;
    parts = g_strsplit (title + strlen ("snap:"), ":", 3);
    if (g_strv_length (parts) == 3 && (hash = strrchr (parts[2], '#')) != NULL && hash > parts[2]) {
        g_free (st.doc);
        st.doc = g_strdup (parts[0]);
        g_free (st.latest);
        st.latest = g_strndup (parts[2], (gsize) (hash - parts[2] - 1));
        if (sscanf (hash + 1, "%u,%u", &st.ticks, &st.hidden) != 2)
            st.ticks = st.hidden = 0;
    }
    g_strfreev (parts);
    check ();
}

static void
on_load (uc_webview *wv, int event, const char *uri, void *user)
{
    (void) wv; (void) uri; (void) user;
    if (st.finished)
        return;
    if (event == UC_WEBVIEW_LOAD_FAILED) {
        g_printerr ("device-test: load failed\n");
        st.fail++;
        finish (1);
        return;
    }
    /* as a host may: wait for the loaded page's first rendering */
    if (event == UC_WEBVIEW_LOAD_FINISHED && st.want_rendered) {
        st.want_rendered = FALSE;
        if (uc_webview_when_rendered (st.wv, on_rendered, NULL) != 0)
            note ("uc_webview_when_rendered() refused");
    }
}

/* every dialog is answered at once: a beforeunload question with "leave", so that the load goes on */
static void
on_dialog (uc_webview *wv, uint64_t request, int type, const char *origin, const char *message,
           const char *default_text, void *user)
{
    (void) origin; (void) message; (void) default_text; (void) user;
    if (type == UC_WEBVIEW_DIALOG_BEFORE_UNLOAD) {
        st.dialogs++;
        g_print ("device-test:   \"leave this page?\" asked\n");
    } else {
        g_string_append_printf (st.other_dialogs, "%sdialog of type %d", st.other_dialogs->len > 0 ? ", " : "", type);
    }
    uc_webview_request_reply (wv, request, 1, NULL);
}

static void
on_frame (uc_webview *wv, const uc_webview_frame *frame, void *user)
{
    (void) user;
    uc_webview_frame_rendered (wv, frame->handle);
    if (st.held != NULL)
        uc_webview_frame_release (wv, st.held);
    st.held = frame->handle;
}

static void
on_terminated (uc_webview *wv, int reason, void *user)
{
    (void) wv; (void) user;
    if (st.expect_terminated && !st.finished) {
        st.expect_terminated = FALSE;
        g_print ("device-test:   web process terminated (reason %d), as the step wants\n", reason);
        /* from the main context: the library's callback is still running */
        st.aux_source = g_idle_add (crash_while_gone, NULL);
        return;
    }
    g_printerr ("device-test: web process terminated (reason %d)\n", reason);
    st.fail++;
    finish (1);
}

static void
start_step (void)
{
    const Step *step;

    if (st.finished)
        return;
    if (st.step >= G_N_ELEMENTS (steps)) {
        g_print ("device-test: %d passed, %d failed: %s\n", st.pass, st.fail, st.fail == 0 ? "ok" : "FAILED");
        finish (st.fail == 0 ? 0 : 1);
        return;
    }
    step = &steps[st.step];
    g_print ("device-test: step %s: %s\n", step->name, step->what);
    g_string_truncate (st.calls, 0);
    g_string_truncate (st.other_dialogs, 0);
    st.dialogs = 0;
    st.step_done = FALSE;
    st.want_rendered = step->rendered;
    st.rendered_status = -1;
    g_free (st.step_doc);
    st.step_doc = g_strdup (st.doc);
    st.step_start = g_get_monotonic_time ();
    step->act ();
    if (st.step_done)
        return;
    if (step->expect == NULL)
        st.source = g_timeout_add (STEP_MS, step_timeout, NULL);    /* act_throttle() hides and shows the view */
    else if (step->quiet)
        st.source = g_timeout_add (QUIET_MS, quiet_done, NULL);
    else
        st.source = g_timeout_add (STEP_MS, step_timeout, NULL);
}

static guint run_timeout_source;

static gboolean
run_timeout (gpointer user_data)
{
    int timeout_s = GPOINTER_TO_INT (user_data);
    run_timeout_source = 0;
    g_printerr ("device-test: timeout after %d s in step %s (%d passed, %d failed so far)\n", timeout_s,
                st.step < G_N_ELEMENTS (steps) ? steps[st.step].name : "-", st.pass, st.fail);
    finish (2);
    return G_SOURCE_REMOVE;
}

static gboolean
first_step (gpointer user_data)
{
    (void) user_data;
    st.source = 0;
    start_step ();
    return G_SOURCE_REMOVE;
}

int
device_test_run (int width, int height, double scale, int timeout_s)
{
    uc_webview_callbacks cb;

    st.loop = g_main_loop_new (NULL, FALSE);
    st.calls = g_string_new (NULL);
    st.other_dialogs = g_string_new (NULL);

    memset (&cb, 0, sizeof cb);
    cb.frame_ready = on_frame;
    cb.load_changed = on_load;
    cb.title_changed = on_title;
    cb.web_process_terminated = on_terminated;
    cb.script_dialog = on_dialog;
    st.wv = uc_webview_new_sized (width, height, &cb, sizeof cb, NULL);
    if (st.wv == NULL) {
        g_printerr ("device-test: uc_webview_new_sized failed\n");
        g_string_free (st.calls, TRUE);
        g_string_free (st.other_dialogs, TRUE);
        g_main_loop_unref (st.loop);
        return 2;
    }
    /* what remote-ui does for a new view */
    uc_webview_set_scale (st.wv, scale);
    uc_webview_set_mapped (st.wv, 1);
    uc_webview_set_focused (st.wv, 1);

    g_print ("device-test: %u steps, pages from load_html() with base " BASE_URI " and " OTHER_URI "\n",
             (unsigned) G_N_ELEMENTS (steps));
    run_timeout_source = g_timeout_add_seconds ((guint) timeout_s, run_timeout, GINT_TO_POINTER (timeout_s));
    st.source = g_idle_add (first_step, NULL);
    g_main_loop_run (st.loop);

    if (run_timeout_source != 0)
        g_source_remove (run_timeout_source);
    run_timeout_source = 0;
    if (st.source != 0)
        g_source_remove (st.source);
    st.source = 0;
    if (st.aux_source != 0)
        g_source_remove (st.aux_source);
    st.aux_source = 0;
    if (st.held != NULL)
        uc_webview_frame_release (st.wv, st.held);
    st.held = NULL;
    uc_webview_free (st.wv);
    st.wv = NULL;
    g_clear_pointer (&st.doc, g_free);
    g_clear_pointer (&st.step_doc, g_free);
    g_clear_pointer (&st.latest, g_free);
    g_string_free (st.calls, TRUE);
    g_string_free (st.other_dialogs, TRUE);
    g_main_loop_unref (st.loop);
    return st.exit_code;
}
