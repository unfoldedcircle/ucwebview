/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Device state for web pages (library 0.9.0), the UI process side, see uc_device_wk.h.
 */
#include "uc_device_wk.h"
#include "uc_ext_protocol.h"

struct _UcDeviceWk {
    WebKitWebView *web_view;                /* borrowed */
    UcPageWk *page;                         /* borrowed */
    WebKitUserContentManager *manager;      /* borrowed from web_view (one per view, WebKitWebView.cpp:990-991) */
    WebKitUserScript *script;               /* in manager while enabled */
    gboolean enabled;
    UcDeviceState state;
};

UcDeviceWk *
uc_device_wk_new (WebKitWebView *web_view, UcPageWk *page)
{
    UcDeviceWk *device = g_new0 (UcDeviceWk, 1);

    device->web_view = web_view;
    device->page = page;
    device->manager = webkit_web_view_get_user_content_manager (web_view);
    uc_device_state_init (&device->state);
    return device;
}

static void
remove_script (UcDeviceWk *device)
{
    if (device->script == NULL)
        return;
    webkit_user_content_manager_remove_script (device->manager, device->script);
    webkit_user_script_unref (device->script);
    device->script = NULL;
}

/* the script with the current state; the new one goes in before the old one goes out */
static void
install_script (UcDeviceWk *device)
{
    char *source = uc_device_start_script (&device->state);
    WebKitUserScript *script = webkit_user_script_new_for_world (source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                                 WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                                 UC_EXT_HOST_WORLD, NULL, NULL);

    g_free (source);
    webkit_user_content_manager_add_script (device->manager, script);
    remove_script (device);
    device->script = script;
}

void
uc_device_wk_free (UcDeviceWk *device)
{
    if (device == NULL)
        return;
    remove_script (device);
    g_free (device);
}

/* change: a floating a{sv} (uc_device_state_change(), uc_device_event_change()); to the shown page while the API is
 * on and the page is reachable. A document that started while the API was off has no shim and ignores it. */
static void
send_change (UcDeviceWk *device, GVariant *change)
{
    g_variant_ref_sink (change);
    if (device->enabled)
        uc_page_wk_send (device->page, UC_EXT_DEVICE_MESSAGE, change);
    g_variant_unref (change);
}

gboolean
uc_device_wk_enabled (UcDeviceWk *device)
{
    return device->enabled;
}

void
uc_device_wk_set_enabled (UcDeviceWk *device, gboolean enabled)
{
    enabled = !!enabled;
    if (enabled == device->enabled)
        return;
    device->enabled = enabled;
    if (enabled) {
        install_script (device);
        send_change (device, uc_device_state_change (&device->state));
    } else {
        remove_script (device);
    }
}

gboolean
uc_device_wk_set_state (UcDeviceWk *device, const char *json, GString *ignored, const char **error)
{
    UcDeviceState next = device->state;

    if (!uc_device_state_merge_json (&next, json, ignored, error))
        return FALSE;
    if (uc_device_state_equal (&next, &device->state))
        return TRUE;
    device->state = next;
    if (device->enabled) {
        install_script (device);
        send_change (device, uc_device_state_change (&device->state));
    }
    return TRUE;
}

const UcDeviceState *
uc_device_wk_state (UcDeviceWk *device)
{
    return &device->state;
}

void
uc_device_wk_event (UcDeviceWk *device, UcDeviceEvent event, gint64 slept_ms)
{
    send_change (device, uc_device_event_change (&device->state, event, slept_ms));
}

void
uc_device_wk_load_finished (UcDeviceWk *device)
{
    if (device->enabled)
        send_change (device, uc_device_state_change (&device->state));
}
