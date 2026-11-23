/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --ime-swap-test of ucwebview-test (library 0.7.5): a page focuses an input itself when it loads (autofocus, like
 * the web-configurator's PIN field), then the view navigates to a page of another site, where WebKit moves the page
 * to another web process, or of the same site (the control). The test checks the ime_focus reports a host gets:
 * the replay of the element while the view loses and regains the focus before the navigation, ime_focus(0) at the
 * navigation, no ime_focus(1) for the gone element when the view loses and regains the focus
 * (remote-ui does that around its dialogs), ime_focus(1) with the right purpose when the new page focuses a field,
 * and ime_focus(0) at its blur. WebKit 2.54 keeps the gone element's input method state after a web process swap
 * (uc_ime_state.h, "Focus reports"); up to library 0.7.4 the cross-site variants fail. The pages come from a small
 * HTTP server inside the tool (GIO) on 127.0.0.1 and localhost, two sites, no network needed. Every variant runs in a
 * view of its own and prints PASS/FAIL per check.
 */
#ifndef UCWEBVIEW_TEST_IME_H
#define UCWEBVIEW_TEST_IME_H

#include <glib.h>

G_BEGIN_DECLS

extern const GOptionEntry ime_swap_test_entries[];

/* --ime-swap-test was given */
gboolean ime_swap_test_mode            (void);
/* the options after parsing; prints the problem and returns FALSE on a usage error (exit 3) */
gboolean ime_swap_test_check_options   (void);
/* the default --timeout of the test in seconds */
int      ime_swap_test_default_timeout (void);
/* After uc_webview_global_init(): runs every variant in views of width x height at scale in a main loop of its own,
 * prints the results and returns the exit code: 0 every check passed, 1 a check failed or the web process died,
 * 2 timeout or the server could not start. */
int      ime_swap_test_run             (int width, int height, double scale, int timeout_s);

G_END_DECLS

#endif /* UCWEBVIEW_TEST_IME_H */
