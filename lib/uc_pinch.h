/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Two-finger pinch recognition of libucwebview (library 0.6.0). GLib only, so that the rules are unit-tested on a
 * host without WPE (tests/test-pinch.c); uc_wpe_gesture.c feeds it from the view's WPEGestureController.
 *
 * Where the touches come from: WebKit hands a touch event to the view's gesture controller only when the page did not
 * handle it, i.e. no touch listener called preventDefault(), and cancels the controller when the page did
 * (PageClientImpl.cpp:248-266). A page with a pinch of its own (a map, an image viewer) therefore keeps it; the
 * tracker only sees what the page left alone. Every touch event reaches the controller, in order, after the web
 * process answered it (WebPageProxy.cpp:5455-5531, coalesced moves are reported one by one).
 *
 * The rules:
 *  - One finger: the event goes on to WebKit's own controller (tap -> click, drag -> scroll), as before 0.6.0.
 *  - A second finger lands while the first is down: WebKit's controller is cancelled (the first finger's tap or
 *    scroll ends there) and gets nothing more until every finger is up. Two fingers neither tap nor scroll.
 *  - The pinch begins when the distance between the two fingers (the span) has changed by more than the threshold
 *    since the second finger landed (the library's drag threshold, 16 px): BEGIN with scale 1 at the midpoint. The
 *    scale is measured from the span at that moment, so it starts at 1 without a jump (Android's ScaleGestureDetector
 *    does the same); the threshold's distance is not part of the scale.
 *  - Every move of one of the two fingers after that: UPDATE with scale = span / span at BEGIN and the midpoint.
 *  - One of the two fingers lifts: END with the scale and midpoint of the last UPDATE (the lift position does not
 *    change them, so the host's picture and the result agree). A touch cancel of one of them, the page taking the
 *    touches over (uc_pinch_cancel()) or a navigation (uc_pinch_abort()): CANCEL with the last values.
 *  - Two fingers that did not move apart or together far enough (a two-finger tap, a two-finger pan) report nothing.
 *  - After the two fingers, nothing until every finger is up: the remaining finger neither scrolls nor taps, a third
 *    finger is ignored.
 *  - A press of a finger the tracker still holds (its release was lost) starts afresh: CANCEL if a pinch ran, every
 *    finger forgotten, the press is a first finger again.
 */
#ifndef UC_PINCH_H
#define UC_PINCH_H

#include <glib.h>

G_BEGIN_DECLS

/* the values of UC_WEBVIEW_PINCH_* (uc_webview.h, asserted in uc_webview.c) */
typedef enum {
    UC_PINCH_BEGIN = 0,
    UC_PINCH_UPDATE = 1,
    UC_PINCH_END = 2,
    UC_PINCH_CANCEL = 3,
} UcPinchPhase;

typedef enum {
    UC_PINCH_FORWARD,       /* the event goes to WebKit's own gesture controller */
    UC_PINCH_TAKE_OVER,     /* a second finger landed: cancel WebKit's controller now, then like SUPPRESS */
    UC_PINCH_SUPPRESS,      /* not for WebKit's controller: no tap, no scroll */
} UcPinchVerdict;

typedef enum {
    UC_PINCH_STATE_IDLE,        /* no finger down that the tracker follows */
    UC_PINCH_STATE_ONE,         /* one finger, WebKit's controller has it */
    UC_PINCH_STATE_POSSIBLE,    /* two fingers, the span has not changed by the threshold yet */
    UC_PINCH_STATE_PINCHING,    /* BEGIN reported */
    UC_PINCH_STATE_DONE,        /* a two-finger sequence ended: nothing until every finger is up */
} UcPinchState;

/* the touch event types the tracker takes (the values of WPE_EVENT_TOUCH_*, uc_events.h) */
#define UC_PINCH_TOUCH_DOWN   9
#define UC_PINCH_TOUCH_UP     10
#define UC_PINCH_TOUCH_MOVE   11
#define UC_PINCH_TOUCH_CANCEL 12

/* Fingers the tracker remembers; more are ignored (and suppressed while two are down). */
#define UC_PINCH_MAX_POINTS 10

/* The smallest span a scale is measured from, px: two touch points closer than this are one finger anyway. */
#define UC_PINCH_MIN_SPAN 1.0

/* phase is UC_PINCH_*, scale the factor since BEGIN, (x, y) the midpoint of the two fingers in view pixels. */
typedef void (*UcPinchFunc) (UcPinchPhase phase, double scale, double x, double y, gpointer user_data);

typedef struct {
    guint32 id;
    double x, y;
} UcPinchPoint;

typedef struct {
    double threshold;
    UcPinchFunc func;
    gpointer user_data;
    UcPinchState state;
    UcPinchPoint points[UC_PINCH_MAX_POINTS];
    guint n_points;
    guint32 first, second;      /* the two fingers of the pinch (POSSIBLE, PINCHING) */
    double start_span;          /* span when the second finger landed */
    double base_span;           /* span at BEGIN */
    double scale, x, y;         /* the last BEGIN/UPDATE */
} UcPinch;

/* threshold > 0 px; func may be NULL (nothing reported, the touches are still taken over). */
void           uc_pinch_init   (UcPinch *pinch, double threshold, UcPinchFunc func, gpointer user_data);
/* One touch event the page did not handle: type is UC_PINCH_TOUCH_*, id the touch sequence, (x, y) in view pixels.
 * Reports BEGIN/UPDATE/END/CANCEL through func from inside the call. Other types are forwarded untouched. */
UcPinchVerdict uc_pinch_touch  (UcPinch *pinch, int type, guint32 id, double x, double y);
/* The page handled a touch event (WebKit cancels the gesture controller): a running pinch ends with CANCEL, every
 * finger is forgotten; the next press starts afresh. */
void           uc_pinch_cancel (UcPinch *pinch);
/* A navigation committed: a running pinch ends with CANCEL; the fingers still down get nothing until they are up. */
void           uc_pinch_abort  (UcPinch *pinch);
/* Stops reporting (the view goes away); the tracker keeps working without callbacks. */
void           uc_pinch_detach (UcPinch *pinch);
UcPinchState   uc_pinch_state  (const UcPinch *pinch);

G_END_DECLS

#endif /* UC_PINCH_H */
