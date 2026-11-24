/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * GLib's default main context on flutter-pi's sd_event loop, see ucfp_glib_loop.h.
 */
#include "ucfp_glib_loop.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <glib.h>

struct ucfp_glib_task;

struct ucfp_glib_loop {
    GMainContext *context;
    ucfp_post_task_fn post_task;
    int epoll_fd;
    int timer_fd;
    sd_event_source *source; /* the epoll fd in sd_event: on while waiting, off while a task is posted */
    struct ucfp_glib_task *task; /* the posted iteration, NULL when none is */

    GPollFD *fds; /* the fds of the last query, n_fds of capacity */
    int n_fds;
    int capacity;
    GPollFD *registered; /* the fds in the epoll set, n_registered of registered_capacity */
    int n_registered;
    int registered_capacity;
    bool must_rebuild; /* a new epoll set could not be made: the old one may hold stale entries */
    int max_priority;
    bool dispatching;
    bool warned_epoll;
    bool warned_rebuild;
    bool warned_post;

    struct ucfp_glib_stats stats;
};

/* A posted iteration; ucfp_glib_loop_destroy() clears loop when flutter-pi has not run it yet (a platform task cannot
 * be taken back). */
struct ucfp_glib_task {
    struct ucfp_glib_loop *loop;
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

static uint32_t epoll_events(gushort events)
{
    return (events & G_IO_IN ? EPOLLIN : 0) | (events & G_IO_OUT ? EPOLLOUT : 0) | (events & G_IO_PRI ? EPOLLPRI : 0);
}

static const GPollFD *find_fd(const GPollFD *fds, int n, int fd)
{
    for (int i = 0; i < n; i++) {
        if (fds[i].fd == fd)
            return &fds[i];
    }
    return NULL;
}

static void warn_unwatchable(struct ucfp_glib_loop *loop, int fd)
{
    /* a file epoll cannot watch (EPERM: a regular file) is always ready for poll() */
    if (!loop->warned_epoll) {
        fprintf(stderr, "[uc_webview] GLib polls fd %d, which epoll refuses (%s); it is polled every turn\n", fd,
                strerror(errno));
        loop->warned_epoll = true;
    }
}

/* Replaces the epoll set by an empty one with just the timer: the only way to drop an entry whose fd is closed while
 * its file lives on (ucfp_glib_loop.h, "Stale epoll entries"). false when it could not be made: the old set stays. */
static bool new_epoll_set(struct ucfp_glib_loop *loop)
{
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = loop->timer_fd };
    int fd = epoll_create1(EPOLL_CLOEXEC);
    int r;

    if (fd < 0 || epoll_ctl(fd, EPOLL_CTL_ADD, loop->timer_fd, &ev) != 0) {
        r = -errno;
    } else {
        r = sd_event_source_set_io_fd(loop->source, fd);
    }
    if (r < 0) {
        if (!loop->warned_rebuild) {
            fprintf(stderr, "[uc_webview] a new epoll set for GLib's fds failed: %s\n", strerror(-r));
            loop->warned_rebuild = true;
        }
        if (fd >= 0)
            close(fd);
        return false;
    }
    close(loop->epoll_fd);
    loop->epoll_fd = fd;
    loop->stats.epoll_rebuilds++;
    return true;
}

/* Makes the epoll set hold exactly the fds of the last query, and arms the timer for its timeout. false when an fd
 * cannot be watched by epoll, or the set could not be renewed: the caller polls in turns instead. */
static bool update(struct ucfp_glib_loop *loop, int timeout_ms)
{
    bool rebuild = loop->must_rebuild;
    bool watchable = true;

    /* Incrementally while every epoll_ctl() succeeds. A DEL of a number that left the query fails when the fd was
     * closed (EBADF) or the number belongs to another file now (ENOENT): if the closed file lives on in another
     * process or fd, its entry is still in the set and only a new set gets rid of it. A MOD of a number that stayed
     * fails alike when GLib closed it and got the number back for another file. A new number is added. */
    for (int i = 0; i < loop->n_registered && !rebuild; i++) {
        if (!find_fd(loop->fds, loop->n_fds, loop->registered[i].fd) &&
            epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, loop->registered[i].fd, NULL) != 0)
            rebuild = true;
    }
    for (int i = 0; i < loop->n_fds && !rebuild; i++) {
        struct epoll_event ev = { .events = epoll_events(loop->fds[i].events), .data.fd = loop->fds[i].fd };
        if (find_fd(loop->registered, loop->n_registered, loop->fds[i].fd)) {
            if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, loop->fds[i].fd, &ev) != 0)
                rebuild = true;
        } else if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->fds[i].fd, &ev) != 0) {
            if (errno == EPERM) {
                warn_unwatchable(loop, loop->fds[i].fd);
                watchable = false;
            } else {
                rebuild = true;
            }
        }
    }
    if (rebuild) {
        watchable = true;
        loop->must_rebuild = !new_epoll_set(loop);
        if (loop->must_rebuild)
            watchable = false;
        for (int i = 0; i < loop->n_fds && !loop->must_rebuild; i++) {
            struct epoll_event ev = { .events = epoll_events(loop->fds[i].events), .data.fd = loop->fds[i].fd };
            if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->fds[i].fd, &ev) != 0) {
                warn_unwatchable(loop, loop->fds[i].fd);
                watchable = false;
            }
        }
    }

    if (loop->registered_capacity < loop->n_fds) {
        loop->registered = g_renew(GPollFD, loop->registered, loop->n_fds);
        loop->registered_capacity = loop->n_fds;
    }
    if (loop->n_fds)
        memcpy(loop->registered, loop->fds, (size_t) loop->n_fds * sizeof(GPollFD));
    loop->n_registered = loop->n_fds;

    /* timeout < 0: no timer (0 does not get here: a source is ready) */
    struct itimerspec spec = { 0 };
    if (timeout_ms > 0) {
        spec.it_value.tv_sec = timeout_ms / 1000;
        spec.it_value.tv_nsec = (long) (timeout_ms % 1000) * 1000000L;
    }
    timerfd_settime(loop->timer_fd, 0, &spec, NULL);
    return watchable;
}

static int run_task(void *userdata);

/* Posts the next iteration as a platform task, with the io source off until it ran. false when posting failed: the
 * io source is then on with an expired timer, and on_ready() iterates by itself. */
static bool schedule(struct ucfp_glib_loop *loop)
{
    struct ucfp_glib_task *task;
    int err;

    if (loop->task)
        return true;
    sd_event_source_set_enabled(loop->source, SD_EVENT_OFF);
    task = g_new(struct ucfp_glib_task, 1);
    task->loop = loop;
    err = loop->post_task(run_task, task);
    if (err == 0) {
        loop->task = task;
        return true;
    }
    g_free(task);
    if (!loop->warned_post) {
        fprintf(stderr, "[uc_webview] posting a GLib iteration failed: %s\n", strerror(err));
        loop->warned_post = true;
    }
    struct itimerspec spec = { .it_value.tv_nsec = 1 };
    timerfd_settime(loop->timer_fd, 0, &spec, NULL);
    sd_event_source_set_enabled(loop->source, SD_EVENT_ON);
    return false;
}

/* Prepares and queries the context: a ready one gets the next turn (a posted task), an idle one waits for its fds and
 * its timeout in the epoll set. */
static void prepare_and_arm(struct ucfp_glib_loop *loop)
{
    int timeout_ms = -1;
    int n;
    bool ready;

    g_main_context_prepare(loop->context, &loop->max_priority);
    while ((n = g_main_context_query(loop->context, loop->max_priority, &timeout_ms, loop->fds, loop->capacity)) >
           loop->capacity) {
        loop->fds = g_renew(GPollFD, loop->fds, n);
        loop->capacity = n;
    }
    loop->n_fds = n;

    ready = timeout_ms == 0;
    if (!ready && loop->n_fds) {
        for (int i = 0; i < loop->n_fds; i++)
            loop->fds[i].revents = 0;
        ready = g_poll(loop->fds, (guint) loop->n_fds, 0) > 0;
    }
    if (!ready && update(loop, timeout_ms)) {
        sd_event_source_set_enabled(loop->source, SD_EVENT_ON);
        return;
    }
    schedule(loop);
}

/* One GLib iteration: the fds' revents (timeout 0), check, dispatch, and the next prepare/query. */
static void iterate(struct ucfp_glib_loop *loop)
{
    uint64_t start, elapsed;

    for (int i = 0; i < loop->n_fds; i++)
        loop->fds[i].revents = 0;
    if (loop->n_fds)
        g_poll(loop->fds, (guint) loop->n_fds, 0);

    start = now_us();
    loop->dispatching = true;
    g_main_context_check(loop->context, loop->max_priority, loop->fds, loop->n_fds);
    g_main_context_dispatch(loop->context);
    loop->dispatching = false;
    elapsed = now_us() - start;

    loop->stats.dispatches++;
    loop->stats.total_dispatch_us += elapsed;
    if (elapsed > UCFP_GLIB_SLOW_US)
        loop->stats.slow_dispatches++;
    if (elapsed > loop->stats.max_dispatch_us) {
        loop->stats.max_dispatch_us = elapsed;
        if (elapsed >= 4000)
            fprintf(stderr, "[uc_webview] longest GLib dispatch so far: %llu us (after %llu dispatches)\n",
                    (unsigned long long) elapsed, (unsigned long long) loop->stats.dispatches);
    }

    prepare_and_arm(loop);
}

static int run_task(void *userdata)
{
    struct ucfp_glib_task *task = userdata;
    struct ucfp_glib_loop *loop = task->loop;

    g_free(task);
    if (loop) {
        loop->task = NULL;
        iterate(loop);
    }
    return 0;
}

/* The epoll set is readable: a GLib fd or the timer. */
static int on_ready(sd_event_source *source, int fd, uint32_t revents, void *userdata)
{
    struct ucfp_glib_loop *loop = userdata;
    uint64_t expirations;

    (void) source;
    (void) fd;
    (void) revents;

    if (read(loop->timer_fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
        fprintf(stderr, "[uc_webview] reading the GLib timer failed: %s\n", strerror(errno));
    if (!schedule(loop))
        iterate(loop);
    return 0;
}

struct ucfp_glib_loop *ucfp_glib_loop_new(ucfp_add_io_fn add_io, ucfp_post_task_fn post_task, char *reason,
                                          size_t reason_size)
{
    struct ucfp_glib_loop *loop = g_new0(struct ucfp_glib_loop, 1);
    struct epoll_event ev = { .events = EPOLLIN };
    int err;

    loop->post_task = post_task;
    loop->epoll_fd = -1;
    loop->timer_fd = -1;
    loop->context = g_main_context_default();
    if (!g_main_context_acquire(loop->context)) {
        snprintf(reason, reason_size, "another thread owns GLib's default main context");
        g_free(loop);
        return NULL;
    }

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    loop->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    ev.data.fd = loop->timer_fd;
    if (loop->epoll_fd < 0 || loop->timer_fd < 0 || epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->timer_fd, &ev) != 0) {
        snprintf(reason, reason_size, "epoll/timerfd: %s", strerror(errno));
        goto fail;
    }
    err = add_io(&loop->source, loop->epoll_fd, EPOLLIN, on_ready, loop);
    if (err != 0) {
        snprintf(reason, reason_size, "adding the GLib fds to the event loop failed: %s", strerror(err < 0 ? -err : err));
        goto fail;
    }

    prepare_and_arm(loop);
    return loop;

fail:
    if (loop->epoll_fd >= 0)
        close(loop->epoll_fd);
    if (loop->timer_fd >= 0)
        close(loop->timer_fd);
    g_main_context_release(loop->context);
    g_free(loop);
    return NULL;
}

void ucfp_glib_loop_destroy(struct ucfp_glib_loop *loop)
{
    if (!loop)
        return;
    if (loop->task)
        loop->task->loop = NULL;
    sd_event_source_set_enabled(loop->source, SD_EVENT_OFF);
    sd_event_source_unref(loop->source);
    close(loop->epoll_fd);
    close(loop->timer_fd);
    g_free(loop->fds);
    g_free(loop->registered);
    g_main_context_release(loop->context);
    g_free(loop);
}

void ucfp_glib_loop_rearm(struct ucfp_glib_loop *loop)
{
    if (loop && !loop->dispatching && !loop->task)
        prepare_and_arm(loop);
}

void ucfp_glib_loop_get_stats(const struct ucfp_glib_loop *loop, struct ucfp_glib_stats *stats)
{
    if (loop)
        *stats = loop->stats;
    else
        memset(stats, 0, sizeof(*stats));
}
