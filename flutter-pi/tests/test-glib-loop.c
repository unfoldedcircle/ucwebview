/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host test of flutterpi/ucfp_glib_loop.c: GLib's default main context driven from an sd_event loop, as flutter-pi's
 * platform thread does (needs GLib and libsystemd, no flutter-pi). The test's own sd_event loop stands in for
 * flutter-pi's; test_add_io() for flutterpi_sd_event_add_io().
 *
 * Covered: timeouts fire on time, a source attached from another thread wakes the loop by itself, one attached from
 * the loop's thread outside a dispatch runs after ucfp_glib_loop_rearm(), fd sources dispatch, an fd number GLib
 * closed and got back for another file is watched again, and the dispatch time is measured.
 */
#include "ucfp_glib_loop.h"

#include <fcntl.h>
#include <unistd.h>

#include <glib-unix.h>
#include <glib.h>

static sd_event *event;

static int test_add_io(sd_event_source **source_out, int fd, uint32_t events, sd_event_io_handler_t callback,
                       void *userdata)
{
    int r = sd_event_add_io(event, source_out, fd, events, callback, userdata);
    return r < 0 ? -r : 0;
}

/* runs the sd_event loop until *flag or timeout_ms passed; returns the flag */
static gboolean run_until(const gboolean *flag, int timeout_ms)
{
    gint64 end = g_get_monotonic_time() + timeout_ms * 1000;

    while (!*flag && g_get_monotonic_time() < end)
        g_assert_cmpint(sd_event_run(event, 10 * 1000), >=, 0);
    return *flag;
}

static gboolean set_flag(gpointer data)
{
    *(gboolean *) data = TRUE;
    return G_SOURCE_REMOVE;
}

static struct ucfp_glib_loop *setup(void)
{
    char reason[256];
    struct ucfp_glib_loop *loop;

    g_assert_cmpint(sd_event_new(&event), >=, 0);
    loop = ucfp_glib_loop_new(test_add_io, reason, sizeof(reason));
    g_assert_nonnull(loop);
    return loop;
}

static void teardown(struct ucfp_glib_loop *loop)
{
    ucfp_glib_loop_destroy(loop);
    event = sd_event_unref(event);
}

static void test_timeout(void)
{
    struct ucfp_glib_loop *loop = setup();
    gboolean fired = FALSE;
    gint64 start = g_get_monotonic_time();

    /* attached from this thread before the next prepare: the loop's own re-arm after the timer picks it up */
    g_timeout_add(30, set_flag, &fired);
    ucfp_glib_loop_rearm(loop);
    g_assert_true(run_until(&fired, 2000));
    gint64 elapsed_ms = (g_get_monotonic_time() - start) / 1000;
    g_assert_cmpint(elapsed_ms, >=, 29);
    g_assert_cmpint(elapsed_ms, <, 500);
    teardown(loop);
}

static gpointer attach_later(gpointer data)
{
    g_usleep(20 * 1000);
    g_idle_add(set_flag, data); /* the default context, owned by the test's thread: GLib signals its wakeup */
    return NULL;
}

static void test_other_thread_wakes(void)
{
    struct ucfp_glib_loop *loop = setup();
    gboolean fired = FALSE;
    GThread *thread = g_thread_new("attach", attach_later, &fired);

    g_assert_true(run_until(&fired, 2000));
    g_thread_join(thread);
    teardown(loop);
}

static void test_same_thread_needs_rearm(void)
{
    struct ucfp_glib_loop *loop = setup();
    gboolean fired = FALSE;

    /* without a dispatch in between, nothing would wake the loop: the source is attached by the owner thread */
    g_idle_add(set_flag, &fired);
    g_assert_false(run_until(&fired, 100));
    ucfp_glib_loop_rearm(loop);
    g_assert_true(run_until(&fired, 1000));
    teardown(loop);
}

static gboolean on_readable(gint fd, GIOCondition condition, gpointer data)
{
    char c;
    (void) condition;
    g_assert_cmpint(read(fd, &c, 1), ==, 1);
    *(gboolean *) data = TRUE;
    return G_SOURCE_CONTINUE;
}

static void test_fd_and_reuse(void)
{
    struct ucfp_glib_loop *loop = setup();
    gboolean fired = FALSE;
    int a[2], b[2];
    guint id;

    g_assert_cmpint(pipe2(a, O_CLOEXEC), ==, 0);
    id = g_unix_fd_add(a[0], G_IO_IN, on_readable, &fired);
    ucfp_glib_loop_rearm(loop);
    g_assert_cmpint(write(a[1], "x", 1), ==, 1);
    g_assert_true(run_until(&fired, 1000));

    /* Between two queries GLib drops the source, the fd is closed and the next pipe gets the same number for its read
     * end, polled for the same events: the epoll set lost the closed file, so the loop must add the number again
     * although the query looks unchanged. */
    int old = a[0];
    g_source_remove(id);
    close(a[0]);
    close(a[1]);
    g_assert_cmpint(pipe2(b, O_CLOEXEC), ==, 0);
    g_assert_cmpint(b[0], ==, old);
    fired = FALSE;
    id = g_unix_fd_add(b[0], G_IO_IN, on_readable, &fired);
    ucfp_glib_loop_rearm(loop);
    /* GLib signals its wakeup when a poll fd is added or removed (gmain.c, g_main_context_add_poll_unlocked()):
     * let the loop take that wake-up first, so that only the epoll entry of b[0] can report the byte */
    g_assert_false(run_until(&fired, 50));
    g_assert_cmpint(write(b[1], "y", 1), ==, 1);
    g_assert_true(run_until(&fired, 1000));
    g_source_remove(id);
    close(b[0]);
    close(b[1]);
    teardown(loop);
}

static gboolean busy(gpointer data)
{
    g_usleep(5000);
    *(gboolean *) data = TRUE;
    return G_SOURCE_REMOVE;
}

static void test_stats(void)
{
    struct ucfp_glib_loop *loop = setup();
    struct ucfp_glib_stats stats;
    gboolean fired = FALSE;

    g_idle_add(busy, &fired);
    ucfp_glib_loop_rearm(loop);
    g_assert_true(run_until(&fired, 1000));
    ucfp_glib_loop_get_stats(loop, &stats);
    g_assert_cmpuint(stats.dispatches, >=, 1);
    g_assert_cmpuint(stats.max_dispatch_us, >=, 5000);
    g_assert_cmpuint(stats.total_dispatch_us, >=, stats.max_dispatch_us);
    g_assert_cmpuint(stats.slow_dispatches, ==, 0);
    teardown(loop);
}

static void test_idle_loop_sleeps(void)
{
    struct ucfp_glib_loop *loop = setup();
    struct ucfp_glib_stats stats;
    gboolean never = FALSE;

    /* nothing attached: no wake-ups at all (an idle web view must cost no CPU) */
    g_assert_false(run_until(&never, 200));
    ucfp_glib_loop_get_stats(loop, &stats);
    g_assert_cmpuint(stats.dispatches, ==, 0);
    teardown(loop);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/glib-loop/timeout", test_timeout);
    g_test_add_func("/glib-loop/other-thread-wakes", test_other_thread_wakes);
    g_test_add_func("/glib-loop/same-thread-needs-rearm", test_same_thread_needs_rearm);
    g_test_add_func("/glib-loop/fd-and-reuse", test_fd_and_reuse);
    g_test_add_func("/glib-loop/stats", test_stats);
    g_test_add_func("/glib-loop/idle-loop-sleeps", test_idle_loop_sleeps);
    return g_test_run();
}
