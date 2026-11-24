/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Frame pacing of a view: which of the library's frames goes to the engine when, and when the library hears that a
 * frame was rendered (uc_webview_frame_rendered(), which lets WebKit composite its next frame). Plus the frame clock
 * behind the pacing figures of the "stats" reply.
 *
 * Why a frame is reported before it is on screen
 * ==============================================
 * WebKit composites one frame at a time: the next only after the host reported the previous one rendered
 * (uc_webview.h, "Frame lifecycle"). A host whose frames start on the display's vblanks (flutter-pi with a vsync
 * callback that answers with the next vblank) shows a web frame one refresh after the vblank of the first engine
 * frame that fetches it, and that engine frame only exists if the web frame arrived, and the engine asked for a
 * frame, before that vblank. So each web frame has from its report until the next vblank to be composited by WebKit,
 * sent, imported by the library and handed to the engine; one that is late leaves a refresh without a new frame.
 *
 *  - UCFL_PACING_FETCH (the default) reports a frame when the engine fetches it for a frame, which is shortly after
 *    a vblank: the next web frame has about one refresh period minus the fetch delay, and a frame that takes longer
 *    leaves a refresh without a new frame. A report after the page flip that shows the frame would come at the very
 *    vblank the next engine frame has to be asked for before, so every web frame would take two refresh periods: half
 *    the panel rate.
 *  - UCFL_PACING_AHEAD reports a frame when it is handed to the engine, so WebKit composites the next frame while
 *    the engine has yet to fetch this one, and holds that next frame until this one was fetched (one frame waits,
 *    never more: WebKit is back-pressured by the frame it waits for). Each web frame has about two refresh periods,
 *    every vblank gets a new frame while the page draws at least the panel rate, and WebKit still composites exactly
 *    one frame per fetch, so a page that draws on every requestAnimationFrame stays locked to the display. The cost is
 *    one refresh period of latency while frames stream (an idle page's first frame goes to the engine at once) and
 *    one more buffer held (three: the one the engine shows, the one handed to it, the one waiting; WebKit's swap
 *    chain has four).
 *
 * FETCH is the default: a WebKit turnaround well below a refresh period (a few milliseconds against 16 to 21 ms) gets
 * every refresh a new frame in both modes, and AHEAD then only adds its refresh period of latency. AHEAD
 * (UC_WEBVIEW_FRAME_PACING=ahead) is for pages whose turnaround approaches a refresh period or varies around it.
 *
 * Thread: the adapter's platform thread only (no locking).
 */
#ifndef UCFL_PACING_H
#define UCFL_PACING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ucfl_pacing_mode {
    UCFL_PACING_FETCH = 0, /* the default */
    UCFL_PACING_AHEAD = 1,
};

/* The mode named by value ("fetch", "ahead"); NULL or "" is the default. Returns 0, or -1 for another value (mode
 * then is the default). */
int ucfl_pacing_parse(const char *value, enum ucfl_pacing_mode *mode);
/* "fetch", "ahead" */
const char *ucfl_pacing_name(enum ucfl_pacing_mode mode);

/* The pacing state of one view; frames are the adapter's opaque pointers. */
struct ucfl_pacer {
    enum ucfl_pacing_mode mode;
    void *pushed; /* handed to the engine and not fetched yet, or NULL */
    void *held;   /* UCFL_PACING_AHEAD: waits until `pushed` is fetched, or NULL */
};

/* What the adapter does next, in this order: release (without showing it), push (hand to the engine), rendered
 * (report). NULL members: nothing of that. A push that fails is followed by ucfl_pacer_gone() and a release. */
struct ucfl_pace {
    void *release;
    void *push;
    void *rendered;
};

void ucfl_pacer_init(struct ucfl_pacer *pacer, enum ucfl_pacing_mode mode);
/* The library delivered frame. */
struct ucfl_pace ucfl_pacer_ready(struct ucfl_pacer *pacer, void *frame);
/* The engine fetched frame (the pushed one or an older one). */
struct ucfl_pace ucfl_pacer_fetched(struct ucfl_pacer *pacer, void *frame);
/* frame left the adapter: released after the engine dropped it, dropped unfetched, or its push failed. */
void ucfl_pacer_gone(struct ucfl_pacer *pacer, void *frame);
/* The waiting frame, which the adapter releases (the view goes away); NULL without one. */
void *ucfl_pacer_take_held(struct ucfl_pacer *pacer);

/* ---- frame clock: the figures of the "stats" reply ---------------------------------------------------------------- */

#define UCFL_CLOCK_RING   128           /* fetch times kept: more than a second at 120 Hz */
#define UCFL_CLOCK_WINDOW 1000000000LL  /* ns over which the fetch rate is measured */
#define UCFL_CLOCK_SPAN   250000000LL   /* ns: longer delays are pauses of the page, not part of the figures */

/* A delay's figures since the last ucfl_delay_take(). */
struct ucfl_delay {
    int64_t max_ns, sum_ns;
    uint64_t count;
};

struct ucfl_frame_clock {
    int64_t period_ns;              /* the display's refresh period, > 0 */
    int64_t fetch_ns[UCFL_CLOCK_RING];
    size_t n_fetches, next;         /* entries used (up to the ring size), next slot to write */
    uint64_t gaps;                  /* fetch intervals of 1.5 to 4.5 refresh periods (refreshes without a new frame) */
    struct ucfl_delay turnaround;   /* frame reported rendered -> the next frame delivered (WebKit's response) */
    struct ucfl_delay to_fetch;     /* frame handed to the engine -> fetched */
};

/* refresh_hz outside (0, 1000] or NaN: 60 Hz. The rate may be the display mode's nominal one, which can be a few per
 * cent off the rate the panel flips at: the gap bounds are half a period away from whole periods, so a rate up to
 * about 10 % off still counts each missed refresh, and only those. */
void ucfl_frame_clock_init(struct ucfl_frame_clock *clock, double refresh_hz);
/* A frame was fetched at fetch_ns (CLOCK_MONOTONIC); calls in time order. */
void ucfl_frame_clock_fetched(struct ucfl_frame_clock *clock, int64_t fetch_ns);
/* Fetches per second (times 1000) over the fetches of the last UCFL_CLOCK_WINDOW before now_ns: the intervals between
 * them divided by their span. 0 with fewer than two. */
int64_t ucfl_frame_clock_milli_fps(const struct ucfl_frame_clock *clock, int64_t now_ns);
/* Adds a delay of from_ns -> to_ns unless from_ns is 0 or it is negative or longer than UCFL_CLOCK_SPAN. */
void ucfl_delay_add(struct ucfl_delay *delay, int64_t from_ns, int64_t to_ns);
/* Merges src into dst (the figures of several views) and clears src. */
void ucfl_delay_take(struct ucfl_delay *dst, struct ucfl_delay *src);
/* Average in microseconds, 0 without delays. */
int64_t ucfl_delay_avg_us(const struct ucfl_delay *delay);

#endif /* UCFL_PACING_H */
