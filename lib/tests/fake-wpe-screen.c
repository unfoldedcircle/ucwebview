/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * A fake of WPEPlatform's WPEScreen and WPEScreenSyncObserver for tests/test-wpe-screen.c, compiled against the
 * generated 2.54.1 headers (the class and instance structs are the real ones). It reproduces what libucwebview
 * relies on in WPE WebKit 2.54.1:
 *  - add_callback() calls the class's start for the first callback, remove_callback() calls stop after the last
 *    one, outside the callback lock (WPEScreenSyncObserver.cpp:105-153);
 *  - the base class's sync vfunc copies the callbacks under the lock and calls them without it (:71-89);
 *  - wpe_screen_get_sync_observer() returns the class vfunc's pointer unchanged (WPEScreen.cpp:656-665);
 *  - wpe_screen_set_refresh_rate() ignores -1 and unchanged values (WPEScreen.cpp:636-646);
 *  - wpe_platform_get_major/minor/micro_version() report the release, settable with fake_wpe_set_version() (default
 *    2.54.1) because the ownership of get_sync_observer's result depends on it (uc_wpe_screen.c).
 * The base class's DRM lookup (WPEScreen.cpp:214-249) is left out: UcWpeScreen overrides that vfunc.
 * The same file builds with the generated 2.54.2 headers: the two classes are unchanged in 2.54.2.
 */
#include <wpe/wpe-platform.h>

/* ---- version (WPEVersion.h) ----------------------------------------------------------------- */

static guint fake_version[3] = { 2, 54, 1 };

void fake_wpe_set_version (guint major, guint minor, guint micro);

void
fake_wpe_set_version (guint major, guint minor, guint micro)
{
    fake_version[0] = major;
    fake_version[1] = minor;
    fake_version[2] = micro;
}

guint
wpe_platform_get_major_version (void)
{
    return fake_version[0];
}

guint
wpe_platform_get_minor_version (void)
{
    return fake_version[1];
}

guint
wpe_platform_get_micro_version (void)
{
    return fake_version[2];
}

/* ---- WPEScreenSyncObserver --------------------------------------------------------------- */

typedef struct {
    WPEScreenSyncObserverSyncFunc func;
    gpointer user_data;
    GDestroyNotify destroy;
} FakeCallback;

struct _WPEScreenSyncObserverPrivate {
    GMutex lock;
    GHashTable *callbacks;      /* id -> FakeCallback */
};

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (WPEScreenSyncObserver, wpe_screen_sync_observer, G_TYPE_OBJECT)

static void
fake_callback_free (gpointer data)
{
    FakeCallback *callback = data;
    if (callback->destroy != NULL)
        callback->destroy (callback->user_data);
    g_free (callback);
}

static void
fake_sync (WPEScreenSyncObserver *observer)
{
    WPEScreenSyncObserverPrivate *priv = observer->priv;
    GPtrArray *copy = g_ptr_array_new_with_free_func (g_free);
    GHashTableIter iter;
    gpointer value;

    g_mutex_lock (&priv->lock);
    g_hash_table_iter_init (&iter, priv->callbacks);
    while (g_hash_table_iter_next (&iter, NULL, &value))
        g_ptr_array_add (copy, g_memdup2 (value, sizeof (FakeCallback)));
    g_mutex_unlock (&priv->lock);
    for (guint i = 0; i < copy->len; i++) {
        FakeCallback *callback = g_ptr_array_index (copy, i);
        callback->func (observer, callback->user_data);
    }
    g_ptr_array_unref (copy);
}

static void
wpe_screen_sync_observer_finalize (GObject *object)
{
    WPEScreenSyncObserverPrivate *priv = WPE_SCREEN_SYNC_OBSERVER (object)->priv;
    g_hash_table_unref (priv->callbacks);
    g_mutex_clear (&priv->lock);
    G_OBJECT_CLASS (wpe_screen_sync_observer_parent_class)->finalize (object);
}

static void
wpe_screen_sync_observer_class_init (WPEScreenSyncObserverClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = wpe_screen_sync_observer_finalize;
    klass->sync = fake_sync;
}

static void
wpe_screen_sync_observer_init (WPEScreenSyncObserver *observer)
{
    observer->priv = wpe_screen_sync_observer_get_instance_private (observer);
    g_mutex_init (&observer->priv->lock);
    observer->priv->callbacks = g_hash_table_new_full (NULL, NULL, NULL, fake_callback_free);
}

guint
wpe_screen_sync_observer_add_callback (WPEScreenSyncObserver *observer, WPEScreenSyncObserverSyncFunc sync_func,
                                       gpointer user_data, GDestroyNotify destroy_notify)
{
    static guint last_id;
    WPEScreenSyncObserverPrivate *priv = observer->priv;
    FakeCallback *callback = g_new0 (FakeCallback, 1);
    gboolean should_start;
    guint id;

    callback->func = sync_func;
    callback->user_data = user_data;
    callback->destroy = destroy_notify;
    g_mutex_lock (&priv->lock);
    id = ++last_id;
    should_start = g_hash_table_size (priv->callbacks) == 0;
    g_hash_table_insert (priv->callbacks, GUINT_TO_POINTER (id), callback);
    g_mutex_unlock (&priv->lock);
    if (should_start)
        WPE_SCREEN_SYNC_OBSERVER_GET_CLASS (observer)->start (observer);
    return id;
}

void
wpe_screen_sync_observer_remove_callback (WPEScreenSyncObserver *observer, guint id)
{
    WPEScreenSyncObserverPrivate *priv = observer->priv;
    gboolean should_stop;

    g_mutex_lock (&priv->lock);
    g_hash_table_remove (priv->callbacks, GUINT_TO_POINTER (id));
    should_stop = g_hash_table_size (priv->callbacks) == 0;
    g_mutex_unlock (&priv->lock);
    if (should_stop)
        WPE_SCREEN_SYNC_OBSERVER_GET_CLASS (observer)->stop (observer);
}

/* ---- WPEScreen ------------------------------------------------------------------------------- */

struct _WPEScreenPrivate {
    guint32 id;
    int refresh_rate;
};

enum { PROP_0, PROP_ID };

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (WPEScreen, wpe_screen, G_TYPE_OBJECT)

static void
wpe_screen_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    if (prop_id == PROP_ID)
        WPE_SCREEN (object)->priv->id = g_value_get_uint (value);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
wpe_screen_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    if (prop_id == PROP_ID)
        g_value_set_uint (value, WPE_SCREEN (object)->priv->id);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
wpe_screen_class_init (WPEScreenClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->set_property = wpe_screen_set_property;
    object_class->get_property = wpe_screen_get_property;
    g_object_class_install_property (object_class, PROP_ID,
                                     g_param_spec_uint ("id", NULL, NULL, 0, G_MAXUINT, 0,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
wpe_screen_init (WPEScreen *screen)
{
    screen->priv = wpe_screen_get_instance_private (screen);
    screen->priv->refresh_rate = -1;
}

guint32
wpe_screen_get_id (WPEScreen *screen)
{
    return screen->priv->id;
}

int
wpe_screen_get_refresh_rate (WPEScreen *screen)
{
    return screen->priv->refresh_rate;
}

void
wpe_screen_set_refresh_rate (WPEScreen *screen, int refresh_rate)
{
    g_return_if_fail (refresh_rate == -1 || refresh_rate >= 0);
    if (refresh_rate == -1 || refresh_rate == screen->priv->refresh_rate)
        return;
    screen->priv->refresh_rate = refresh_rate;
}

WPEScreenSyncObserver *
wpe_screen_get_sync_observer (WPEScreen *screen)
{
    WPEScreenClass *klass = WPE_SCREEN_GET_CLASS (screen);
    return klass->get_sync_observer != NULL ? klass->get_sync_observer (screen) : NULL;
}
