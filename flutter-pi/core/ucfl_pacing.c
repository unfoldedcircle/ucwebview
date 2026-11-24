/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Frame pacing of a view and its frame clock (ucfl_pacing.h).
 */
#include "ucfl_pacing.h"

#include <string.h>

int ucfl_pacing_parse(const char *value, enum ucfl_pacing_mode *mode)
{
    *mode = UCFL_PACING_FETCH;
    if (!value || !*value || strcmp(value, "fetch") == 0)
        return 0;
    if (strcmp(value, "ahead") == 0) {
        *mode = UCFL_PACING_AHEAD;
        return 0;
    }
    return -1;
}

const char *ucfl_pacing_name(enum ucfl_pacing_mode mode)
{
    return mode == UCFL_PACING_FETCH ? "fetch" : "ahead";
}

void ucfl_pacer_init(struct ucfl_pacer *pacer, enum ucfl_pacing_mode mode)
{
    memset(pacer, 0, sizeof(*pacer));
    pacer->mode = mode;
}

/* hand frame to the engine; AHEAD reports it at once */
static struct ucfl_pace push(struct ucfl_pacer *pacer, void *frame)
{
    struct ucfl_pace pace = { .push = frame };

    pacer->pushed = frame;
    if (pacer->mode == UCFL_PACING_AHEAD)
        pace.rendered = frame;
    return pace;
}

struct ucfl_pace ucfl_pacer_ready(struct ucfl_pacer *pacer, void *frame)
{
    struct ucfl_pace pace = { 0 };

    if (pacer->mode == UCFL_PACING_FETCH || !pacer->pushed)
        return push(pacer, frame); /* FETCH: replaces an unfetched frame, which the texture drops */
    /* WebKit sends no frame before the previous one was reported, and the waiting one is not: a second one would be
     * a forced repaint; the newer frame wins */
    pace.release = pacer->held;
    pacer->held = frame;
    return pace;
}

struct ucfl_pace ucfl_pacer_fetched(struct ucfl_pacer *pacer, void *frame)
{
    struct ucfl_pace pace = { 0 };
    void *held;

    if (pacer->mode == UCFL_PACING_FETCH) {
        if (pacer->pushed == frame)
            pacer->pushed = NULL;
        pace.rendered = frame;
        return pace;
    }
    if (pacer->pushed != frame)
        return pace; /* an older frame: the pushed one is still to be fetched */
    pacer->pushed = NULL;
    held = pacer->held;
    pacer->held = NULL;
    return held ? push(pacer, held) : pace;
}

void ucfl_pacer_gone(struct ucfl_pacer *pacer, void *frame)
{
    if (pacer->pushed == frame)
        pacer->pushed = NULL;
    if (pacer->held == frame)
        pacer->held = NULL;
}

void *ucfl_pacer_take_held(struct ucfl_pacer *pacer)
{
    void *held = pacer->held;

    pacer->held = NULL;
    return held;
}

/* ---- frame clock ------------------------------------------------------------------------------------------------- */

void ucfl_frame_clock_init(struct ucfl_frame_clock *clock, double refresh_hz)
{
    memset(clock, 0, sizeof(*clock));
    if (!(refresh_hz > 0.0 && refresh_hz <= 1000.0)) /* NaN and infinity included */
        refresh_hz = 60.0;
    clock->period_ns = (int64_t) (1e9 / refresh_hz + 0.5);
}

void ucfl_frame_clock_fetched(struct ucfl_frame_clock *clock, int64_t fetch_ns)
{
    if (clock->n_fetches) {
        size_t last = (clock->next + UCFL_CLOCK_RING - 1) % UCFL_CLOCK_RING;
        int64_t interval = fetch_ns - clock->fetch_ns[last];

        /* 1.5 to 4.5 periods: one to three refreshes missed, with half a period of margin for a nominal rate */
        if (2 * interval >= 3 * clock->period_ns && 2 * interval <= 9 * clock->period_ns)
            clock->gaps++;
    }
    clock->fetch_ns[clock->next] = fetch_ns;
    clock->next = (clock->next + 1) % UCFL_CLOCK_RING;
    if (clock->n_fetches < UCFL_CLOCK_RING)
        clock->n_fetches++;
}

int64_t ucfl_frame_clock_milli_fps(const struct ucfl_frame_clock *clock, int64_t now_ns)
{
    int64_t newest = 0, oldest = 0;
    size_t n = 0;

    for (size_t i = 1; i <= clock->n_fetches; i++) {
        int64_t t = clock->fetch_ns[(clock->next + UCFL_CLOCK_RING - i) % UCFL_CLOCK_RING];

        if (t <= now_ns - UCFL_CLOCK_WINDOW || t > now_ns)
            break;
        if (n++ == 0)
            newest = t;
        oldest = t;
    }
    if (n < 2 || newest <= oldest)
        return 0;
    return (int64_t) (n - 1) * 1000 * 1000000000LL / (newest - oldest);
}

void ucfl_delay_add(struct ucfl_delay *delay, int64_t from_ns, int64_t to_ns)
{
    int64_t d = to_ns - from_ns;

    if (from_ns == 0 || d < 0 || d > UCFL_CLOCK_SPAN)
        return;
    if (d > delay->max_ns)
        delay->max_ns = d;
    delay->sum_ns += d;
    delay->count++;
}

void ucfl_delay_take(struct ucfl_delay *dst, struct ucfl_delay *src)
{
    if (src->max_ns > dst->max_ns)
        dst->max_ns = src->max_ns;
    dst->sum_ns += src->sum_ns;
    dst->count += src->count;
    memset(src, 0, sizeof(*src));
}

int64_t ucfl_delay_avg_us(const struct ucfl_delay *delay)
{
    return delay->count ? delay->sum_ns / (int64_t) delay->count / 1000 : 0;
}
