/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * --device-test of ucwebview-test (library 0.9.0): the device state API of a page (uc_webview_set_device_api(),
 * uc_webview_set_device_state(), uc_webview_device_event()) end to end in WebKit. A page from uc_webview_load_html()
 * (no server, no network) records every event of window.deviceHost, its battery and its document, and reports its
 * state and record in its title by itself whenever they change, so that the tool runs no script in it: a script of
 * WebKit's API counts as a user gesture (uc_webview_run_javascript()), and the page must show that the library's
 * deliveries do not (navigator.userActivation.hasBeenActive stays false, and the page's beforeunload handler never
 * makes WebKit ask "leave this page?" at the next load). Steps: the state the page starts with and that no frame
 * gets it, a state change, an unchanged state, suspend, resume with the time slept, the power source and level,
 * invalid input, a reload, a page of another site (another web process), the hidden page throttling (a 100 ms interval
 * while the view is unmapped), a killed web process, the API switched off and on again, and a control with
 * uc_webview_run_javascript() that shows the activation checks bite. PASS/FAIL per step, a summary, exit 0 only when
 * all pass.
 */
#ifndef UCWEBVIEW_TEST_DEVICE_H
#define UCWEBVIEW_TEST_DEVICE_H

#include <glib.h>

G_BEGIN_DECLS

extern const GOptionEntry device_test_entries[];

/* --device-test was given */
gboolean device_test_mode            (void);
/* the default --timeout of the test in seconds */
int      device_test_default_timeout (void);
/* After uc_webview_global_init(): runs the steps in a view of width x height at scale in a main loop of its own, prints
 * the results and returns the exit code: 0 every step passed, 1 a step failed or the web process died outside the
 * step that kills it, 2 timeout or the view could not be created. */
int      device_test_run             (int width, int height, double scale, int timeout_s);

G_END_DECLS

#endif /* UCWEBVIEW_TEST_DEVICE_H */
