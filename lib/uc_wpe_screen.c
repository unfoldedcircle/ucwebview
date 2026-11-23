/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * UcWpeScreen and UcWpeSyncObserver: the virtual screen of libucwebview's display and the vsync source that paces
 * WebKit on it. Uses only the WPEScreen/WPEScreenSyncObserver API, so that tests/test-wpe-screen.c can run it on a
 * host against a fake of those two classes (tests/fake-wpe-screen.c).
 */
#include "uc_internal.h"
#include "uc_refresh.h"
#include "uc_vsync.h"

/*
 * Frame pacing
 * ============
 * WebKit runs requestAnimationFrame and main-thread rendering updates from a display link per screen
 * (UIProcess/DisplayLink.cpp), created at the first page load from the screen of the view
 * (DisplayLinkCollection::nominalFramesPerSecondForDisplay, DisplayLink.cpp:256-261). Its vblank monitor is
 * DisplayVBlankMonitorWPE when the screen has a WPEScreenSyncObserver, else a timer
 * (DisplayVBlankMonitor.cpp:54-61):
 *  - the timer ignores the screen: it sleeps 1000 / 60 ms per tick (WebCore::FullSpeedFramesPerSecond,
 *    DisplayVBlankMonitorTimer.cpp:40-48, AnimationFrameRate.h:56), about 61 Hz whatever the panel does;
 *  - DisplayVBlankMonitorWPE ticks on the observer's syncs and takes wpe_screen_get_refresh_rate() / 1000 as the
 *    nominal frame rate (DisplayVBlankMonitorWPE.cpp:44-50), from which the web process derives its preferred
 *    rendering update rate (DisplayLinkGLib.cpp:47-48, Page.cpp:2773, AnimationFrameRate.cpp:47-86).
 * So the refresh rate alone changes nothing; it takes an observer. The base WPEScreen class has one when built
 * with libdrm (WPEScreen.cpp:214-249, 375-377), but it looks for a connected DRM connector whose size in mm
 * equals the screen's physical size, which a virtual screen sized after the view never matches, and it opens
 * every DRM primary node of the system on each attempt. UcWpeScreen therefore overrides get_sync_observer with
 * UcWpeSyncObserver, a UcVsync thread (uc_vsync.h) at the configured rate plus 1 % (uc_refresh_tick_mhz(); the
 * rate comes from uc_webview_global_set_refresh_rate() or UC_WEBVIEW_REFRESH_RATE, uc_refresh.h).
 *
 * What the tick rate does and does not decide: frames are back-pressured, WebKit composites the next one only
 * after uc_webview_frame_rendered() (AcceleratedBackingStore.cpp:301-310, ThreadedCompositor.cpp:676-700), and a
 * tick that arrives while the previous update is still waiting for the compositor is kept and served right after
 * that composite (LayerTreeHost.cpp:141-153, 438-458). Accelerated CSS animations do not use the display link at
 * all: the compositor composites again after every frame while they run (ThreadedCompositor.cpp:354, 412-413).
 * So while a page draws, a host that reports frames as it presents them (remote-ui after Qt's page flip) paces it
 * at the panel rate as long as the ticks are not slower than the panel; the surplus tick of a faster link is
 * absorbed, which is why the observer runs 1 % fast. The tick rate itself shows in requestAnimationFrame loops
 * that do not draw, in WebKit's nominal frame rate and in hosts that report frames at once (ucwebview-test).
 *
 * Self-clocked instead of driven by the host's frame reports: uc_webview_frame_rendered() only arrives while
 * frames flow, but the display link has to tick before the first frame of an animation exists (an idle page
 * that starts one would never get its first requestAnimationFrame), and Qt 5.15 emits frameSwapped when the page
 * flip is queued, not when it happens (eglfs swapBuffers: waitForVSync of the previous flip, eglSwapBuffers,
 * presentBuffer, qeglfscontext.cpp:105-117), so it carries no vblank phase; the back-pressure above already locks
 * drawing pages to the presentation.
 */

/* ---- UcWpeSyncObserver ------------------------------------------------------ */

#define UC_TYPE_WPE_SYNC_OBSERVER (uc_wpe_sync_observer_get_type ())
G_DECLARE_FINAL_TYPE (UcWpeSyncObserver, uc_wpe_sync_observer, UC, WPE_SYNC_OBSERVER, WPEScreenSyncObserver)

struct _UcWpeSyncObserver {
    WPEScreenSyncObserver parent;
    UcVsync *vsync;
};

G_DEFINE_FINAL_TYPE (UcWpeSyncObserver, uc_wpe_sync_observer, WPE_TYPE_SCREEN_SYNC_OBSERVER)

static void
uc_wpe_sync_observer_tick (gpointer user_data)
{
    /* vsync thread: run the callbacks WebKit added (DisplayVBlankMonitorWPE.cpp:76-82 -> DisplayLink handler,
     * which must not run on the main thread, DisplayLink.cpp:187), like WPEScreenSyncObserverDRM.cpp does */
    WPE_SCREEN_SYNC_OBSERVER_CLASS (uc_wpe_sync_observer_parent_class)->sync (WPE_SCREEN_SYNC_OBSERVER (user_data));
}

/* start/stop: called by wpe_screen_sync_observer_add/remove_callback() for the first/last callback
 * (WPEScreenSyncObserver.cpp:121-124, 149-152) on the UI thread, while DisplayVBlankMonitorWPE holds its lock;
 * both return at once (a tick in progress may wait for that lock, DisplayVBlankMonitorWPE.cpp:78). */
static void
uc_wpe_sync_observer_start (WPEScreenSyncObserver *observer)
{
    uc_vsync_start (UC_WPE_SYNC_OBSERVER (observer)->vsync);
}

static void
uc_wpe_sync_observer_stop (WPEScreenSyncObserver *observer)
{
    uc_vsync_stop (UC_WPE_SYNC_OBSERVER (observer)->vsync);
}

static void
uc_wpe_sync_observer_dispose (GObject *object)
{
    /* joins the vsync thread: never the last unref from inside a tick, see uc_wpe_screen_get_sync_observer() */
    g_clear_pointer (&UC_WPE_SYNC_OBSERVER (object)->vsync, uc_vsync_free);
    G_OBJECT_CLASS (uc_wpe_sync_observer_parent_class)->dispose (object);
}

static void
uc_wpe_sync_observer_class_init (UcWpeSyncObserverClass *klass)
{
    WPEScreenSyncObserverClass *observer_class = WPE_SCREEN_SYNC_OBSERVER_CLASS (klass);
    G_OBJECT_CLASS (klass)->dispose = uc_wpe_sync_observer_dispose;
    /* both are called unconditionally (WPEScreenSyncObserver.cpp:122-123, 150-151); sync stays the base one */
    observer_class->start = uc_wpe_sync_observer_start;
    observer_class->stop = uc_wpe_sync_observer_stop;
}

static void
uc_wpe_sync_observer_init (UcWpeSyncObserver *self)
{
    self->vsync = uc_vsync_new (uc_refresh_tick_mhz (UC_REFRESH_DEFAULT_MHZ), uc_wpe_sync_observer_tick, self);
}

/* ---- UcWpeScreen ------------------------------------------------------------ */

struct _UcWpeScreen {
    WPEScreen parent;
    UcWpeSyncObserver *observer;
};

G_DEFINE_FINAL_TYPE (UcWpeScreen, uc_wpe_screen, WPE_TYPE_SCREEN)

/* Whether the running WebKit adopts the pointer that get_sync_observer returns. WPEScreen.h documents (transfer
 * none) (WPEScreen.cpp:654), and the only caller is DisplayVBlankMonitorWPE::create(), which drops its reference
 * when the display link goes:
 *  - up to 2.54.1 it adopts the pointer (adoptGRef, DisplayVBlankMonitorWPE.cpp:44), so the vfunc has to hand out a
 *    reference of its own; without it that unref would leave the screen with a dangling observer, or dispose it from
 *    DisplayVBlankMonitorWPE::invalidate() while a tick waits for that monitor's lock;
 *  - from 2.54.2 it takes a reference itself (same line, "Fix screen sync observer leak in
 *    wpe_screen_get_sync_observer()" in the 2.54.2 NEWS), so an extra reference would never be dropped.
 * Asked at run time and not at build time: the rule lives in libWPEWebKit-2.0 (which also carries WPEPlatform and
 * its version functions), and a library built against one release and copied onto a firmware with the other must
 * neither leak nor over-release. The 2.55 development releases are not covered (stable series only). */
static gboolean
webkit_adopts_sync_observer (void)
{
    guint major = wpe_platform_get_major_version ();
    guint minor = wpe_platform_get_minor_version ();
    guint micro = wpe_platform_get_micro_version ();

    return major < 2 || (major == 2 && (minor < 54 || (minor == 54 && micro < 2)));
}

static WPEScreenSyncObserver *
uc_wpe_screen_get_sync_observer (WPEScreen *screen)
{
    UcWpeScreen *self = UC_WPE_SCREEN (screen);

    if (webkit_adopts_sync_observer ())
        return WPE_SCREEN_SYNC_OBSERVER (g_object_ref (self->observer));
    return WPE_SCREEN_SYNC_OBSERVER (self->observer);
}

static void
uc_wpe_screen_dispose (GObject *object)
{
    g_clear_object (&UC_WPE_SCREEN (object)->observer);
    G_OBJECT_CLASS (uc_wpe_screen_parent_class)->dispose (object);
}

static void
uc_wpe_screen_class_init (UcWpeScreenClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = uc_wpe_screen_dispose;
    /* replaces the base class's DRM connector lookup (WPEScreen.cpp:375-377), see "Frame pacing" above */
    WPE_SCREEN_CLASS (klass)->get_sync_observer = uc_wpe_screen_get_sync_observer;
}

static void
uc_wpe_screen_init (UcWpeScreen *screen)
{
    screen->observer = g_object_new (UC_TYPE_WPE_SYNC_OBSERVER, NULL);
}

UcWpeScreen *
uc_wpe_screen_new (guint32 id)
{
    /* "id" is construct-only (WPEScreen.cpp:263-268); 0 would collide with "no display" in WebKit. */
    g_return_val_if_fail (id != 0, NULL);
    return g_object_new (UC_TYPE_WPE_SCREEN, "id", id, NULL);
}

void
uc_wpe_screen_set_refresh_rate (UcWpeScreen *screen, int refresh_mhz)
{
    g_return_if_fail (UC_IS_WPE_SCREEN (screen));
    g_return_if_fail (refresh_mhz > 0);
    /* WebKit reads it when it creates the display link (DisplayVBlankMonitorWPE.cpp:50) as its nominal frame rate;
     * the ticks follow at once, with the headroom of uc_refresh_tick_mhz() */
    wpe_screen_set_refresh_rate (WPE_SCREEN (screen), refresh_mhz);
    uc_vsync_set_rate (screen->observer->vsync, uc_refresh_tick_mhz (refresh_mhz));
}

void
uc_wpe_screen_set_vsync_trace (UcWpeScreen *screen, gboolean trace)
{
    g_return_if_fail (UC_IS_WPE_SCREEN (screen));
    uc_vsync_set_trace (screen->observer->vsync, trace);
}
