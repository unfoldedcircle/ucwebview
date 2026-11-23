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

struct ucfp_glib_loop {
    GMainContext *context;
    int epoll_fd;
    int timer_fd;
    sd_event_source *source;

    GPollFD *fds; /* the fds of the last query, n_fds of capacity */
    int n_fds;
    int capacity;
    GPollFD *registered; /* the fds in the epoll set, n_registered of registered_capacity */
    int n_registered;
    int registered_capacity;
    int max_priority;
    bool dispatching;
    bool warned_epoll;

    struct ucfp_glib_stats stats;
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

/* Makes the epoll set hold exactly the fds of the last query, and arms the timer for its timeout. */
static void update(struct ucfp_glib_loop *loop, int timeout_ms)
{
    bool always_ready = false;

    /* fds of the previous query that are gone (EBADF or ENOENT when GLib closed them already: ignored) */
    for (int i = 0; i < loop->n_registered; i++) {
        if (!find_fd(loop->fds, loop->n_fds, loop->registered[i].fd))
            epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, loop->registered[i].fd, NULL);
    }
    /* Every fd again, also one with the same number and events as before: GLib may have closed it and got the number
     * back for another file, which epoll does not know (it keys its entries by file and fd number; the closed file
     * left the set with its last fd). MOD fails with ENOENT for such an fd, which is then added. */
    for (int i = 0; i < loop->n_fds; i++) {
        struct epoll_event ev = { .events = epoll_events(loop->fds[i].events), .data.fd = loop->fds[i].fd };
        if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, loop->fds[i].fd, &ev) == 0)
            continue;
        if (errno == ENOENT && epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->fds[i].fd, &ev) == 0)
            continue;
        /* a file epoll cannot watch (EPERM: a regular file) is always ready for poll() */
        if (!loop->warned_epoll) {
            fprintf(stderr, "[uc_webview] GLib polls fd %d, which epoll refuses (%s); it is polled at once\n",
                    loop->fds[i].fd, strerror(errno));
            loop->warned_epoll = true;
        }
        always_ready = true;
    }
    if (loop->registered_capacity < loop->n_fds) {
        loop->registered = g_renew(GPollFD, loop->registered, loop->n_fds);
        loop->registered_capacity = loop->n_fds;
    }
    if (loop->n_fds)
        memcpy(loop->registered, loop->fds, (size_t) loop->n_fds * sizeof(GPollFD));
    loop->n_registered = loop->n_fds;

    /* timeout < 0: no timer; 0: a source is ready, come back after the other events of this sd_event iteration */
    struct itimerspec spec = { 0 };
    if (timeout_ms == 0 || always_ready) {
        spec.it_value.tv_nsec = 1;
    } else if (timeout_ms > 0) {
        spec.it_value.tv_sec = timeout_ms / 1000;
        spec.it_value.tv_nsec = (long) (timeout_ms % 1000) * 1000000L;
    }
    timerfd_settime(loop->timer_fd, 0, &spec, NULL);
}

static void prepare_and_arm(struct ucfp_glib_loop *loop)
{
    int timeout_ms = -1;
    int n;

    g_main_context_prepare(loop->context, &loop->max_priority);
    while ((n = g_main_context_query(loop->context, loop->max_priority, &timeout_ms, loop->fds, loop->capacity)) >
           loop->capacity) {
        loop->fds = g_renew(GPollFD, loop->fds, n);
        loop->capacity = n;
    }
    loop->n_fds = n;
    update(loop, timeout_ms);
}

static int on_ready(sd_event_source *source, int fd, uint32_t revents, void *userdata)
{
    struct ucfp_glib_loop *loop = userdata;
    uint64_t expirations, start, elapsed;

    (void) source;
    (void) fd;
    (void) revents;

    if (read(loop->timer_fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
        fprintf(stderr, "[uc_webview] reading the GLib timer failed: %s\n", strerror(errno));

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
    return 0;
}

struct ucfp_glib_loop *ucfp_glib_loop_new(ucfp_add_io_fn add_io, char *reason, size_t reason_size)
{
    struct ucfp_glib_loop *loop = g_new0(struct ucfp_glib_loop, 1);
    struct epoll_event ev = { .events = EPOLLIN };
    int err;

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
    if (loop && !loop->dispatching)
        prepare_and_arm(loop);
}

void ucfp_glib_loop_get_stats(const struct ucfp_glib_loop *loop, struct ucfp_glib_stats *stats)
{
    if (loop)
        *stats = loop->stats;
    else
        memset(stats, 0, sizeof(*stats));
}
