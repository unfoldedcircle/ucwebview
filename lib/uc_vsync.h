/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * UcVsync: a self-clocked vsync source for libucwebview. A thread of its own calls `func` once per refresh period
 * while the source is started; UcWpeSyncObserver (uc_wpe_display.c) turns the ticks into WPEScreenSyncObserver
 * syncs, which drive WebKit's display link (requestAnimationFrame, CSS animations, rendering updates).
 *
 * Why a thread with absolute deadlines: WPEScreenSyncObserver callbacks are meant to run "from a secondary
 * thread" (WPEScreenSyncObserver.cpp:98) and WebKit's display link handler asserts it is not on the main thread
 * (DisplayLink.cpp:187), so a busy UI thread (Qt's GUI thread) cannot delay a tick. Deadlines are kept in ns on
 * CLOCK_MONOTONIC (g_get_monotonic_time()) and advance by exactly one period per tick, so the rate does not drift
 * with the wake-up latency the way a relative sleep does (WebKit's own fallback sleeps 1000 / 60 = 16 ms per
 * tick, DisplayVBlankMonitorTimer.cpp:45-48, i.e. about 61 Hz).
 *
 * GLib only, host tested (tests/test-vsync.c). Every function may be called from any thread, also from inside
 * `func`, except uc_vsync_free(), which joins the vsync thread and is refused from inside `func`.
 */
#ifndef UC_VSYNC_H
#define UC_VSYNC_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _UcVsync UcVsync;

/* Called on the vsync thread, without any lock of UcVsync held. */
typedef void (*UcVsyncFunc) (gpointer user_data);

/* A stopped source ticking at refresh_mhz (> 0) once started; the thread is created by the first start. */
UcVsync  *uc_vsync_new         (int refresh_mhz, UcVsyncFunc func, gpointer user_data);
/* Stops the source and joins its thread (waits for a tick in progress to return). */
void      uc_vsync_free        (UcVsync *vsync);
/* Starts ticking; from the stopped state the first tick comes one period later. Starting a started source does
 * nothing. Returns at once. */
void      uc_vsync_start       (UcVsync *vsync);
/* Stops ticking; returns at once without waiting for a tick in progress, the thread parks without wake-ups. */
void      uc_vsync_stop        (UcVsync *vsync);
gboolean  uc_vsync_is_running  (UcVsync *vsync);
/* New rate (> 0) from the next tick on, measured from the last tick. */
void      uc_vsync_set_rate    (UcVsync *vsync, int refresh_mhz);
int       uc_vsync_get_rate    (UcVsync *vsync);
/* Ticks delivered so far (tests and the trace). */
guint64   uc_vsync_get_ticks   (UcVsync *vsync);
/* g_message() on start and stop and every 10 s while running: configured rate, measured rate, latest wake-up. */
void      uc_vsync_set_trace   (UcVsync *vsync, gboolean trace);

G_END_DECLS

#endif /* UC_VSYNC_H */
