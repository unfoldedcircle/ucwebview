/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * UcVsync: the self-clocked vsync thread of libucwebview, see uc_vsync.h.
 */
#include "uc_vsync.h"

#include "uc_refresh.h"

#define UC_VSYNC_TRACE_INTERVAL_NS G_GINT64_CONSTANT (10000000000)

typedef enum {
    VSYNC_STOPPED,
    VSYNC_RUNNING,
    VSYNC_QUIT,
} VsyncState;

struct _UcVsync {
    GMutex lock;
    GCond cond;
    GThread *thread;            /* created by the first start, joined by uc_vsync_free() */
    VsyncState state;
    gboolean restart;           /* started from STOPPED: the first tick is one period after the start */
    int rate_mhz;
    gint64 period_ns;
    gint64 last_ns;             /* when the last tick was due */
    guint64 ticks;
    UcVsyncFunc func;
    gpointer user_data;
    /* trace: one line per UC_VSYNC_TRACE_INTERVAL_NS while running */
    gboolean trace;
    gint64 window_start_ns;
    guint64 window_ticks;
    gint64 window_max_late_ns;
};

static gint64
now_ns (void)
{
    return g_get_monotonic_time () * 1000;
}

static gpointer
vsync_thread (gpointer data)
{
    UcVsync *v = data;

    g_mutex_lock (&v->lock);
    while (v->state != VSYNC_QUIT) {
        gint64 now, deadline;
        char *trace_line = NULL;

        if (v->state == VSYNC_STOPPED) {
            g_cond_wait (&v->cond, &v->lock);
            continue;
        }
        now = now_ns ();
        if (v->restart) {
            v->restart = FALSE;
            v->last_ns = now;
            v->window_start_ns = now;
            v->window_ticks = 0;
            v->window_max_late_ns = 0;
        }
        deadline = uc_refresh_next_tick (v->last_ns, now, v->period_ns);
        if (deadline > now) {
            /* TRUE: woken by start/stop/set_rate/free (or spuriously) before the deadline, so re-evaluate;
             * FALSE: the deadline (rounded up to the µs of g_cond_wait_until) has passed */
            if (g_cond_wait_until (&v->cond, &v->lock, (deadline + 999) / 1000))
                continue;
            if (v->state != VSYNC_RUNNING)
                continue;
            now = now_ns ();
        }
        v->last_ns = deadline;
        v->ticks++;
        v->window_ticks++;
        v->window_max_late_ns = MAX (v->window_max_late_ns, now - deadline);
        if (v->trace && now - v->window_start_ns >= UC_VSYNC_TRACE_INTERVAL_NS) {
            double seconds = (now - v->window_start_ns) / 1e9;
            trace_line = g_strdup_printf ("ucwebview: vsync %.3f Hz: %" G_GUINT64_FORMAT " ticks in %.1f s = %.2f Hz, "
                                          "latest wake-up %.2f ms after the deadline",
                                          v->rate_mhz / 1000.0, v->window_ticks, seconds, v->window_ticks / seconds,
                                          v->window_max_late_ns / 1e6);
            v->window_start_ns = now;
            v->window_ticks = 0;
            v->window_max_late_ns = 0;
        }
        g_mutex_unlock (&v->lock);

        v->func (v->user_data);
        if (trace_line != NULL) {
            g_message ("%s", trace_line);
            g_free (trace_line);
        }

        g_mutex_lock (&v->lock);
    }
    g_mutex_unlock (&v->lock);
    return NULL;
}

UcVsync *
uc_vsync_new (int refresh_mhz, UcVsyncFunc func, gpointer user_data)
{
    UcVsync *v;

    g_return_val_if_fail (refresh_mhz > 0, NULL);
    g_return_val_if_fail (func != NULL, NULL);

    v = g_new0 (UcVsync, 1);
    g_mutex_init (&v->lock);
    g_cond_init (&v->cond);
    v->state = VSYNC_STOPPED;
    v->rate_mhz = refresh_mhz;
    v->period_ns = uc_refresh_period_ns (refresh_mhz);
    v->func = func;
    v->user_data = user_data;
    return v;
}

void
uc_vsync_free (UcVsync *v)
{
    GThread *thread;

    if (v == NULL)
        return;
    g_mutex_lock (&v->lock);
    if (v->thread != NULL && g_thread_self () == v->thread) {
        g_mutex_unlock (&v->lock);
        g_critical ("ucwebview: uc_vsync_free() called from the vsync thread; leaking the vsync source");
        return;
    }
    v->state = VSYNC_QUIT;
    thread = v->thread;
    v->thread = NULL;
    g_cond_broadcast (&v->cond);
    g_mutex_unlock (&v->lock);

    if (thread != NULL)
        g_thread_join (thread);
    g_cond_clear (&v->cond);
    g_mutex_clear (&v->lock);
    g_free (v);
}

void
uc_vsync_start (UcVsync *v)
{
    gboolean started = FALSE, trace = FALSE;
    int rate = 0;

    g_return_if_fail (v != NULL);
    g_mutex_lock (&v->lock);
    if (v->state == VSYNC_STOPPED) {
        v->state = VSYNC_RUNNING;
        v->restart = TRUE;
        if (v->thread == NULL) {
            GError *error = NULL;
            v->thread = g_thread_try_new ("uc-vsync", vsync_thread, v, &error);
            if (v->thread == NULL) {
                g_warning ("ucwebview: cannot start the vsync thread: %s", error != NULL ? error->message : "unknown error");
                g_clear_error (&error);
                v->state = VSYNC_STOPPED;
            }
        }
        g_cond_broadcast (&v->cond);
        started = v->state == VSYNC_RUNNING;
        trace = v->trace;
        rate = v->rate_mhz;
    }
    g_mutex_unlock (&v->lock);
    if (started && trace)
        g_message ("ucwebview: vsync started at %.3f Hz", rate / 1000.0);
}

void
uc_vsync_stop (UcVsync *v)
{
    gboolean stopped = FALSE, trace = FALSE;
    guint64 ticks = 0;

    g_return_if_fail (v != NULL);
    g_mutex_lock (&v->lock);
    if (v->state == VSYNC_RUNNING) {
        v->state = VSYNC_STOPPED;
        g_cond_broadcast (&v->cond);
        stopped = TRUE;
        trace = v->trace;
        ticks = v->ticks;
    }
    g_mutex_unlock (&v->lock);
    if (stopped && trace)
        g_message ("ucwebview: vsync stopped (%" G_GUINT64_FORMAT " ticks so far)", ticks);
}

gboolean
uc_vsync_is_running (UcVsync *v)
{
    gboolean running;

    g_return_val_if_fail (v != NULL, FALSE);
    g_mutex_lock (&v->lock);
    running = v->state == VSYNC_RUNNING;
    g_mutex_unlock (&v->lock);
    return running;
}

void
uc_vsync_set_rate (UcVsync *v, int refresh_mhz)
{
    g_return_if_fail (v != NULL);
    g_return_if_fail (refresh_mhz > 0);
    g_mutex_lock (&v->lock);
    if (refresh_mhz != v->rate_mhz) {
        v->rate_mhz = refresh_mhz;
        v->period_ns = uc_refresh_period_ns (refresh_mhz);
        g_cond_broadcast (&v->cond);
    }
    g_mutex_unlock (&v->lock);
}

int
uc_vsync_get_rate (UcVsync *v)
{
    int rate;

    g_return_val_if_fail (v != NULL, 0);
    g_mutex_lock (&v->lock);
    rate = v->rate_mhz;
    g_mutex_unlock (&v->lock);
    return rate;
}

guint64
uc_vsync_get_ticks (UcVsync *v)
{
    guint64 ticks;

    g_return_val_if_fail (v != NULL, 0);
    g_mutex_lock (&v->lock);
    ticks = v->ticks;
    g_mutex_unlock (&v->lock);
    return ticks;
}

void
uc_vsync_set_trace (UcVsync *v, gboolean trace)
{
    g_return_if_fail (v != NULL);
    g_mutex_lock (&v->lock);
    v->trace = trace;
    g_mutex_unlock (&v->lock);
}
