/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The request modes of ucwebview-test (library 0.7.0): --dialog-test, --auth-test, --window-test, --download-test.
 * Each loads pages from a small HTTP server inside the tool (libsoup, 127.0.0.1, no network needed), runs a fixed
 * sequence of steps and compares, per step, what the library reported (a transcript of the request and load
 * callbacks) and what the page saw (its title) with what the step expects. The request callbacks are the tool's in
 * every mode: outside the request modes they print the request and answer it like a host without a dialog would.
 */
#ifndef UCWEBVIEW_TEST_REQUESTS_H
#define UCWEBVIEW_TEST_REQUESTS_H

#include "uc_webview.h"

#include <glib.h>

G_BEGIN_DECLS

/* ends the tool's main loop with an exit code (0 ok, 1 failed, 2 timeout) */
typedef void (*RequestTestQuit) (int code);

extern const GOptionEntry request_test_entries[];

/* a request mode was asked for */
gboolean request_test_mode            (void);
/* the options after parsing; prints the problem and returns FALSE on a usage error (exit 3) */
gboolean request_test_check_options   (void);
/* the default --timeout of a request mode in seconds */
int      request_test_default_timeout (void);
/* sets the six request callbacks of cb */
void     request_test_set_callbacks   (uc_webview_callbacks *cb);
/* the tool's quit: outside the request modes a refused document before any commit ends the run ("load refused") */
void     request_test_set_quit        (RequestTestQuit quit);
/* starts the server and the first step; plain: the view came from uc_webview_new() and gets no request callbacks */
gboolean request_test_start           (uc_webview *wv, gboolean plain, RequestTestQuit quit);
/* the tool's own callbacks, forwarded; TRUE: a request mode took the event (no default action of the tool) */
gboolean request_test_load_changed    (int event, const char *uri);
void     request_test_title_changed   (const char *title);
gboolean request_test_terminated      (int reason);
/* around uc_webview_free(): a request callback inside it is a failure */
void     request_test_freeing         (gboolean freeing);
/* after uc_webview_free(): prints the mode's result line, returns the exit code */
int      request_test_finish          (int exit_code);
/* the web processes among the tool's descendants (WPEWebProcess on WPE, WebKitWebProcess on WebKitGTK): their number;
 * with kill_them each gets SIGKILL, and the number is that of the killed ones (also --device-test, 0.9.0) */
int      request_test_web_processes   (gboolean kill_them);

G_END_DECLS

#endif /* UCWEBVIEW_TEST_REQUESTS_H */
