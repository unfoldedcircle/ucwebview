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
 *   - one GLib iteration (poll with timeout 0, check, dispatch, prepare, query) runs per flutter-pi platform task
 *     (flutterpi_post_platform_task()), never more than one per task, so GLib work takes turns with flutter-pi's
 *     own work (see "Scheduling" below);
 *   - when the query leaves nothing ready (timeout not 0, no polled fd readable), the polled fds go into one epoll
 *     set, together with a timerfd for the timeout of the query, and the epoll fd waits in sd_event as an io source
 *     (flutterpi_sd_event_add_io()); when it is readable, the io source turns itself off and posts the next
 *     iteration.
 * A source without fds (an idle, a timeout, a ready time) attached from the platform thread itself outside a dispatch
 * does not signal the wakeup, because the thread owns the context (GLib signals it for every added or removed poll fd,
 * g_main_context_add_poll_unlocked()), so every call into the library from a platform message is followed by
 * ucfp_glib_loop_rearm().
 *
 * Scheduling. sd_event dispatches one source per iteration, the pending one with the lowest priority value
 * (sd-event.c, pending_prioq_compare(); a defer source stays pending until it is disabled, an io source until it is
 * dispatched). flutter-pi gives its input source SD_EVENT_PRIORITY_IDLE - 10 and every platform task, the
 * begin-frame of each vsync included, the next value of an ever-growing counter (flutter-pi.c,
 * flutterpi_post_platform_task()). A GLib io source at any fixed priority would, while GLib stays ready (a busy page
 * with idle sources, an fd GLib does not drain), win against every platform task posted after the counter passed
 * that priority, and at a priority below input it would also starve input. A fixed priority low enough for both
 * would starve GLib instead as long as flutter-pi posts tasks. So GLib does not keep a source of its own pending:
 * each iteration is a platform task, ordered first-in first-out with flutter-pi's tasks by that counter, and the io
 * source is off while one is posted. A task that is pending already cannot be overtaken by the GLib task posted after
 * it, and GLib's next task cannot be overtaken by tasks posted after it either: neither side starves the other.
 * Input (a priority value far below the counter after start-up) is handled before a pending GLib task. The io source
 * keeps sd_event's default priority: it only posts a task, it never dispatches GLib itself.
 *
 * Stale epoll entries. epoll keys an entry by fd number and file and removes it by itself only when the last
 * reference to the file is closed; an fd that GLib stops polling and that is closed while another reference to its
 * file exists (a rendering fence the web process holds too) leaves an entry that EPOLL_CTL_DEL by the number cannot
 * reach any more (EBADF, or ENOENT when the number belongs to another file). Such an entry keeps reporting its file's
 * readiness. The set is kept incrementally while every epoll_ctl() succeeds; whenever a DEL fails, or a MOD of a
 * number that was in the set fails, the whole set is replaced by a new epoll fd holding just the current fds
 * (sd_event_source_set_io_fd()), and the old epoll fd is closed with its stale entries.
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
    uint64_t dispatches;        /* iterations that checked and dispatched the context */
    uint64_t max_dispatch_us;   /* the longest check + dispatch */
    uint64_t total_dispatch_us; /* all of them together */
    uint64_t slow_dispatches;   /* those longer than UCFP_GLIB_SLOW_US */
    uint64_t epoll_rebuilds;    /* epoll sets replaced because an entry could not be removed or changed */
};

/* one frame period at 60 Hz */
#define UCFP_GLIB_SLOW_US 16667

/* flutterpi_post_platform_task() of flutter-pi.h: runs callback(userdata) as a platform task, after the tasks posted
 * before it; 0 or an errno. The tests pass their own. */
typedef int (*ucfp_post_task_fn)(int (*callback)(void *userdata), void *userdata);

/* Acquires g_main_context_default() for the calling thread and starts iterating it. NULL with a reason when another
 * thread owns the context or an fd could not be created. */
struct ucfp_glib_loop *ucfp_glib_loop_new(ucfp_add_io_fn add_io, ucfp_post_task_fn post_task, char *reason,
                                          size_t reason_size);

/* Stops iterating and releases the context. A GLib task flutter-pi has not run yet does nothing when it runs. */
void ucfp_glib_loop_destroy(struct ucfp_glib_loop *loop);

/* Prepares and queries the context again: call it after calling into GLib or the library from outside a dispatch
 * (a platform message, a platform task). Does nothing inside a dispatch, which re-arms when it ends, or while a GLib
 * task is posted, which prepares and queries when it has dispatched. */
void ucfp_glib_loop_rearm(struct ucfp_glib_loop *loop);

void ucfp_glib_loop_get_stats(const struct ucfp_glib_loop *loop, struct ucfp_glib_stats *stats);

#endif /* UCFP_GLIB_LOOP_H */
