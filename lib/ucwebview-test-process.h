/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --process-test of ucwebview-test (library 0.10.4): the web processes WebKit keeps with the library's cache model
 * (uc_cache_model.h) and the back/forward cache of one view. Pages of a server inside the tool (libsoup) on two sites,
 * http://127.0.0.1:P and http://127.0.0.2:Q; each page reports in its title how pageshow came ("a:load", or
 * "a:bfcache" for a page from the back/forward cache). The tool counts the WPEWebProcess processes below its own
 * process in /proc. Steps:
 *
 *   close         a view loads page A of one site and is freed: with the models viewer and browser no web process is
 *                 left within --process-wait seconds; with web-browser WebKit's web process cache keeps it (when the
 *                 system has 2 GB of RAM or more, WebProcessCache.cpp:346-352);
 *   back-forward  a new view loads A, then B of the other site (a process swap), goes back to A and forward to B:
 *                 both from the back/forward cache with browser and web-browser, loaded again with viewer;
 *   swap-close    that view is freed: with viewer and browser at most one web process is left, WebKit's spare process
 *                 (prewarmed after the process swap, independent of the cache model, uc_cache_model.h).
 *
 * The model is the library's, read from UC_WEBVIEW_CACHE_MODEL like the library does. PASS/FAIL per check, INFO lines
 * with the process counts, a summary, exit 0 only when all pass.
 */
#ifndef UCWEBVIEW_TEST_PROCESS_H
#define UCWEBVIEW_TEST_PROCESS_H

#include <glib.h>

G_BEGIN_DECLS

extern const GOptionEntry process_test_entries[];

/* --process-test was given */
gboolean process_test_mode            (void);
/* the options after parsing; prints the problem and returns FALSE on a usage error (exit 3) */
gboolean process_test_check_options   (void);
/* the default --timeout of the test in seconds */
int      process_test_default_timeout (void);
/* After uc_webview_global_init(): runs the steps in views of width x height at scale, prints the results and returns
 * the exit code: 0 every check passed, 1 a check failed or a web process died, 2 timeout or the server could not
 * start. */
int      process_test_run             (int width, int height, double scale, int timeout_s);

G_END_DECLS

#endif /* UCWEBVIEW_TEST_PROCESS_H */
