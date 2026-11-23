/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --site-test of ucwebview-test (library 0.8.0): the site settings of uc_webview.h ("Site settings") against pages of
 * HTTP and HTTPS servers inside the tool (libsoup on 127.0.0.1, localhost and 127.0.0.2, no network needed). It
 * checks what reaches the server (User-Agent, Cookie and the origin headers per request) and what the page sees
 * (navigator.userAgent, document.cookie, prefers-color-scheme), the certificate exceptions with a certificate given by
 * --site-tls-cert, clearing website data and, with --data-dir, that cookies with an expiry time are written to the
 * data directory and come back in the next run (--site-expect-persisted). PASS/FAIL per check, a summary at the end.
 */
#ifndef UCWEBVIEW_TEST_SITE_H
#define UCWEBVIEW_TEST_SITE_H

#include <glib.h>

G_BEGIN_DECLS

extern const GOptionEntry site_test_entries[];

/* --site-test was given */
gboolean site_test_mode            (void);
/* the options after parsing; prints the problem and returns FALSE on a usage error (exit 3) */
gboolean site_test_check_options   (void);
/* the default --timeout of the test in seconds */
int      site_test_default_timeout (void);
/* After uc_webview_global_init() with data_dir (may be NULL): runs the checks in a view of width x height at scale,
 * prints the results and returns the exit code: 0 every check passed (skipped ones aside), 1 a check failed or the
 * web process died, 2 timeout or a server could not start. */
int      site_test_run             (int width, int height, double scale, const char *data_dir, int timeout_s);

G_END_DECLS

#endif /* UCWEBVIEW_TEST_SITE_H */
