/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test of core/ucfl_pacing.c: the decisions of both pacing modes over a stream of frames, and the frame
 * clock's rate, gaps and delays. The loop test drives a model of WebKit (one frame at a time, the next after the
 * report plus a turnaround) against a model of flutter-pi's vsync-paced fetches, and checks the frame rate each mode
 * reaches with short, long and jittering turnarounds.
 */
#include "ucfl_pacing.h"

#include <glib.h>

static int frames[16];
#define F(i) ((void *) &frames[i])

static void test_parse(void)
{
    enum ucfl_pacing_mode mode = UCFL_PACING_FETCH;

    g_assert_cmpint(ucfl_pacing_parse(NULL, &mode), ==, 0);
    g_assert_cmpint(mode, ==, UCFL_PACING_AHEAD);
    g_assert_cmpint(ucfl_pacing_parse("fetch", &mode), ==, 0);
    g_assert_cmpint(mode, ==, UCFL_PACING_FETCH);
    g_assert_cmpint(ucfl_pacing_parse("", &mode), ==, 0);
    g_assert_cmpint(mode, ==, UCFL_PACING_AHEAD);
    g_assert_cmpint(ucfl_pacing_parse("ahead", &mode), ==, 0);
    g_assert_cmpint(mode, ==, UCFL_PACING_AHEAD);
    g_assert_cmpint(ucfl_pacing_parse("present", &mode), ==, -1);
    g_assert_cmpint(mode, ==, UCFL_PACING_AHEAD);
    g_assert_cmpstr(ucfl_pacing_name(UCFL_PACING_FETCH), ==, "fetch");
    g_assert_cmpstr(ucfl_pacing_name(UCFL_PACING_AHEAD), ==, "ahead");
}

static void test_fetch_mode(void)
{
    struct ucfl_pacer pacer;
    struct ucfl_pace pace;

    ucfl_pacer_init(&pacer, UCFL_PACING_FETCH);
    pace = ucfl_pacer_ready(&pacer, F(0));
    g_assert_true(pace.push == F(0) && !pace.rendered && !pace.release);
    /* a newer frame replaces the unfetched one in the texture, which drops it */
    pace = ucfl_pacer_ready(&pacer, F(1));
    g_assert_true(pace.push == F(1) && !pace.rendered && !pace.release);
    ucfl_pacer_gone(&pacer, F(0));
    g_assert_true(pacer.pushed == F(1));
    pace = ucfl_pacer_fetched(&pacer, F(1));
    g_assert_true(!pace.push && pace.rendered == F(1) && !pace.release);
    g_assert_null(pacer.pushed);
    g_assert_null(ucfl_pacer_take_held(&pacer));
}

static void test_ahead_mode(void)
{
    struct ucfl_pacer pacer;
    struct ucfl_pace pace;

    ucfl_pacer_init(&pacer, UCFL_PACING_AHEAD);
    /* nothing pushed: at once, reported at once */
    pace = ucfl_pacer_ready(&pacer, F(0));
    g_assert_true(pace.push == F(0) && pace.rendered == F(0) && !pace.release);
    /* the next one waits for the fetch of the pushed one, unreported */
    pace = ucfl_pacer_ready(&pacer, F(1));
    g_assert_true(!pace.push && !pace.rendered && !pace.release);
    g_assert_true(pacer.held == F(1));
    /* the fetch hands it over and reports it */
    pace = ucfl_pacer_fetched(&pacer, F(0));
    g_assert_true(pace.push == F(1) && pace.rendered == F(1) && !pace.release);
    g_assert_true(pacer.pushed == F(1) && !pacer.held);
    /* the engine drops the old one: nothing to do */
    ucfl_pacer_gone(&pacer, F(0));
    g_assert_true(pacer.pushed == F(1));
    /* a forced second frame while one waits: the newer one waits, the older one is released unshown */
    pace = ucfl_pacer_ready(&pacer, F(2));
    g_assert_true(!pace.push && !pace.rendered && !pace.release);
    pace = ucfl_pacer_ready(&pacer, F(3));
    g_assert_true(!pace.push && !pace.rendered && pace.release == F(2));
    g_assert_true(pacer.held == F(3));
    /* a fetch of an older frame does not free the slot */
    pace = ucfl_pacer_fetched(&pacer, F(0));
    g_assert_true(!pace.push && !pace.rendered && !pace.release);
    pace = ucfl_pacer_fetched(&pacer, F(1));
    g_assert_true(pace.push == F(3) && pace.rendered == F(3));
    /* fetched with nothing waiting: the slot is free, the next frame goes at once */
    pace = ucfl_pacer_fetched(&pacer, F(3));
    g_assert_true(!pace.push && !pace.rendered && !pace.release);
    pace = ucfl_pacer_ready(&pacer, F(4));
    g_assert_true(pace.push == F(4) && pace.rendered == F(4));
    /* a failed push: gone, the slot is free again */
    ucfl_pacer_gone(&pacer, F(4));
    g_assert_null(pacer.pushed);
    pace = ucfl_pacer_ready(&pacer, F(5));
    g_assert_true(pace.push == F(5));
    pace = ucfl_pacer_ready(&pacer, F(6));
    g_assert_true(ucfl_pacer_take_held(&pacer) == F(6));
    g_assert_null(pacer.held);
}

/*
 * The loop: flutter-pi answers a frame request with the next vblank at least 3/4 period after the last frame start
 * (or a quarter period from now after two idle periods), and the engine fetches the texture `fetch_delay` after the
 * start. WebKit delivers its next frame a turnaround after the report (once per report), alternating between
 * `turnaround` and `turnaround2`. Returns the fetch rate in mHz over `seconds`.
 */
static int64_t run_loop(enum ucfl_pacing_mode mode, int64_t period, int64_t fetch_delay, int64_t turnaround,
                        int64_t turnaround2, int seconds)
{
    struct ucfl_pacer pacer;
    struct ucfl_frame_clock clock;
    int64_t t_end = (int64_t) seconds * 1000000000LL, vblank0 = 1000;
    int64_t next_ready = 0;       /* when WebKit's next frame arrives, 0: none in progress */
    int64_t start = -1;           /* the engine frame that will fetch, -1: none requested */
    int64_t last_start = -1;
    int64_t first_fetch = 0, last_fetch = 0;
    int next_frame = 1, fetches = 0, reports = 0;
    struct ucfl_pace pace;

    ucfl_pacer_init(&pacer, mode);
    ucfl_frame_clock_init(&clock, 1e9 / (double) period);

    /* frame 0 arrives at t = 0 */
    pace = ucfl_pacer_ready(&pacer, F(0));
    for (int64_t now = 0;;) {
        /* apply: a push requests an engine frame, a report starts WebKit on the next one */
        if (pace.push && start < 0) {
            if (last_start < 0 || now >= last_start + 2 * period) {
                start = now + period / 4;
            } else {
                int64_t earliest = last_start + period - period / 4;
                if (earliest < now)
                    earliest = now;
                start = vblank0 + ((earliest - vblank0) / period + 1) * period;
            }
        }
        if (pace.rendered)
            next_ready = now + (reports++ % 2 ? turnaround2 : turnaround);
        pace = (struct ucfl_pace) { 0 };

        /* the next event */
        int64_t fetch_at = start >= 0 ? start + fetch_delay : -1;
        if (next_ready && (fetch_at < 0 || next_ready <= fetch_at)) {
            now = next_ready;
            next_ready = 0;
            pace = ucfl_pacer_ready(&pacer, F(next_frame));
            next_frame = (next_frame + 1) % 16;
            if (mode == UCFL_PACING_FETCH)
                g_assert_null(pace.rendered);
        } else if (fetch_at >= 0) {
            now = fetch_at;
            if (now > t_end)
                break;
            last_start = start;
            start = -1;
            void *fetched = pacer.pushed;
            g_assert_nonnull(fetched);
            ucfl_frame_clock_fetched(&clock, now);
            if (!fetches++)
                first_fetch = now;
            last_fetch = now;
            pace = ucfl_pacer_fetched(&pacer, fetched);
        } else {
            g_assert_not_reached();
        }
    }
    g_assert_cmpint(fetches, >, 2);
    /* the clock agrees with the count over its window */
    int64_t clock_fps = ucfl_frame_clock_milli_fps(&clock, last_fetch);
    int64_t fps = (int64_t) (fetches - 1) * 1000 * 1000000000LL / (last_fetch - first_fetch);
    g_assert_cmpint(clock_fps, >=, fps - 2000);
    g_assert_cmpint(clock_fps, <=, fps + 2000);
    g_test_message("%s, turnaround %lld/%lld us: %lld mHz", ucfl_pacing_name(mode), (long long) turnaround / 1000,
                   (long long) turnaround2 / 1000, (long long) fps);
    return fps;
}

static void test_loop(void)
{
    const int64_t period = 20148000; /* 49.632 Hz */
    const int64_t hz = 49632;
    int64_t fps;

    /* WebKit answers within the period minus the fetch delay: both modes reach the panel rate */
    fps = run_loop(UCFL_PACING_FETCH, period, 3000000, 12000000, 12000000, 10);
    g_assert_cmpint(fps, >=, hz - 100);
    fps = run_loop(UCFL_PACING_AHEAD, period, 3000000, 12000000, 12000000, 10);
    g_assert_cmpint(fps, >=, hz - 100);
    /* a report at the page flip that shows the frame instead of the fetch (fetch plus the rest of the period) leaves
     * WebKit the same 12 ms but halves the rate (ucfl_pacing.h) */
    fps = run_loop(UCFL_PACING_FETCH, period, 3000000, 12000000 + period - 3000000, 12000000 + period - 3000000, 10);
    g_assert_cmpint(fps, <=, hz / 2 + 100);
    /* a turnaround a bit longer than the period minus the fetch delay: "fetch" falls to half the rate, "ahead" keeps
     * the panel rate */
    fps = run_loop(UCFL_PACING_FETCH, period, 3000000, 18000000, 18000000, 10);
    g_assert_cmpint(fps, <=, hz / 2 + 100);
    fps = run_loop(UCFL_PACING_AHEAD, period, 3000000, 18000000, 18000000, 10);
    g_assert_cmpint(fps, >=, hz - 100);
    /* jitter: every second turnaround longer than a period, the average shorter: "fetch" loses a refresh on each long
     * one, "ahead" absorbs it */
    fps = run_loop(UCFL_PACING_FETCH, period, 3000000, 8000000, 24000000, 10);
    g_assert_cmpint(fps, <=, hz * 3 / 4);
    fps = run_loop(UCFL_PACING_AHEAD, period, 3000000, 8000000, 24000000, 10);
    g_assert_cmpint(fps, >=, hz - 100);
    /* longer than a period on average: neither keeps up, "ahead" is not slower */
    g_assert_cmpint(run_loop(UCFL_PACING_AHEAD, period, 3000000, 30000000, 30000000, 10), >=,
                    run_loop(UCFL_PACING_FETCH, period, 3000000, 30000000, 30000000, 10));
}

static void test_clock(void)
{
    struct ucfl_frame_clock clock;
    struct ucfl_delay sum = { 0 };
    const int64_t period = 20000000;

    ucfl_frame_clock_init(&clock, 50.0);
    g_assert_cmpint(clock.period_ns, ==, period);
    g_assert_cmpint(ucfl_frame_clock_milli_fps(&clock, 0), ==, 0);
    /* 3 s at 50 Hz with one refresh skipped and one pause of the page */
    int64_t t = 1000000000;
    for (int i = 0; i < 150; i++) {
        t += period;
        if (i == 60)
            t += period; /* a gap */
        if (i == 80)
            t += 10 * period; /* a pause, not a gap */
        ucfl_frame_clock_fetched(&clock, t);
    }
    g_assert_cmpuint(clock.gaps, ==, 1);
    g_assert_cmpuint(clock.n_fetches, ==, UCFL_CLOCK_RING);
    g_assert_cmpint(ucfl_frame_clock_milli_fps(&clock, t), ==, 50000);
    g_assert_cmpint(ucfl_frame_clock_milli_fps(&clock, t + 2 * UCFL_CLOCK_WINDOW), ==, 0);

    ucfl_frame_clock_init(&clock, 0.0);
    g_assert_cmpint(clock.period_ns, ==, 16666667);
    ucfl_frame_clock_init(&clock, 1.0 / 0.0);
    g_assert_cmpint(clock.period_ns, ==, 16666667);

    ucfl_delay_add(&clock.turnaround, 0, 5000000);          /* no start: ignored */
    ucfl_delay_add(&clock.turnaround, 100, 50);             /* negative: ignored */
    ucfl_delay_add(&clock.turnaround, 1, 1 + UCFL_CLOCK_SPAN + 1); /* a pause: ignored */
    ucfl_delay_add(&clock.turnaround, 1000, 1000 + 4000000);
    ucfl_delay_add(&clock.turnaround, 1000, 1000 + 8000000);
    g_assert_cmpint(ucfl_delay_avg_us(&clock.turnaround), ==, 6000);
    ucfl_delay_take(&sum, &clock.turnaround);
    g_assert_cmpuint(clock.turnaround.count, ==, 0);
    g_assert_cmpuint(sum.count, ==, 2);
    g_assert_cmpint(sum.max_ns, ==, 8000000);
    g_assert_cmpint(ucfl_delay_avg_us(&sum), ==, 6000);
    g_assert_cmpint(ucfl_delay_avg_us(&clock.turnaround), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pacing/parse", test_parse);
    g_test_add_func("/pacing/fetch", test_fetch_mode);
    g_test_add_func("/pacing/ahead", test_ahead_mode);
    g_test_add_func("/pacing/loop", test_loop);
    g_test_add_func("/pacing/clock", test_clock);
    return g_test_run();
}
