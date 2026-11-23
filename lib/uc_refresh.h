/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Display refresh rate of libucwebview: where the rate WebKit is paced with comes from, and the arithmetic of the
 * vsync ticks (uc_vsync.c). GLib only, so that the rules are unit-testable on a host without WPE.
 *
 * Precedence (highest first):
 *   1. UC_WEBVIEW_REFRESH_RATE=<Hz> in the environment, read once by uc_webview_global_init(): for panels whose
 *      DRM mode the host rounds (Qt reports the integer vrefresh, 60 for the Remote Two's 59.600 Hz) and for
 *      experiments;
 *   2. the host's value, uc_webview_global_set_refresh_rate() (remote-ui: QScreen::refreshRate());
 *   3. 60 Hz, what WebKit assumed before (WebCore::FullSpeedFramesPerSecond, AnimationFrameRate.h:56).
 * Valid rates are 20..240 Hz; values outside are rejected, not clamped. Rates are kept in mHz like
 * WPEScreen:refresh-rate (WPEScreen.cpp:361-370).
 */
#ifndef UC_REFRESH_H
#define UC_REFRESH_H

#include <glib.h>

G_BEGIN_DECLS

#define UC_REFRESH_MIN_MHZ     20000
#define UC_REFRESH_MAX_MHZ     240000
#define UC_REFRESH_DEFAULT_MHZ 60000

typedef enum {
    UC_REFRESH_SOURCE_DEFAULT = 0,  /* nothing known: 60 Hz */
    UC_REFRESH_SOURCE_HOST,         /* uc_webview_global_set_refresh_rate() */
    UC_REFRESH_SOURCE_ENV,          /* UC_WEBVIEW_REFRESH_RATE */
} UcRefreshSource;

/* hz in mHz (rounded) when it is a finite number in 20..240 Hz, else 0. */
int          uc_refresh_mhz_from_hz     (double hz);
/* A UC_WEBVIEW_REFRESH_RATE value: a plain decimal number of Hz ("50", "49.999", "59.6"; surrounding blanks are
 * ignored; no sign, exponent, hex, unit or decimal comma), in 20..240 Hz. Returns mHz, or 0 for NULL, "", a
 * malformed value or one out of range. */
int          uc_refresh_parse           (const char *value);
/* The rate to use: env_mhz if > 0, else host_mhz if > 0, else UC_REFRESH_DEFAULT_MHZ. source may be NULL. */
int          uc_refresh_effective       (int env_mhz, int host_mhz, UcRefreshSource *source);
/* "UC_WEBVIEW_REFRESH_RATE", "host", "default"; never NULL. */
const char  *uc_refresh_source_name     (UcRefreshSource source);
/* The rate (mHz) the vsync thread ticks WebKit's display link at for a display refresh rate (mHz, > 0): 1 % above
 * it. A display link slower than the panel would now and then leave a refresh without a new frame; a slightly faster
 * one costs nothing where the host reports frames as it presents them (remote-ui), because WebKit keeps a tick that
 * arrives while the previous frame is still being composited and serves it right after that composite
 * (LayerTreeHost.cpp:141-153, 438-458), so pages that draw stay locked to the presentation. The 1 % covers a rate the
 * host rounded (Qt reports the integer DRM vrefresh, 0.5 Hz off at most). */
#define UC_REFRESH_TICK_HEADROOM_PERCENT 1
int          uc_refresh_tick_mhz        (int refresh_mhz);
/* Nanoseconds between two ticks at refresh_mhz (> 0), rounded: 20000000 for 50 Hz, 20000400 for 49.999 Hz. */
gint64       uc_refresh_period_ns       (int refresh_mhz);
/* The tick after the one at `last` (all in ns, period > 0): last + period; after a stall that missed whole ticks,
 * the latest missed tick on the same phase instead (it is due at once, the older ones are dropped). */
gint64       uc_refresh_next_tick       (gint64 last, gint64 now, gint64 period);

G_END_DECLS

#endif /* UC_REFRESH_H */
