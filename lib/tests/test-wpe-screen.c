/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host test of UcWpeScreen/UcWpeSyncObserver (uc_wpe_screen.c) against a fake of WPEScreen and
 * WPEScreenSyncObserver (tests/fake-wpe-screen.c) compiled with the generated 2.54.1 or 2.54.2 headers:
 * `make check-wpe-fake WPE_HOST_INCLUDE=<dir>`. The test plays WebKit's part the way
 * DisplayVBlankMonitorWPE.cpp:36-121 does: it takes the observer, adds a callback to start it, expects syncs
 * from another thread at the screen's rate, removes the callback to stop it and drops its reference. How it takes
 * the observer depends on the release, which the fake reports through wpe_platform_get_*_version(): up to 2.54.1
 * it adopts the returned pointer, from 2.54.2 on it takes a reference of its own. Each test runs for both.
 */
#include "uc_internal.h"

void fake_wpe_set_version (guint major, guint minor, guint micro);   /* tests/fake-wpe-screen.c */

typedef struct {
    guint major, minor, micro;
    gboolean adopts;    /* DisplayVBlankMonitorWPE.cpp:44: adoptGRef (TRUE) or a GRefPtr of the raw pointer */
} Release;

static const Release release_2_54_1 = { 2, 54, 1, TRUE };
static const Release release_2_54_2 = { 2, 54, 2, FALSE };

typedef struct {
    GMutex lock;
    guint syncs;
    GThread *thread;
} Probe;

static void
on_sync (WPEScreenSyncObserver *observer, gpointer user_data)
{
    Probe *p = user_data;
    (void) observer;
    g_mutex_lock (&p->lock);
    p->syncs++;
    p->thread = g_thread_self ();
    g_mutex_unlock (&p->lock);
}

static guint
probe_syncs (Probe *p)
{
    guint n;
    g_mutex_lock (&p->lock);
    n = p->syncs;
    g_mutex_unlock (&p->lock);
    return n;
}

static void
sleep_ms (int ms)
{
    g_usleep ((gulong) ms * 1000);
}

/* DisplayVBlankMonitorWPE::create() of the given release: the reference it holds afterwards */
static WPEScreenSyncObserver *
webkit_take_observer (const Release *release, UcWpeScreen *screen)
{
    WPEScreenSyncObserver *observer;

    fake_wpe_set_version (release->major, release->minor, release->micro);
    observer = wpe_screen_get_sync_observer (WPE_SCREEN (screen));
    if (observer != NULL && !release->adopts)
        g_object_ref (observer);
    return observer;
}

/* DisplayVBlankMonitorWPE::invalidate(): m_observer = nullptr */
static void
webkit_drop_observer (WPEScreenSyncObserver *observer)
{
    g_object_unref (observer);
}

static void
test_observer_drives_webkit_callbacks (gconstpointer data)
{
    const Release *release = data;
    Probe p = { .syncs = 0, .thread = NULL };
    UcWpeScreen *screen;
    WPEScreenSyncObserver *observer, *again;
    gpointer weak_observer;
    guint id, before;

    g_mutex_init (&p.lock);
    screen = uc_wpe_screen_new (1);
    g_assert_cmpuint (wpe_screen_get_id (WPE_SCREEN (screen)), ==, 1);
    uc_wpe_screen_set_refresh_rate (screen, 200000);
    g_assert_cmpint (wpe_screen_get_refresh_rate (WPE_SCREEN (screen)), ==, 200000);

    observer = webkit_take_observer (release, screen);
    g_assert_nonnull (observer);
    g_assert_cmpstr (G_OBJECT_TYPE_NAME (observer), ==, "UcWpeSyncObserver");
    g_assert_cmpuint (G_OBJECT (observer)->ref_count, ==, 2);   /* the screen's and WebKit's */
    again = webkit_take_observer (release, screen);
    g_assert_true (again == observer);
    g_assert_cmpuint (G_OBJECT (observer)->ref_count, ==, 3);
    webkit_drop_observer (again);
    g_assert_cmpuint (G_OBJECT (observer)->ref_count, ==, 2);
    weak_observer = observer;
    g_object_add_weak_pointer (G_OBJECT (observer), &weak_observer);

    /* nothing ticks before the display link starts */
    sleep_ms (30);
    g_assert_cmpuint (probe_syncs (&p), ==, 0);

    /* DisplayVBlankMonitorWPE::start: the first callback starts the observer; syncs come from its thread */
    id = wpe_screen_sync_observer_add_callback (observer, on_sync, &p, NULL);
    sleep_ms (100);
    before = probe_syncs (&p);
    g_test_message ("%u syncs in 100 ms at 200 Hz", before);
    g_assert_cmpuint (before, >=, 10);
    g_assert_cmpuint (before, <=, 21);
    g_mutex_lock (&p.lock);
    g_assert_true (p.thread != NULL && p.thread != g_thread_self ());
    g_mutex_unlock (&p.lock);

    /* the rate follows uc_wpe_screen_set_refresh_rate() at once: 50 ms ticks, 2 in 120 ms (+1 already running) */
    uc_wpe_screen_set_refresh_rate (screen, 20000);
    g_assert_cmpint (wpe_screen_get_refresh_rate (WPE_SCREEN (screen)), ==, 20000);
    before = probe_syncs (&p);
    sleep_ms (120);
    g_assert_cmpuint (probe_syncs (&p) - before, <=, 4);

    /* DisplayVBlankMonitorWPE::stop: removing the last callback stops the ticks */
    wpe_screen_sync_observer_remove_callback (observer, id);
    sleep_ms (10);
    before = probe_syncs (&p);
    sleep_ms (120);
    g_assert_cmpuint (probe_syncs (&p), ==, before);

    /* DisplayVBlankMonitorWPE::invalidate drops WebKit's reference: the screen still holds the observer, and the
     * screen's dispose releases the last one (no leaked reference) */
    webkit_drop_observer (observer);
    g_assert_nonnull (weak_observer);
    g_assert_cmpuint (G_OBJECT (observer)->ref_count, ==, 1);
    g_object_unref (screen);
    g_assert_null (weak_observer);
    g_mutex_clear (&p.lock);
}

static void
test_display_link_outlives_the_screen (gconstpointer data)
{
    /* uc_webview_global_shutdown() drops the display (and screen) while WebKit's display link, which lives as long
     * as the process pool, still holds its observer: the observer keeps ticking until the callback goes */
    const Release *release = data;
    Probe p = { .syncs = 0, .thread = NULL };
    UcWpeScreen *screen = uc_wpe_screen_new (7);
    WPEScreenSyncObserver *observer;
    gpointer weak_observer;
    guint id, before;

    g_mutex_init (&p.lock);
    uc_wpe_screen_set_refresh_rate (screen, 200000);
    observer = webkit_take_observer (release, screen);
    weak_observer = observer;
    g_object_add_weak_pointer (G_OBJECT (observer), &weak_observer);
    id = wpe_screen_sync_observer_add_callback (observer, on_sync, &p, NULL);
    g_object_unref (screen);
    g_assert_nonnull (weak_observer);
    g_assert_cmpuint (G_OBJECT (observer)->ref_count, ==, 1);   /* WebKit's only */
    sleep_ms (60);
    before = probe_syncs (&p);
    g_assert_cmpuint (before, >, 0);
    wpe_screen_sync_observer_remove_callback (observer, id);
    webkit_drop_observer (observer);    /* last reference: dispose joins the parked vsync thread */
    g_assert_null (weak_observer);
    g_mutex_clear (&p.lock);
}

static void
test_ownership_by_release (void)
{
    /* the version boundary of webkit_adopts_sync_observer(): WebKit's reference is the only one besides the
     * screen's after create(), and nothing is left after invalidate() and the screen's dispose */
    static const Release releases[] = {
        { 2, 52, 9, TRUE }, { 2, 54, 0, TRUE }, { 2, 54, 1, TRUE },
        { 2, 54, 2, FALSE }, { 2, 54, 3, FALSE }, { 2, 56, 0, FALSE }, { 3, 0, 0, FALSE },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (releases); i++) {
        UcWpeScreen *screen = uc_wpe_screen_new (1);
        WPEScreenSyncObserver *observer = webkit_take_observer (&releases[i], screen);
        gpointer weak_observer = observer;

        g_test_message ("%u.%u.%u: WebKit %s", releases[i].major, releases[i].minor, releases[i].micro,
                        releases[i].adopts ? "adopts" : "takes a reference");
        g_object_add_weak_pointer (G_OBJECT (observer), &weak_observer);
        g_assert_cmpuint (G_OBJECT (observer)->ref_count, ==, 2);
        webkit_drop_observer (observer);
        g_assert_cmpuint (G_OBJECT (observer)->ref_count, ==, 1);
        g_object_unref (screen);
        g_assert_null (weak_observer);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/wpe-screen/2.54.1/observer-drives-webkit-callbacks", &release_2_54_1,
                          test_observer_drives_webkit_callbacks);
    g_test_add_data_func ("/wpe-screen/2.54.2/observer-drives-webkit-callbacks", &release_2_54_2,
                          test_observer_drives_webkit_callbacks);
    g_test_add_data_func ("/wpe-screen/2.54.1/display-link-outlives-the-screen", &release_2_54_1,
                          test_display_link_outlives_the_screen);
    g_test_add_data_func ("/wpe-screen/2.54.2/display-link-outlives-the-screen", &release_2_54_2,
                          test_display_link_outlives_the_screen);
    g_test_add_func ("/wpe-screen/ownership-by-release", test_ownership_by_release);
    return g_test_run ();
}
