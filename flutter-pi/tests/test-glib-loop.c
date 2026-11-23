/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host test of flutterpi/ucfp_glib_loop.c: GLib's default main context driven from an sd_event loop, as flutter-pi's
 * platform thread does (needs GLib and libsystemd, no flutter-pi). The test's own sd_event loop stands in for
 * flutter-pi's; test_add_io() for flutterpi_sd_event_add_io(), test_post_task() for flutterpi_post_platform_task()
 * (a defer source per task, priority from a growing counter, as flutter-pi.c does).
 *
 * Covered: timeouts fire on time, a source attached from another thread wakes the loop by itself, one attached from
 * the loop's thread outside a dispatch runs after ucfp_glib_loop_rearm(), fd sources dispatch, an fd number GLib
 * closed and got back for another file is watched again, a closed fd whose file stays open and readable elsewhere
 * does not keep the loop busy, an always-ready GLib takes turns with flutter-pi's tasks and input instead of starving
 * them (and is not starved by them), and the dispatch time is measured.
 */
#include "ucfp_glib_loop.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
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

struct test_task {
    int (*callback)(void *userdata);
    void *userdata;
};

/* flutter-pi's platform tasks start at 0; a running flutter-pi has posted hundreds already */
static int64_t task_counter = 1000;

static int on_test_task(sd_event_source *source, void *userdata)
{
    struct test_task task = *(struct test_task *) userdata;

    g_free(userdata);
    sd_event_source_set_enabled(source, SD_EVENT_OFF);
    sd_event_source_unref(source);
    task.callback(task.userdata);
    return 0;
}

static int test_post_task(int (*callback)(void *userdata), void *userdata)
{
    struct test_task *task = g_new(struct test_task, 1);
    sd_event_source *source;
    int r;

    task->callback = callback;
    task->userdata = userdata;
    r = sd_event_add_defer(event, &source, on_test_task, task);
    if (r < 0) {
        g_free(task);
        return -r;
    }
    sd_event_source_set_priority(source, task_counter++);
    return 0;
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
    loop = ucfp_glib_loop_new(test_add_io, test_post_task, reason, sizeof(reason));
    g_assert_nonnull(loop);
    return loop;
}

static void teardown(struct ucfp_glib_loop *loop)
{
    ucfp_glib_loop_destroy(loop);
    /* tasks still posted run, and do nothing for the destroyed loop */
    while (sd_event_run(event, 0) > 0) {
    }
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

/* counts the sd_event iterations that dispatched something within ms: an idle loop has none */
static int busy_iterations(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    int busy = 0;

    while (g_get_monotonic_time() < end) {
        int r = sd_event_run(event, 10 * 1000);
        g_assert_cmpint(r, >=, 0);
        busy += r > 0;
    }
    return busy;
}

static gboolean never_called(gint fd, GIOCondition condition, gpointer data)
{
    (void) fd;
    (void) condition;
    (void) data;
    g_assert_not_reached();
    return G_SOURCE_REMOVE;
}

/* An fd leaves GLib's poll set and is closed while another reference keeps its file open and readable (on a device: a
 * rendering fence that WebKit stops polling and closes, held by the web process too). epoll keys its entries by fd
 * number and file and removes an entry only when the file's last reference is closed, so the entry stays, and a DEL by
 * the number fails (EBADF, or ENOENT once the number belongs to another file). A stale entry for a readable file keeps
 * the epoll fd readable: the loop must not wake for it again and again. reuse: the number is taken by another file. */
static void stale_entry(gboolean eventfd_file, gboolean reuse)
{
    struct ucfp_glib_loop *loop = setup();
    int fds[2], keep, other = -1;
    guint id;

    if (eventfd_file) {
        fds[0] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        fds[1] = dup(fds[0]);
        g_assert_cmpint(fds[0], >=, 0);
    } else {
        g_assert_cmpint(pipe2(fds, O_CLOEXEC), ==, 0);
    }
    keep = dup(fds[0]);
    g_assert_cmpint(keep, >=, 0);
    id = g_unix_fd_add(fds[0], G_IO_IN, never_called, NULL);
    ucfp_glib_loop_rearm(loop);
    /* the wake-up of the added poll fd */
    g_assert_cmpint(busy_iterations(50), <=, 3);

    int old = fds[0];
    g_source_remove(id);
    close(fds[0]);
    if (reuse) {
        other = open("/dev/null", O_RDONLY | O_CLOEXEC);
        g_assert_cmpint(other, ==, old);
    }
    /* readable from now on, through the file the closed fd shared with keep */
    if (eventfd_file)
        g_assert_cmpint(eventfd_write(fds[1], 1), ==, 0);
    else
        g_assert_cmpint(write(fds[1], "x", 1), ==, 1);
    ucfp_glib_loop_rearm(loop);

    /* a few wake-ups for the removed poll fd at most, then quiet (a spin makes thousands) */
    int busy = busy_iterations(300);
    g_test_message("busy iterations in 300 ms: %d", busy);
    g_assert_cmpint(busy, <=, 5);

    if (other >= 0)
        close(other);
    close(keep);
    close(fds[1]);
    teardown(loop);
}

static void test_stale_pipe_reused(void)
{
    stale_entry(FALSE, TRUE);
}

static void test_stale_eventfd(void)
{
    stale_entry(TRUE, FALSE);
}

static gboolean count_idle(gpointer data)
{
    (*(int *) data)++;
    return G_SOURCE_CONTINUE;
}

struct frames {
    int count;
    gboolean stop;
};

/* a platform task that posts itself again, as a steady stream of flutter-pi work (begin-frames, messages) */
static int frame_task(void *userdata)
{
    struct frames *frames = userdata;

    frames->count++;
    if (!frames->stop)
        g_assert_cmpint(test_post_task(frame_task, frames), ==, 0);
    return 0;
}

static int on_input(sd_event_source *source, int fd, uint32_t revents, void *userdata)
{
    uint64_t expirations;

    (void) source;
    (void) revents;
    g_assert_cmpint(read(fd, &expirations, sizeof(expirations)), ==, sizeof(expirations));
    (*(int *) userdata)++;
    return 0;
}

/* GLib always ready (an idle source that stays) against flutter-pi always busy (a task always pending) and input every
 * millisecond at flutter-pi's input priority: everyone gets turns. */
static void test_no_starvation(void)
{
    struct ucfp_glib_loop *loop = setup();
    struct frames frames = { 0 };
    sd_event_source *input;
    int idles = 0, inputs = 0;
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec every_ms = { .it_interval.tv_nsec = 1000000, .it_value.tv_nsec = 1000000 };

    g_assert_cmpint(tfd, >=, 0);
    g_assert_cmpint(sd_event_add_io(event, &input, tfd, EPOLLIN, on_input, &inputs), >=, 0);
    g_assert_cmpint(sd_event_source_set_priority(input, SD_EVENT_PRIORITY_IDLE - 10), >=, 0);
    g_assert_cmpint(timerfd_settime(tfd, 0, &every_ms, NULL), ==, 0);

    guint id = g_idle_add(count_idle, &idles);
    ucfp_glib_loop_rearm(loop);
    g_assert_cmpint(test_post_task(frame_task, &frames), ==, 0);

    gint64 end = g_get_monotonic_time() + 300 * 1000;
    while (g_get_monotonic_time() < end)
        g_assert_cmpint(sd_event_run(event, 10 * 1000), >=, 0);
    g_test_message("in 300 ms: %d GLib idles, %d flutter-pi tasks, %d input wake-ups", idles, frames.count, inputs);

    /* one GLib iteration and one task in turn; input every millisecond (some expirations coalesce) */
    g_assert_cmpint(idles, >, 200);
    g_assert_cmpint(frames.count, >, 200);
    g_assert_cmpint(idles, >=, frames.count / 2);
    g_assert_cmpint(frames.count, >=, idles / 2);
    g_assert_cmpint(inputs, >=, 100);

    g_source_remove(id);
    frames.stop = TRUE;
    sd_event_source_set_enabled(input, SD_EVENT_OFF);
    sd_event_source_unref(input);
    close(tfd);
    teardown(loop);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/glib-loop/timeout", test_timeout);
    g_test_add_func("/glib-loop/other-thread-wakes", test_other_thread_wakes);
    g_test_add_func("/glib-loop/same-thread-needs-rearm", test_same_thread_needs_rearm);
    g_test_add_func("/glib-loop/fd-and-reuse", test_fd_and_reuse);
    g_test_add_func("/glib-loop/stale-entry/pipe-number-reused", test_stale_pipe_reused);
    g_test_add_func("/glib-loop/stale-entry/eventfd", test_stale_eventfd);
    g_test_add_func("/glib-loop/no-starvation", test_no_starvation);
    g_test_add_func("/glib-loop/stats", test_stats);
    g_test_add_func("/glib-loop/idle-loop-sleeps", test_idle_loop_sleeps);
    return g_test_run();
}
