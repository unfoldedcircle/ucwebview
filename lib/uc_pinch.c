/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Two-finger pinch recognition, see uc_pinch.h for the rules.
 */
#include "uc_pinch.h"

#include <string.h>

/* sqrt without libm: the library links no libm, and its NEEDED list stays as it is (README, device checks).
 * Newton's method from a start at or above the root decreases monotonically to it; v >= 0. */
static double
span_sqrt (double v)
{
    double r;

    if (!(v > 0))
        return 0;
    r = v >= 1 ? v : 1;
    for (int i = 0; i < 128; i++) {
        double next = 0.5 * (r + v / r);
        if (!(next < r))
            break;
        r = next;
    }
    return r;
}

static double
abs_value (double v)
{
    return v < 0 ? -v : v;
}

void
uc_pinch_init (UcPinch *pinch, double threshold, UcPinchFunc func, gpointer user_data)
{
    memset (pinch, 0, sizeof *pinch);
    pinch->threshold = threshold > 0 ? threshold : 1.0;
    pinch->func = func;
    pinch->user_data = user_data;
    pinch->state = UC_PINCH_STATE_IDLE;
    pinch->scale = 1.0;
}

UcPinchState
uc_pinch_state (const UcPinch *pinch)
{
    return pinch->state;
}

void
uc_pinch_detach (UcPinch *pinch)
{
    pinch->func = NULL;
    pinch->user_data = NULL;
}

static void
report (UcPinch *pinch, UcPinchPhase phase)
{
    if (pinch->func != NULL)
        pinch->func (phase, pinch->scale, pinch->x, pinch->y, pinch->user_data);
}

static UcPinchPoint *
find_point (UcPinch *pinch, guint32 id)
{
    for (guint i = 0; i < pinch->n_points; i++)
        if (pinch->points[i].id == id)
            return &pinch->points[i];
    return NULL;
}

static void
remove_point (UcPinch *pinch, guint32 id)
{
    for (guint i = 0; i < pinch->n_points; i++) {
        if (pinch->points[i].id == id) {
            pinch->points[i] = pinch->points[pinch->n_points - 1];
            pinch->n_points--;
            return;
        }
    }
}

/* span and midpoint of the two pinch fingers; FALSE when one of them is not down */
static gboolean
measure (UcPinch *pinch, double *span, double *x, double *y)
{
    const UcPinchPoint *a = find_point (pinch, pinch->first);
    const UcPinchPoint *b = find_point (pinch, pinch->second);

    if (a == NULL || b == NULL)
        return FALSE;
    *span = span_sqrt ((b->x - a->x) * (b->x - a->x) + (b->y - a->y) * (b->y - a->y));
    *x = (a->x + b->x) / 2.0;
    *y = (a->y + b->y) / 2.0;
    return TRUE;
}

void uc_pinch_cancel (UcPinch *pinch);

static gboolean
two_finger_state (const UcPinch *pinch)
{
    return pinch->state == UC_PINCH_STATE_POSSIBLE || pinch->state == UC_PINCH_STATE_PINCHING
           || pinch->state == UC_PINCH_STATE_DONE;
}

static UcPinchVerdict
touch_down (UcPinch *pinch, guint32 id, double x, double y)
{
    UcPinchPoint *point = find_point (pinch, id);

    if (point != NULL && two_finger_state (pinch)) {
        /* a press of a finger the tracker still holds: its release never came (the web process went away with the
         * touches in flight). Start afresh with this finger as the first one, as WebKit's controller does on a press
         * of its sequence (WPEGestureDetector.cpp:49-56). */
        uc_pinch_cancel (pinch);
        point = NULL;
    }
    if (point == NULL) {
        if (pinch->n_points == UC_PINCH_MAX_POINTS)
            return two_finger_state (pinch) ? UC_PINCH_SUPPRESS : UC_PINCH_FORWARD;
        point = &pinch->points[pinch->n_points++];
        point->id = id;
    }
    point->x = x;
    point->y = y;

    switch (pinch->state) {
    case UC_PINCH_STATE_IDLE:
        pinch->state = UC_PINCH_STATE_ONE;
        pinch->first = id;
        return UC_PINCH_FORWARD;
    case UC_PINCH_STATE_ONE:
        if (id == pinch->first)
            return UC_PINCH_FORWARD;    /* the same finger pressed again: WebKit's controller restarts with it */
        pinch->state = UC_PINCH_STATE_POSSIBLE;
        pinch->second = id;
        {
            double span, mx, my;
            measure (pinch, &span, &mx, &my);
            pinch->start_span = span;
            pinch->x = mx;
            pinch->y = my;
        }
        return UC_PINCH_TAKE_OVER;
    case UC_PINCH_STATE_POSSIBLE:
    case UC_PINCH_STATE_PINCHING:
    case UC_PINCH_STATE_DONE:
    default:
        return UC_PINCH_SUPPRESS;
    }
}

static UcPinchVerdict
touch_move (UcPinch *pinch, guint32 id, double x, double y)
{
    UcPinchPoint *point = find_point (pinch, id);
    double span, mx, my;

    if (point == NULL)
        return two_finger_state (pinch) ? UC_PINCH_SUPPRESS : UC_PINCH_FORWARD;
    point->x = x;
    point->y = y;

    switch (pinch->state) {
    case UC_PINCH_STATE_ONE:
    case UC_PINCH_STATE_IDLE:
        return UC_PINCH_FORWARD;
    case UC_PINCH_STATE_POSSIBLE:
        if ((id == pinch->first || id == pinch->second) && measure (pinch, &span, &mx, &my)
            && abs_value (span - pinch->start_span) > pinch->threshold) {
            pinch->state = UC_PINCH_STATE_PINCHING;
            pinch->base_span = MAX (span, UC_PINCH_MIN_SPAN);
            pinch->scale = 1.0;
            pinch->x = mx;
            pinch->y = my;
            report (pinch, UC_PINCH_BEGIN);
        }
        return UC_PINCH_SUPPRESS;
    case UC_PINCH_STATE_PINCHING:
        if ((id == pinch->first || id == pinch->second) && measure (pinch, &span, &mx, &my)) {
            pinch->scale = MAX (span, UC_PINCH_MIN_SPAN) / pinch->base_span;
            pinch->x = mx;
            pinch->y = my;
            report (pinch, UC_PINCH_UPDATE);
        }
        return UC_PINCH_SUPPRESS;
    case UC_PINCH_STATE_DONE:
    default:
        return UC_PINCH_SUPPRESS;
    }
}

static UcPinchVerdict
touch_end (UcPinch *pinch, guint32 id, gboolean cancelled)
{
    UcPinchVerdict verdict;
    gboolean pinch_finger;

    if (find_point (pinch, id) == NULL)
        return two_finger_state (pinch) ? UC_PINCH_SUPPRESS : UC_PINCH_FORWARD;
    remove_point (pinch, id);
    pinch_finger = id == pinch->first || id == pinch->second;

    switch (pinch->state) {
    case UC_PINCH_STATE_ONE:
    case UC_PINCH_STATE_IDLE:
        verdict = UC_PINCH_FORWARD;     /* WebKit's tap or the end of its scroll */
        if (pinch->n_points == 0)
            pinch->state = UC_PINCH_STATE_IDLE;
        return verdict;
    case UC_PINCH_STATE_POSSIBLE:
        if (pinch_finger)
            pinch->state = UC_PINCH_STATE_DONE;     /* two fingers that did not pinch: nothing */
        break;
    case UC_PINCH_STATE_PINCHING:
        if (pinch_finger) {
            pinch->state = UC_PINCH_STATE_DONE;
            report (pinch, cancelled ? UC_PINCH_CANCEL : UC_PINCH_END);
        }
        break;
    case UC_PINCH_STATE_DONE:
    default:
        break;
    }
    if (pinch->n_points == 0)
        pinch->state = UC_PINCH_STATE_IDLE;
    return UC_PINCH_SUPPRESS;
}

UcPinchVerdict
uc_pinch_touch (UcPinch *pinch, int type, guint32 id, double x, double y)
{
    switch (type) {
    case UC_PINCH_TOUCH_DOWN:
        return touch_down (pinch, id, x, y);
    case UC_PINCH_TOUCH_MOVE:
        return touch_move (pinch, id, x, y);
    case UC_PINCH_TOUCH_UP:
        return touch_end (pinch, id, FALSE);
    case UC_PINCH_TOUCH_CANCEL:
        return touch_end (pinch, id, TRUE);
    default:
        return UC_PINCH_FORWARD;
    }
}

void
uc_pinch_cancel (UcPinch *pinch)
{
    gboolean pinching = pinch->state == UC_PINCH_STATE_PINCHING;

    pinch->n_points = 0;
    pinch->state = UC_PINCH_STATE_IDLE;
    if (pinching)
        report (pinch, UC_PINCH_CANCEL);
}

void
uc_pinch_abort (UcPinch *pinch)
{
    switch (pinch->state) {
    case UC_PINCH_STATE_PINCHING:
        pinch->state = UC_PINCH_STATE_DONE;
        report (pinch, UC_PINCH_CANCEL);
        break;
    case UC_PINCH_STATE_POSSIBLE:
        pinch->state = UC_PINCH_STATE_DONE;
        break;
    default:
        break;  /* one finger stays WebKit's, no finger: nothing */
    }
}
