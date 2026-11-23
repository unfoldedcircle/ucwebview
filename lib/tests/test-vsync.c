/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_vsync.c (GLib threads, real time): run with `make check`. The bounds are loose enough for
 * a loaded build host; they check the rate, the thread, start/stop/rate changes and the shutdown.
 */
#include "uc_vsync.h"

#include "uc_refresh.h"

typedef struct {
    GMutex lock;
    guint ticks;
    gint64 first_us;            /* time of the first tick */
    GThread *thread;            /* thread of the last tick */
} Probe;

static void
on_tick (gpointer user_data)
{
    Probe *p = user_data;
    g_mutex_lock (&p->lock);
    if (p->ticks == 0)
        p->first_us = g_get_monotonic_time ();
    p->ticks++;
    p->thread = g_thread_self ();
    g_mutex_unlock (&p->lock);
}

static guint
probe_ticks (Probe *p)
{
    guint ticks;
    g_mutex_lock (&p->lock);
    ticks = p->ticks;
    g_mutex_unlock (&p->lock);
    return ticks;
}

static void
probe_init (Probe *p)
{
    g_mutex_init (&p->lock);
    p->ticks = 0;
    p->first_us = 0;
    p->thread = NULL;
}

static void
sleep_ms (int ms)
{
    g_usleep ((gulong) ms * 1000);
}

static void
test_rate_and_thread (void)
{
    Probe p;
    UcVsync *v;
    gint64 start, elapsed_us;
    guint ticks, expected;

    probe_init (&p);
    v = uc_vsync_new (200000, on_tick, &p);          /* 200 Hz: 5 ms */
    g_assert_false (uc_vsync_is_running (v));
    g_assert_cmpint (uc_vsync_get_rate (v), ==, 200000);
    start = g_get_monotonic_time ();
    uc_vsync_start (v);
    g_assert_true (uc_vsync_is_running (v));
    sleep_ms (300);
    ticks = probe_ticks (&p);
    elapsed_us = g_get_monotonic_time () - start;
    uc_vsync_stop (v);
    expected = (guint) (elapsed_us / 5000);
    g_test_message ("%u ticks in %.1f ms, %u expected", ticks, elapsed_us / 1000.0, expected);
    /* absolute deadlines: never more than one tick per period, and not much fewer */
    g_assert_cmpuint (ticks, <=, expected + 1);
    g_assert_cmpuint (ticks, >=, expected * 3 / 4);
    g_assert_cmpuint (uc_vsync_get_ticks (v), >=, ticks);
    /* the ticks come from the vsync thread, not from the caller's */
    g_mutex_lock (&p.lock);
    g_assert_nonnull (p.thread);
    g_assert_true (p.thread != g_thread_self ());
    g_mutex_unlock (&p.lock);
    uc_vsync_free (v);
    g_mutex_clear (&p.lock);
}

static void
test_first_tick_one_period_after_start (void)
{
    Probe p;
    UcVsync *v;
    gint64 start;

    probe_init (&p);
    v = uc_vsync_new (20000, on_tick, &p);           /* 20 Hz: 50 ms */
    start = g_get_monotonic_time ();
    uc_vsync_start (v);
    sleep_ms (30);
    g_assert_cmpuint (probe_ticks (&p), ==, 0);
    sleep_ms (120);
    g_assert_cmpuint (probe_ticks (&p), >=, 1);
    g_mutex_lock (&p.lock);
    g_assert_cmpint (p.first_us - start, >=, 49000);
    g_assert_cmpint (p.first_us - start, <, 150000);
    g_mutex_unlock (&p.lock);
    uc_vsync_free (v);
    g_mutex_clear (&p.lock);
}

static void
test_stop_parks_and_start_resumes (void)
{
    Probe p;
    UcVsync *v;
    guint stopped_at;

    probe_init (&p);
    v = uc_vsync_new (200000, on_tick, &p);
    uc_vsync_start (v);
    uc_vsync_start (v);                              /* a second start changes nothing */
    sleep_ms (50);
    uc_vsync_stop (v);
    g_assert_false (uc_vsync_is_running (v));
    sleep_ms (10);                                   /* a tick that was running when stop came has returned */
    stopped_at = probe_ticks (&p);
    g_assert_cmpuint (stopped_at, >, 0);
    sleep_ms (60);
    g_assert_cmpuint (probe_ticks (&p), ==, stopped_at);
    uc_vsync_stop (v);                               /* stopping a stopped source is fine */

    uc_vsync_start (v);
    sleep_ms (60);
    g_assert_cmpuint (probe_ticks (&p), >, stopped_at);
    uc_vsync_free (v);
    g_mutex_clear (&p.lock);
}

static void
test_rate_change_applies_to_the_next_tick (void)
{
    Probe p;
    UcVsync *v;
    guint slow;

    probe_init (&p);
    v = uc_vsync_new (20000, on_tick, &p);           /* 50 ms */
    uc_vsync_start (v);
    sleep_ms (120);
    slow = probe_ticks (&p);
    g_assert_cmpuint (slow, <=, 3);
    uc_vsync_set_rate (v, 200000);                   /* 5 ms, wakes the thread out of its 50 ms wait */
    g_assert_cmpint (uc_vsync_get_rate (v), ==, 200000);
    sleep_ms (100);
    g_assert_cmpuint (probe_ticks (&p) - slow, >=, 10);
    g_assert_cmpuint (probe_ticks (&p) - slow, <=, 22);
    uc_vsync_free (v);
    g_mutex_clear (&p.lock);
}

static void
test_free_while_running_joins (void)
{
    Probe p;
    UcVsync *v;
    gint64 start;
    guint after_free;

    probe_init (&p);
    v = uc_vsync_new (20000, on_tick, &p);           /* 50 ms: free comes in the middle of a wait */
    uc_vsync_start (v);
    sleep_ms (70);
    start = g_get_monotonic_time ();
    uc_vsync_free (v);                               /* wakes the thread, joins it */
    g_assert_cmpint (g_get_monotonic_time () - start, <, 40000);
    after_free = probe_ticks (&p);
    sleep_ms (80);
    g_assert_cmpuint (probe_ticks (&p), ==, after_free);
    g_mutex_clear (&p.lock);
}

static void
test_never_started (void)
{
    Probe p;
    UcVsync *v;

    probe_init (&p);
    v = uc_vsync_new (60000, on_tick, &p);
    uc_vsync_set_trace (v, TRUE);
    uc_vsync_set_rate (v, 50000);
    uc_vsync_stop (v);
    sleep_ms (40);
    g_assert_cmpuint (probe_ticks (&p), ==, 0);
    g_assert_cmpuint (uc_vsync_get_ticks (v), ==, 0);
    uc_vsync_free (v);
    uc_vsync_free (NULL);
    g_mutex_clear (&p.lock);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/vsync/rate-and-thread", test_rate_and_thread);
    g_test_add_func ("/vsync/first-tick", test_first_tick_one_period_after_start);
    g_test_add_func ("/vsync/stop-start", test_stop_parks_and_start_resumes);
    g_test_add_func ("/vsync/set-rate", test_rate_change_applies_to_the_next_tick);
    g_test_add_func ("/vsync/free-while-running", test_free_while_running_joins);
    g_test_add_func ("/vsync/never-started", test_never_started);
    return g_test_run ();
}
