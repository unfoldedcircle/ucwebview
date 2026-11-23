/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_pinch.c (GLib only): which touches go on to WebKit's own gesture controller, when a pinch
 * begins (the threshold), its scale and midpoint, and how it ends (lift, touch cancel, the page taking over, a
 * navigation). Run with `make check`.
 */
#include "uc_events.h"
#include "uc_pinch.h"
#include "uc_webview.h"

#define DOWN   UC_PINCH_TOUCH_DOWN
#define MOVE   UC_PINCH_TOUCH_MOVE
#define UP     UC_PINCH_TOUCH_UP
#define CANCEL UC_PINCH_TOUCH_CANCEL

#define THRESHOLD 16.0

typedef struct {
    UcPinchPhase phase;
    double scale, x, y;
} Report;

typedef struct {
    UcPinch pinch;
    GArray *reports;    /* Report */
} Fixture;

static void
on_pinch (UcPinchPhase phase, double scale, double x, double y, gpointer user_data)
{
    Fixture *fx = user_data;
    Report r = { phase, scale, x, y };
    g_array_append_val (fx->reports, r);
}

static void
fixture_init (Fixture *fx)
{
    fx->reports = g_array_new (FALSE, TRUE, sizeof (Report));
    uc_pinch_init (&fx->pinch, THRESHOLD, on_pinch, fx);
}

static void
fixture_clear (Fixture *fx)
{
    g_array_unref (fx->reports);
}

static UcPinchVerdict
touch (Fixture *fx, int type, guint32 id, double x, double y)
{
    return uc_pinch_touch (&fx->pinch, type, id, x, y);
}

static const Report *
report (Fixture *fx, guint i)
{
    g_assert_cmpuint (i, <, fx->reports->len);
    return &g_array_index (fx->reports, Report, i);
}

static void
assert_report (Fixture *fx, guint i, UcPinchPhase phase, double scale, double x, double y)
{
    const Report *r = report (fx, i);
    g_assert_cmpint (r->phase, ==, phase);
    g_assert_cmpfloat_with_epsilon (r->scale, scale, 1e-9);
    g_assert_cmpfloat_with_epsilon (r->x, x, 1e-9);
    g_assert_cmpfloat_with_epsilon (r->y, y, 1e-9);
}

/* the event types are WPEEventType values (uc_events.h mirrors them, uc_wpe_view.c asserts them) */
static void
test_event_values (void)
{
    g_assert_cmpint (UC_PINCH_TOUCH_DOWN, ==, UC_WPE_EVENT_TOUCH_DOWN);
    g_assert_cmpint (UC_PINCH_TOUCH_UP, ==, UC_WPE_EVENT_TOUCH_UP);
    g_assert_cmpint (UC_PINCH_TOUCH_MOVE, ==, UC_WPE_EVENT_TOUCH_MOVE);
    g_assert_cmpint (UC_PINCH_TOUCH_CANCEL, ==, UC_WPE_EVENT_TOUCH_CANCEL);
    g_assert_cmpint (UC_PINCH_BEGIN, ==, UC_WEBVIEW_PINCH_BEGIN);
    g_assert_cmpint (UC_PINCH_UPDATE, ==, UC_WEBVIEW_PINCH_UPDATE);
    g_assert_cmpint (UC_PINCH_END, ==, UC_WEBVIEW_PINCH_END);
    g_assert_cmpint (UC_PINCH_CANCEL, ==, UC_WEBVIEW_PINCH_CANCEL);
}

/* one finger: everything goes to WebKit's controller (tap, scroll), nothing is reported */
static void
test_one_finger (void)
{
    Fixture fx;
    fixture_init (&fx);
    g_assert_cmpint (touch (&fx, DOWN, 0, 100, 100), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_ONE);
    g_assert_cmpint (touch (&fx, MOVE, 0, 100, 300), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (touch (&fx, UP, 0, 100, 300), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    /* a tap again, with another sequence id */
    g_assert_cmpint (touch (&fx, DOWN, 7, 50, 50), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (touch (&fx, UP, 7, 50, 50), ==, UC_PINCH_FORWARD);
    /* events of a sequence the tracker never saw go on as well (the page handled its press) */
    g_assert_cmpint (touch (&fx, MOVE, 3, 10, 10), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (touch (&fx, UP, 3, 10, 10), ==, UC_PINCH_FORWARD);
    g_assert_cmpuint (fx.reports->len, ==, 0);
    /* other event types pass untouched */
    g_assert_cmpint (touch (&fx, UC_WPE_EVENT_POINTER_DOWN, 0, 1, 1), ==, UC_PINCH_FORWARD);
    fixture_clear (&fx);
}

/* pinch out: the second finger takes over, BEGIN after the threshold at scale 1, UPDATE with span ratio and
 * midpoint, END with the last update's values; the remaining finger neither scrolls nor taps */
static void
test_pinch_out (void)
{
    Fixture fx;
    fixture_init (&fx);
    g_assert_cmpint (touch (&fx, DOWN, 0, 200, 400), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (touch (&fx, DOWN, 1, 280, 400), ==, UC_PINCH_TAKE_OVER);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_POSSIBLE);
    /* span 80 -> 90: within the threshold, nothing yet, but no scroll either */
    g_assert_cmpint (touch (&fx, MOVE, 1, 290, 400), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpuint (fx.reports->len, ==, 0);
    /* span 100 (20 px more than at the second press): BEGIN at the midpoint, scale 1 */
    g_assert_cmpint (touch (&fx, MOVE, 0, 190, 400), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpuint (fx.reports->len, ==, 1);
    assert_report (&fx, 0, UC_PINCH_BEGIN, 1.0, 240, 400);
    /* span 200: scale 2 */
    g_assert_cmpint (touch (&fx, MOVE, 0, 140, 400), ==, UC_PINCH_SUPPRESS);
    assert_report (&fx, 1, UC_PINCH_UPDATE, 1.5, 215, 400);
    g_assert_cmpint (touch (&fx, MOVE, 1, 340, 400), ==, UC_PINCH_SUPPRESS);
    assert_report (&fx, 2, UC_PINCH_UPDATE, 2.0, 240, 400);
    /* the lift position does not change the result */
    g_assert_cmpint (touch (&fx, UP, 1, 360, 420), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpuint (fx.reports->len, ==, 4);
    assert_report (&fx, 3, UC_PINCH_END, 2.0, 240, 400);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_DONE);
    /* the remaining finger is not WebKit's again */
    g_assert_cmpint (touch (&fx, MOVE, 0, 140, 600), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (touch (&fx, UP, 0, 140, 600), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    g_assert_cmpuint (fx.reports->len, ==, 4);
    /* and the next press is a normal one */
    g_assert_cmpint (touch (&fx, DOWN, 0, 10, 10), ==, UC_PINCH_FORWARD);
    fixture_clear (&fx);
}

/* pinch in, diagonal, the first finger lifts first */
static void
test_pinch_in (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 4, 100, 100);
    touch (&fx, DOWN, 5, 400, 500);     /* span 500 */
    touch (&fx, MOVE, 5, 370, 460);     /* span 450: begins, base 450 */
    assert_report (&fx, 0, UC_PINCH_BEGIN, 1.0, 235, 280);
    touch (&fx, MOVE, 4, 145, 160);     /* span 375 */
    assert_report (&fx, 1, UC_PINCH_UPDATE, 375.0 / 450.0, 257.5, 310);
    touch (&fx, UP, 4, 145, 160);
    assert_report (&fx, 2, UC_PINCH_END, 375.0 / 450.0, 257.5, 310);
    g_assert_cmpint (touch (&fx, UP, 5, 370, 460), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    fixture_clear (&fx);
}

/* the threshold: a span change of exactly the threshold does not begin, a little more does */
static void
test_threshold (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 200, 100);                 /* span 100 */
    touch (&fx, MOVE, 1, 216, 100);                 /* +16 */
    touch (&fx, MOVE, 1, 184, 100);                 /* -16 */
    g_assert_cmpuint (fx.reports->len, ==, 0);
    /* a two-finger pan in small steps (the library gets one finger at a time) keeps the span: nothing */
    for (int i = 1; i <= 20; i++) {
        touch (&fx, MOVE, 0, 100, 100 + 5 * i);
        touch (&fx, MOVE, 1, 184, 100 + 5 * i);     /* span 84 .. 84.15 */
    }
    g_assert_cmpuint (fx.reports->len, ==, 0);
    touch (&fx, MOVE, 1, 216.5, 200);               /* span 116.5: +16.5 since the second press */
    g_assert_cmpuint (fx.reports->len, ==, 1);
    assert_report (&fx, 0, UC_PINCH_BEGIN, 1.0, 158.25, 200);
    fixture_clear (&fx);

    /* the threshold is the one given (the display's drag threshold) */
    fixture_init (&fx);
    uc_pinch_init (&fx.pinch, 40.0, on_pinch, &fx);
    touch (&fx, DOWN, 0, 0, 0);
    touch (&fx, DOWN, 1, 100, 0);
    touch (&fx, MOVE, 1, 139, 0);
    g_assert_cmpuint (fx.reports->len, ==, 0);
    touch (&fx, MOVE, 1, 141, 0);
    g_assert_cmpuint (fx.reports->len, ==, 1);
    fixture_clear (&fx);
}

/* two fingers that do not pinch (a two-finger tap) report nothing and give WebKit nothing */
static void
test_two_finger_tap (void)
{
    Fixture fx;
    fixture_init (&fx);
    g_assert_cmpint (touch (&fx, DOWN, 0, 100, 100), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (touch (&fx, DOWN, 1, 160, 100), ==, UC_PINCH_TAKE_OVER);
    g_assert_cmpint (touch (&fx, UP, 0, 100, 100), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_DONE);
    g_assert_cmpint (touch (&fx, UP, 1, 160, 100), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    g_assert_cmpuint (fx.reports->len, ==, 0);
    fixture_clear (&fx);
}

/* a third finger is ignored; the pinch follows its two fingers and ends with one of them */
static void
test_third_finger (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 200, 100);
    touch (&fx, MOVE, 1, 300, 100);                 /* span 200: BEGIN */
    g_assert_cmpint (touch (&fx, DOWN, 2, 400, 400), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (touch (&fx, MOVE, 2, 450, 450), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpuint (fx.reports->len, ==, 1);
    g_assert_cmpint (touch (&fx, UP, 2, 450, 450), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_PINCHING);
    touch (&fx, MOVE, 1, 500, 100);                 /* span 400 */
    assert_report (&fx, 1, UC_PINCH_UPDATE, 2.0, 300, 100);
    touch (&fx, UP, 0, 100, 100);
    assert_report (&fx, 2, UC_PINCH_END, 2.0, 300, 100);
    fixture_clear (&fx);
}

/* a touch cancel of a pinch finger ends the pinch with CANCEL */
static void
test_touch_cancel (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 200, 100);
    touch (&fx, MOVE, 1, 250, 100);
    touch (&fx, MOVE, 1, 325, 100);                 /* base 150, span 225 */
    g_assert_cmpint (touch (&fx, CANCEL, 1, 325, 100), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpuint (fx.reports->len, ==, 3);
    assert_report (&fx, 2, UC_PINCH_CANCEL, 1.5, 212.5, 100);
    g_assert_cmpint (touch (&fx, CANCEL, 0, 100, 100), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    g_assert_cmpuint (fx.reports->len, ==, 3);
    fixture_clear (&fx);
}

/* the page handled a touch (WebKit cancels the controller): CANCEL, every finger forgotten */
static void
test_page_takes_over (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 200, 100);
    touch (&fx, MOVE, 1, 300, 100);
    g_assert_cmpuint (fx.reports->len, ==, 1);
    uc_pinch_cancel (&fx.pinch);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    assert_report (&fx, 1, UC_PINCH_CANCEL, 1.0, 200, 100);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    /* the forgotten fingers' further events go on to WebKit's controller, which was cancelled as well */
    g_assert_cmpint (touch (&fx, MOVE, 1, 350, 100), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (touch (&fx, UP, 1, 350, 100), ==, UC_PINCH_FORWARD);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    /* a cancel without a pinch reports nothing */
    touch (&fx, DOWN, 3, 10, 10);
    uc_pinch_cancel (&fx.pinch);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    fixture_clear (&fx);
}

/* a navigation: a running pinch ends with CANCEL, the fingers still down stay away from WebKit's controller */
static void
test_abort (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 200, 100);
    touch (&fx, MOVE, 1, 300, 100);
    uc_pinch_abort (&fx.pinch);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    assert_report (&fx, 1, UC_PINCH_CANCEL, 1.0, 200, 100);
    g_assert_cmpint (touch (&fx, MOVE, 1, 400, 100), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    touch (&fx, UP, 1, 400, 100);
    g_assert_cmpint (touch (&fx, UP, 0, 100, 100), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    /* one finger is left alone */
    touch (&fx, DOWN, 0, 100, 100);
    uc_pinch_abort (&fx.pinch);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_ONE);
    g_assert_cmpint (touch (&fx, UP, 0, 100, 100), ==, UC_PINCH_FORWARD);
    /* two fingers before the threshold: nothing reported, still suppressed until up */
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 150, 100);
    uc_pinch_abort (&fx.pinch);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    g_assert_cmpint (touch (&fx, MOVE, 1, 300, 100), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    fixture_clear (&fx);
}

/* fingers very close together: the scale never divides by zero */
static void
test_min_span (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 140, 100);                 /* span 40 */
    touch (&fx, MOVE, 1, 100.2, 100);               /* span 0.2: -39.8, begins at the minimum span */
    g_assert_cmpuint (fx.reports->len, ==, 1);
    touch (&fx, MOVE, 1, 101, 100);                 /* span 1 */
    assert_report (&fx, 1, UC_PINCH_UPDATE, 1.0, 100.5, 100);
    touch (&fx, MOVE, 1, 150, 100);
    assert_report (&fx, 2, UC_PINCH_UPDATE, 50.0, 125, 100);
    fixture_clear (&fx);
}

/* detached (the view went): the tracker goes on, nothing is reported */
static void
test_detach (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 200, 100);
    uc_pinch_detach (&fx.pinch);
    g_assert_cmpint (touch (&fx, MOVE, 1, 300, 100), ==, UC_PINCH_SUPPRESS);
    touch (&fx, UP, 1, 300, 100);
    uc_pinch_cancel (&fx.pinch);
    g_assert_cmpuint (fx.reports->len, ==, 0);
    fixture_clear (&fx);
}

/* more fingers than the tracker remembers */
static void
test_many_fingers (void)
{
    Fixture fx;
    fixture_init (&fx);
    for (guint32 id = 0; id < UC_PINCH_MAX_POINTS; id++)
        touch (&fx, DOWN, id, id * 10.0, 0);
    g_assert_cmpint (touch (&fx, DOWN, 99, 500, 500), ==, UC_PINCH_SUPPRESS);
    g_assert_cmpint (touch (&fx, MOVE, 99, 500, 600), ==, UC_PINCH_SUPPRESS);
    for (guint32 id = 0; id < UC_PINCH_MAX_POINTS; id++)
        touch (&fx, UP, id, id * 10.0, 0);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    /* a press of the same id again (its release was lost) keeps one finger */
    touch (&fx, DOWN, 0, 1, 1);
    g_assert_cmpint (touch (&fx, DOWN, 0, 2, 2), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_ONE);
    fixture_clear (&fx);
}

/* a press of a finger the tracker still holds (its release was lost) starts afresh instead of wedging it */
static void
test_lost_release (void)
{
    Fixture fx;
    fixture_init (&fx);
    touch (&fx, DOWN, 0, 100, 100);
    touch (&fx, DOWN, 1, 200, 100);
    touch (&fx, MOVE, 1, 300, 100);                 /* BEGIN */
    /* the releases never come; the next press reuses id 0 */
    g_assert_cmpint (touch (&fx, DOWN, 0, 50, 50), ==, UC_PINCH_FORWARD);
    g_assert_cmpuint (fx.reports->len, ==, 2);
    assert_report (&fx, 1, UC_PINCH_CANCEL, 1.0, 200, 100);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_ONE);
    g_assert_cmpint (touch (&fx, UP, 0, 50, 50), ==, UC_PINCH_FORWARD);
    g_assert_cmpint (uc_pinch_state (&fx.pinch), ==, UC_PINCH_STATE_IDLE);
    fixture_clear (&fx);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/pinch/event-values", test_event_values);
    g_test_add_func ("/pinch/one-finger", test_one_finger);
    g_test_add_func ("/pinch/pinch-out", test_pinch_out);
    g_test_add_func ("/pinch/pinch-in", test_pinch_in);
    g_test_add_func ("/pinch/threshold", test_threshold);
    g_test_add_func ("/pinch/two-finger-tap", test_two_finger_tap);
    g_test_add_func ("/pinch/third-finger", test_third_finger);
    g_test_add_func ("/pinch/touch-cancel", test_touch_cancel);
    g_test_add_func ("/pinch/page-takes-over", test_page_takes_over);
    g_test_add_func ("/pinch/abort", test_abort);
    g_test_add_func ("/pinch/min-span", test_min_span);
    g_test_add_func ("/pinch/detach", test_detach);
    g_test_add_func ("/pinch/many-fingers", test_many_fingers);
    g_test_add_func ("/pinch/lost-release", test_lost_release);
    return g_test_run ();
}
