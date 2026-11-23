/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Display refresh rate configuration and vsync tick arithmetic, see uc_refresh.h.
 */
#include "uc_refresh.h"

int
uc_refresh_mhz_from_hz (double hz)
{
    int mhz;

    /* also false for NaN and the infinities; the margin lets values that round into the range through */
    if (!(hz > 19.0 && hz < 241.0))
        return 0;
    mhz = (int) (hz * 1000.0 + 0.5);   /* positive: rounds half up without libm */
    if (mhz < UC_REFRESH_MIN_MHZ || mhz > UC_REFRESH_MAX_MHZ)
        return 0;
    return mhz;
}

int
uc_refresh_parse (const char *value)
{
    char *text, *end = NULL;
    gboolean digit = FALSE, dot = FALSE, ok = TRUE;
    double hz;

    if (value == NULL)
        return 0;
    text = g_strstrip (g_strdup (value));
    /* digits with at most one '.', nothing else: g_ascii_strtod alone would accept a sign, an exponent, hex
     * floats, "inf" and "nan" */
    for (const char *p = text; *p != '\0' && ok; p++) {
        if (g_ascii_isdigit (*p))
            digit = TRUE;
        else if (*p == '.' && !dot)
            dot = TRUE;
        else
            ok = FALSE;
    }
    if (!ok || !digit) {
        g_free (text);
        return 0;
    }
    hz = g_ascii_strtod (text, &end);
    ok = end != NULL && *end == '\0';
    g_free (text);
    return ok ? uc_refresh_mhz_from_hz (hz) : 0;
}

int
uc_refresh_effective (int env_mhz, int host_mhz, UcRefreshSource *source)
{
    UcRefreshSource chosen = UC_REFRESH_SOURCE_DEFAULT;
    int mhz = UC_REFRESH_DEFAULT_MHZ;

    if (env_mhz > 0) {
        chosen = UC_REFRESH_SOURCE_ENV;
        mhz = env_mhz;
    } else if (host_mhz > 0) {
        chosen = UC_REFRESH_SOURCE_HOST;
        mhz = host_mhz;
    }
    if (source != NULL)
        *source = chosen;
    return mhz;
}

const char *
uc_refresh_source_name (UcRefreshSource source)
{
    switch (source) {
    case UC_REFRESH_SOURCE_ENV:
        return "UC_WEBVIEW_REFRESH_RATE";
    case UC_REFRESH_SOURCE_HOST:
        return "host";
    case UC_REFRESH_SOURCE_DEFAULT:
    default:
        return "default";
    }
}

int
uc_refresh_tick_mhz (int refresh_mhz)
{
    g_return_val_if_fail (refresh_mhz > 0, UC_REFRESH_DEFAULT_MHZ);
    return refresh_mhz + (refresh_mhz * UC_REFRESH_TICK_HEADROOM_PERCENT + 50) / 100;
}

gint64
uc_refresh_period_ns (int refresh_mhz)
{
    g_return_val_if_fail (refresh_mhz > 0, G_GINT64_CONSTANT (1000000000));
    /* 1 s = 1e12 ns * mHz / mHz, rounded to the nearest ns */
    return (G_GINT64_CONSTANT (1000000000000) + refresh_mhz / 2) / refresh_mhz;
}

gint64
uc_refresh_next_tick (gint64 last, gint64 now, gint64 period)
{
    gint64 next = last + period;

    if (period <= 0)
        return now;
    if (next + period <= now) {
        /* missed at least one whole tick (a stalled thread, a suspended system): keep the phase, skip the backlog */
        next += ((now - next) / period) * period;
    }
    return next;
}
