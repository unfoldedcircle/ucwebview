/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --link-test of ucwebview-test (library 0.6.2): taps the elements of a built-in page through uc_webview_touch(), the
 * path remote-ui's touches take, and checks what each tap did: a same-document link, an inline and a block link to a
 * second page, a checkbox and its label, a target=_blank link, buttons calling window.open() and window.close(). The
 * pages come from a small HTTP server inside the tool (GIO, 127.0.0.1, no network needed). Every variant (still or
 * sliding taps, with or without the library's pinch gesture controller, with or without the request callbacks of
 * library 0.7.0, after a script dialog) runs in a view of its own and prints PASS/FAIL per element. Since 0.6.3 the
 * first element of every variant checks that the page's generic font families have metrics (no tap): a firmware
 * whose fonts do not match the generic families gives inline text no height, and no tap can reach a text link.
 */
#ifndef UCWEBVIEW_TEST_LINKS_H
#define UCWEBVIEW_TEST_LINKS_H

#include <glib.h>

G_BEGIN_DECLS

extern const GOptionEntry link_test_entries[];

/* --link-test was given */
gboolean link_test_mode            (void);
/* the options after parsing; prints the problem and returns FALSE on a usage error (exit 3) */
gboolean link_test_check_options   (void);
/* the default --timeout of the link test in seconds */
int      link_test_default_timeout (void);
/* After uc_webview_global_init(): runs every variant in views of width x height at scale in a main loop of its own,
 * prints the results and returns the exit code: 0 every check passed, 1 a check failed or the web process died,
 * 2 timeout or the server could not start. */
int      link_test_run             (int width, int height, double scale, int timeout_s);

G_END_DECLS

#endif /* UCWEBVIEW_TEST_LINKS_H */
