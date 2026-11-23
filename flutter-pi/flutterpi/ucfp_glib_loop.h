/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * GLib's default main context, iterated from flutter-pi's event loop on the platform thread.
 *
 * libucwebview needs the thread that calls it to iterate g_main_context_default() (uc_webview.h, "Threading
 * contract"); WebKit and WPEPlatform deliver everything through it. flutter-pi's platform thread runs an sd_event
 * loop instead. This module drives GLib's context from it, as an external main loop (GLib's g_main_context_prepare(),
 * _query(), _check() and _dispatch(), the steps of g_main_context_iterate()):
 *   - the context is acquired once and kept by the platform thread, so sources attached or made ready from other
 *     threads (WebKit's work queues, the library's display link) wake it through the context's wakeup fd
 *     (GLib gmain.c, g_source_attach_unlocked(): it signals the wakeup when another thread owns the context);
 *   - after each prepare/query the polled fds go into one epoll set, together with a timerfd for the timeout of the
 *     query, and the epoll fd is the only source in sd_event (flutterpi_sd_event_add_io());
 *   - when the epoll fd is readable the fds' revents are polled (timeout 0), the sources are checked and dispatched,
 *     and the next prepare/query re-arms the set and the timer.
 * A source without fds (an idle, a timeout, a ready time) attached from the platform thread itself outside a dispatch
 * does not signal the wakeup, because the thread owns the context (GLib signals it for every added or removed poll fd,
 * g_main_context_add_poll_unlocked()), so every call into the library from a platform message is followed by
 * ucfp_glib_loop_rearm().
 *
 * The time of each check + dispatch is measured: the platform thread holds flutter-pi's event loop lock meanwhile,
 * so a long dispatch delays platform messages, vsync replies and the platform tasks other threads post
 * (flutter-pi.c, flutterpi_post_platform_task()). The longest one is logged and reported by the stats.
 *
 * Thread: the platform thread, for every function.
 */
#ifndef UCFP_GLIB_LOOP_H
#define UCFP_GLIB_LOOP_H

#include <stddef.h>
#include <stdint.h>

#include <systemd/sd-event.h>

/* flutterpi_sd_event_add_io() of flutter-pi.h; the tests pass their own. */
typedef int (*ucfp_add_io_fn)(sd_event_source **source_out, int fd, uint32_t events, sd_event_io_handler_t callback,
                              void *userdata);

struct ucfp_glib_loop;

struct ucfp_glib_stats {
    uint64_t dispatches;        /* wake-ups that checked and dispatched the context */
    uint64_t max_dispatch_us;   /* the longest check + dispatch */
    uint64_t total_dispatch_us; /* all of them together */
    uint64_t slow_dispatches;   /* those longer than UCFP_GLIB_SLOW_US */
};

/* one frame period at 60 Hz */
#define UCFP_GLIB_SLOW_US 16667

/* Acquires g_main_context_default() for the calling thread and starts iterating it. NULL with a reason when another
 * thread owns the context or an fd could not be created. */
struct ucfp_glib_loop *ucfp_glib_loop_new(ucfp_add_io_fn add_io, char *reason, size_t reason_size);

/* Stops iterating and releases the context. */
void ucfp_glib_loop_destroy(struct ucfp_glib_loop *loop);

/* Prepares and queries the context again: call it after calling into GLib or the library from outside a dispatch
 * (a platform message, a platform task). Does nothing inside a dispatch, which re-arms when it ends. */
void ucfp_glib_loop_rearm(struct ucfp_glib_loop *loop);

void ucfp_glib_loop_get_stats(const struct ucfp_glib_loop *loop, struct ucfp_glib_stats *stats);

#endif /* UCFP_GLIB_LOOP_H */
