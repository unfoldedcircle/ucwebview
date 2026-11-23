/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_refresh.c (GLib only): run with `make check`.
 */
#include "uc_refresh.h"

#include <math.h>

static void
test_from_hz (void)
{
    g_assert_cmpint (uc_refresh_mhz_from_hz (50.0), ==, 50000);
    g_assert_cmpint (uc_refresh_mhz_from_hz (49.999), ==, 49999);
    g_assert_cmpint (uc_refresh_mhz_from_hz (59.6), ==, 59600);
    g_assert_cmpint (uc_refresh_mhz_from_hz (60.0), ==, 60000);
    g_assert_cmpint (uc_refresh_mhz_from_hz (20.0), ==, 20000);
    g_assert_cmpint (uc_refresh_mhz_from_hz (240.0), ==, 240000);
    g_assert_cmpint (uc_refresh_mhz_from_hz (240.0004), ==, 240000);   /* rounds into the range */
    g_assert_cmpint (uc_refresh_mhz_from_hz (19.999), ==, 0);
    g_assert_cmpint (uc_refresh_mhz_from_hz (240.001), ==, 0);
    g_assert_cmpint (uc_refresh_mhz_from_hz (0.0), ==, 0);
    g_assert_cmpint (uc_refresh_mhz_from_hz (-50.0), ==, 0);
    g_assert_cmpint (uc_refresh_mhz_from_hz (NAN), ==, 0);
    g_assert_cmpint (uc_refresh_mhz_from_hz (INFINITY), ==, 0);
}

static void
test_parse (void)
{
    g_assert_cmpint (uc_refresh_parse ("50"), ==, 50000);
    g_assert_cmpint (uc_refresh_parse ("49.999"), ==, 49999);
    g_assert_cmpint (uc_refresh_parse ("59.6"), ==, 59600);
    g_assert_cmpint (uc_refresh_parse (" 60 "), ==, 60000);
    g_assert_cmpint (uc_refresh_parse ("\t75.0\n"), ==, 75000);
    g_assert_cmpint (uc_refresh_parse ("50."), ==, 50000);
    g_assert_cmpint (uc_refresh_parse ("020"), ==, 20000);
    g_assert_cmpint (uc_refresh_parse ("240"), ==, 240000);
    g_assert_cmpint (uc_refresh_parse ("49.9994"), ==, 49999);
    /* out of range */
    g_assert_cmpint (uc_refresh_parse ("19.9"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("241"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("0"), ==, 0);
    g_assert_cmpint (uc_refresh_parse (".5"), ==, 0);
    /* malformed */
    g_assert_cmpint (uc_refresh_parse (NULL), ==, 0);
    g_assert_cmpint (uc_refresh_parse (""), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("   "), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("."), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("abc"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("50Hz"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("50 Hz"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("5e1"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("+50"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("-50"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("0x32"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("nan"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("inf"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("50,0"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("1.2.3"), ==, 0);
    g_assert_cmpint (uc_refresh_parse ("5 0"), ==, 0);
}

static void
test_precedence (void)
{
    UcRefreshSource source = UC_REFRESH_SOURCE_ENV;

    /* nothing known: 60 Hz like WebKit's fallback */
    g_assert_cmpint (uc_refresh_effective (0, 0, &source), ==, 60000);
    g_assert_cmpint (source, ==, UC_REFRESH_SOURCE_DEFAULT);
    /* the host's value (Qt's QScreen::refreshRate()) */
    g_assert_cmpint (uc_refresh_effective (0, 50000, &source), ==, 50000);
    g_assert_cmpint (source, ==, UC_REFRESH_SOURCE_HOST);
    /* the environment wins over the host: the Remote Two's 59.600 Hz panel that Qt reports as 60 */
    g_assert_cmpint (uc_refresh_effective (59600, 60000, &source), ==, 59600);
    g_assert_cmpint (source, ==, UC_REFRESH_SOURCE_ENV);
    g_assert_cmpint (uc_refresh_effective (59600, 0, &source), ==, 59600);
    g_assert_cmpint (source, ==, UC_REFRESH_SOURCE_ENV);
    /* source is optional */
    g_assert_cmpint (uc_refresh_effective (0, 75000, NULL), ==, 75000);

    g_assert_cmpstr (uc_refresh_source_name (UC_REFRESH_SOURCE_ENV), ==, "UC_WEBVIEW_REFRESH_RATE");
    g_assert_cmpstr (uc_refresh_source_name (UC_REFRESH_SOURCE_HOST), ==, "host");
    g_assert_cmpstr (uc_refresh_source_name (UC_REFRESH_SOURCE_DEFAULT), ==, "default");
}

static void
test_period (void)
{
    g_assert_cmpint (uc_refresh_period_ns (50000), ==, G_GINT64_CONSTANT (20000000));
    g_assert_cmpint (uc_refresh_period_ns (49999), ==, G_GINT64_CONSTANT (20000400));   /* the Remote 3 panel */
    g_assert_cmpint (uc_refresh_period_ns (59600), ==, G_GINT64_CONSTANT (16778523));   /* the Remote Two panel */
    g_assert_cmpint (uc_refresh_period_ns (60000), ==, G_GINT64_CONSTANT (16666667));
    g_assert_cmpint (uc_refresh_period_ns (240000), ==, G_GINT64_CONSTANT (4166667));
    g_assert_cmpint (uc_refresh_period_ns (20000), ==, G_GINT64_CONSTANT (50000000));
}

static void
test_tick_headroom (void)
{
    /* the display link ticks 1 % above the display: never slower than a panel the host rounded down */
    g_assert_cmpint (uc_refresh_tick_mhz (50000), ==, 50500);
    g_assert_cmpint (uc_refresh_tick_mhz (49999), ==, 50499);
    g_assert_cmpint (uc_refresh_tick_mhz (60000), ==, 60600);
    g_assert_cmpint (uc_refresh_tick_mhz (59600), ==, 60196);
    g_assert_cmpint (uc_refresh_tick_mhz (20000), ==, 20200);
    g_assert_cmpint (uc_refresh_tick_mhz (240000), ==, 242400);
    /* Qt's worst rounding at 50 Hz: a 50.49 Hz panel reported as 50 still gets ticks at or above its rate */
    g_assert_cmpint (uc_refresh_tick_mhz (50000), >=, 50490);
}

static void
test_next_tick (void)
{
    /* regular: one period after the last tick, whether now is before or after it */
    g_assert_cmpint (uc_refresh_next_tick (0, 5, 20), ==, 20);
    g_assert_cmpint (uc_refresh_next_tick (0, 20, 20), ==, 20);
    g_assert_cmpint (uc_refresh_next_tick (0, 39, 20), ==, 20);   /* late but within one period: due now */
    /* a stall missed whole ticks: the latest missed one, same phase */
    g_assert_cmpint (uc_refresh_next_tick (0, 40, 20), ==, 40);
    g_assert_cmpint (uc_refresh_next_tick (0, 45, 20), ==, 40);
    g_assert_cmpint (uc_refresh_next_tick (0, 1005, 20), ==, 1000);
    g_assert_cmpint (uc_refresh_next_tick (7, 1005, 20), ==, 987);
    /* no drift: 1000 ticks of 49.999 Hz from 0 land on the exact multiple */
    {
        gint64 period = uc_refresh_period_ns (49999), t = 0;
        for (int i = 0; i < 1000; i++)
            t = uc_refresh_next_tick (t, t, period);
        g_assert_cmpint (t, ==, 1000 * period);
    }
    /* nonsense period: due now instead of looping */
    g_assert_cmpint (uc_refresh_next_tick (0, 5, 0), ==, 5);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/refresh/from-hz", test_from_hz);
    g_test_add_func ("/refresh/parse", test_parse);
    g_test_add_func ("/refresh/precedence", test_precedence);
    g_test_add_func ("/refresh/period", test_period);
    g_test_add_func ("/refresh/tick-headroom", test_tick_headroom);
    g_test_add_func ("/refresh/next-tick", test_next_tick);
    return g_test_run ();
}
