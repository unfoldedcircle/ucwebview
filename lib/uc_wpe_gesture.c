/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * UcGestureController: the view's WPEGestureController when the host asked for pinch reports (library 0.6.0). It
 * wraps WebKit's default controller, which keeps doing taps and scrolls for one finger, and runs the pinch tracker
 * (uc_pinch.h) next to it.
 *
 * WebKit's side (WPE 2.54.1):
 *  - wpe_view_get_gesture_controller() creates the default controller on first use, wpe_view_set_gesture_controller()
 *    replaces it and keeps a reference of its own (WPEView.cpp:1165-1188, a GRefPtr). The wrapper takes a reference on
 *    the default one before it replaces it.
 *  - A touch event the page did not handle goes to handle_event(); one the page handled cancels the controller
 *    (PageClientImpl.cpp:248-266). For handle_event() == TRUE WebKit asks get_gesture() and the position, delta and
 *    drag begin, and turns TAP into a synthesized click and DRAG into a scroll (WPEWebViewPlatform.cpp:433-497); for
 *    FALSE it does nothing. The default controller follows the first touch sequence only and answers FALSE for the
 *    others (WPEGestureDetector.cpp:35-38).
 * So: FORWARD hands the event to the default controller and returns its answer; TAKE_OVER cancels the default
 * controller (no click at the first finger's release, no further scroll) and returns FALSE; SUPPRESS returns FALSE.
 * cancel() cancels both.
 */
#include "uc_internal.h"
#include "uc_pinch.h"

struct _UcGestureController {
    GObject parent;
    WPEGestureController *inner;    /* WebKit's default controller, a reference */
    UcPinch pinch;
    gboolean suppressing;           /* the last event was not the inner controller's */
};

static void uc_gesture_controller_iface_init (WPEGestureControllerInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (UcGestureController, uc_gesture_controller, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (WPE_TYPE_GESTURE_CONTROLLER, uc_gesture_controller_iface_init))

static gboolean
uc_gesture_controller_handle_event (WPEGestureController *controller, WPEEvent *event)
{
    UcGestureController *self = UC_GESTURE_CONTROLLER (controller);
    WPEEventType type = wpe_event_get_event_type (event);
    double x = 0, y = 0;

    switch (type) {
    case WPE_EVENT_TOUCH_DOWN:
    case WPE_EVENT_TOUCH_UP:
    case WPE_EVENT_TOUCH_MOVE:
    case WPE_EVENT_TOUCH_CANCEL:
        break;
    default:
        /* WebKit hands only touch events to the controller (PageClientImpl.cpp:262-266) */
        return wpe_gesture_controller_handle_event (self->inner, event);
    }
    wpe_event_get_position (event, &x, &y);
    switch (uc_pinch_touch (&self->pinch, (int) type, wpe_event_touch_get_sequence_id (event), x, y)) {
    case UC_PINCH_TAKE_OVER:
        wpe_gesture_controller_cancel (self->inner);
        self->suppressing = TRUE;
        return FALSE;
    case UC_PINCH_SUPPRESS:
        self->suppressing = TRUE;
        return FALSE;
    case UC_PINCH_FORWARD:
    default:
        self->suppressing = FALSE;
        return wpe_gesture_controller_handle_event (self->inner, event);
    }
}

static void
uc_gesture_controller_cancel (WPEGestureController *controller)
{
    UcGestureController *self = UC_GESTURE_CONTROLLER (controller);

    self->suppressing = FALSE;
    wpe_gesture_controller_cancel (self->inner);
    uc_pinch_cancel (&self->pinch);
}

/* WebKit asks only after handle_event() returned TRUE; the checks keep a stale gesture out all the same */
static WPEGesture
uc_gesture_controller_get_gesture (WPEGestureController *controller)
{
    UcGestureController *self = UC_GESTURE_CONTROLLER (controller);
    return self->suppressing ? WPE_GESTURE_NONE : wpe_gesture_controller_get_gesture (self->inner);
}

static gboolean
uc_gesture_controller_get_gesture_position (WPEGestureController *controller, double *x, double *y)
{
    UcGestureController *self = UC_GESTURE_CONTROLLER (controller);
    return !self->suppressing && wpe_gesture_controller_get_gesture_position (self->inner, x, y);
}

static gboolean
uc_gesture_controller_get_gesture_delta (WPEGestureController *controller, double *x, double *y)
{
    UcGestureController *self = UC_GESTURE_CONTROLLER (controller);
    return !self->suppressing && wpe_gesture_controller_get_gesture_delta (self->inner, x, y);
}

static gboolean
uc_gesture_controller_is_drag_begin (WPEGestureController *controller)
{
    UcGestureController *self = UC_GESTURE_CONTROLLER (controller);
    return !self->suppressing && wpe_gesture_controller_is_drag_begin (self->inner);
}

static void
uc_gesture_controller_iface_init (WPEGestureControllerInterface *iface)
{
    iface->handle_event = uc_gesture_controller_handle_event;
    iface->cancel = uc_gesture_controller_cancel;
    iface->get_gesture = uc_gesture_controller_get_gesture;
    iface->get_gesture_position = uc_gesture_controller_get_gesture_position;
    iface->get_gesture_delta = uc_gesture_controller_get_gesture_delta;
    iface->is_drag_begin = uc_gesture_controller_is_drag_begin;
}

static void
uc_gesture_controller_dispose (GObject *object)
{
    UcGestureController *self = UC_GESTURE_CONTROLLER (object);
    uc_pinch_detach (&self->pinch);
    g_clear_object (&self->inner);
    G_OBJECT_CLASS (uc_gesture_controller_parent_class)->dispose (object);
}

static void
uc_gesture_controller_class_init (UcGestureControllerClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = uc_gesture_controller_dispose;
}

static void
uc_gesture_controller_init (UcGestureController *self)
{
    self->inner = NULL;
    self->suppressing = FALSE;
    uc_pinch_init (&self->pinch, 1.0, NULL, NULL);
}

UcGestureController *
uc_gesture_controller_install (WPEView *view, double threshold, UcPinchFunc func, gpointer user_data)
{
    UcGestureController *self;
    WPEGestureController *inner = wpe_view_get_gesture_controller (view);   /* creates WebKit's default one */

    if (inner == NULL)
        return NULL;
    self = g_object_new (UC_TYPE_GESTURE_CONTROLLER, NULL);
    self->inner = g_object_ref (inner);
    uc_pinch_init (&self->pinch, threshold, func, user_data);
    wpe_view_set_gesture_controller (view, WPE_GESTURE_CONTROLLER (self));  /* the view keeps its own reference */
    return self;
}

void
uc_gesture_controller_abort (UcGestureController *controller)
{
    uc_pinch_abort (&controller->pinch);
}

void
uc_gesture_controller_detach (UcGestureController *controller)
{
    uc_pinch_detach (&controller->pinch);
}
