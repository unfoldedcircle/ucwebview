/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --load-fail-test of ucwebview-test (library 0.10.0): load_failed_detail() end to end in WebKit. Loads that fail
 * for a known reason without the network: a host name of .invalid (DNS), a closed port on 127.0.0.1 (refused),
 * 255.255.255.255 (Linux refuses TCP to the broadcast address with ENETUNREACH), 240.0.0.1 (reserved, never answers:
 * WebKit's preconnect gives up after 10 s, uc_load_error.h; needs a default route), a self-signed certificate of an
 * HTTPS server inside the tool (--load-fail-tls-cert, the --site-test certificate), URIs the scheme allowlist refuses;
 * and loads that must bring no detail: a load replaced by a newer one (cancelled), a refused response (attachment),
 * an HTTP error page, an accepted certificate, and a view of a 0.9 host's callback struct. Each failure must bring
 * exactly one load_failed_detail() right before its one load_changed(FAILED), with the same uri. The tls-race step
 * (library 0.10.3) times HTTPS loads of new hosts against WebKit's preconnect, so that some take over the connection of
 * a preconnect that failed on the certificate: neither a TIMEOUT nor a missing page. PASS/FAIL per check, a summary,
 * exit 0 only when all pass.
 */
#ifndef UCWEBVIEW_TEST_LOADFAIL_H
#define UCWEBVIEW_TEST_LOADFAIL_H

#include <glib.h>

G_BEGIN_DECLS

extern const GOptionEntry load_fail_test_entries[];

/* --load-fail-test was given */
gboolean load_fail_test_mode            (void);
/* the options after parsing; prints the problem and returns FALSE on a usage error (exit 3) */
gboolean load_fail_test_check_options   (void);
/* the default --timeout of the test in seconds */
int      load_fail_test_default_timeout (void);
/* After uc_webview_global_init(): runs the checks in views of width x height at scale, prints the results and returns
 * the exit code: 0 every check passed (skipped ones aside), 1 a check failed or the web process died, 2 timeout or the
 * server could not start. */
int      load_fail_test_run             (int width, int height, double scale, int timeout_s);

G_END_DECLS

#endif /* UCWEBVIEW_TEST_LOADFAIL_H */
