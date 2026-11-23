/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 */
#include "uc_egl.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <string.h>

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377   /* EGL_EXT_device_drm_render_node */
#endif

#define UC_FALLBACK_RENDER_NODE  "/dev/dri/renderD128"
#define UC_FALLBACK_PRIMARY_NODE "/dev/dri/card0"

gboolean
uc_egl_extension_in_string (const char *extensions, const char *name)
{
    size_t len;
    const char *p;

    if (extensions == NULL || name == NULL || *name == '\0')
        return FALSE;
    len = strlen (name);
    for (p = extensions; (p = strstr (p, name)) != NULL; p += len) {
        gboolean starts = p == extensions || p[-1] == ' ';
        gboolean ends = p[len] == '\0' || p[len] == ' ';
        if (starts && ends)
            return TRUE;
    }
    return FALSE;
}

static gboolean
nonempty (const char *s)
{
    return s != NULL && *s != '\0';
}

gboolean
uc_egl_choose_drm_nodes (const char *egl_primary, const char *egl_render,
                         UcPathExists exists, gpointer user_data,
                         char **primary, char **render)
{
    *primary = NULL;
    *render = NULL;

    if (nonempty (egl_render) || nonempty (egl_primary)) {
        *render = nonempty (egl_render) ? g_strdup (egl_render) : NULL;
        *primary = nonempty (egl_primary) ? g_strdup (egl_primary) : NULL;
        return TRUE;
    }
    if (exists != NULL && exists (UC_FALLBACK_RENDER_NODE, user_data)) {
        *render = g_strdup (UC_FALLBACK_RENDER_NODE);
        if (exists (UC_FALLBACK_PRIMARY_NODE, user_data))
            *primary = g_strdup (UC_FALLBACK_PRIMARY_NODE);
        return TRUE;
    }
    return FALSE;
}

const char *
uc_egl_display_extensions (void *egl_display)
{
    const char *ext;

    if (egl_display == NULL)
        return "";
    ext = eglQueryString ((EGLDisplay) egl_display, EGL_EXTENSIONS);
    return ext != NULL ? ext : "";
}

gboolean
uc_egl_query_drm_nodes (void *egl_display, char **primary, char **render)
{
    const char *client_ext = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);
    PFNEGLQUERYDISPLAYATTRIBEXTPROC query_display_attrib;
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string;
    EGLAttrib device_attrib = 0;
    EGLDeviceEXT device;
    const char *device_ext;

    *primary = NULL;
    *render = NULL;
    if (egl_display == NULL)
        return FALSE;
    /* EGL_EXT_device_query is a client extension; its entry points come through eglGetProcAddress. */
    if (!uc_egl_extension_in_string (client_ext, "EGL_EXT_device_query"))
        return FALSE;
    query_display_attrib = (PFNEGLQUERYDISPLAYATTRIBEXTPROC) eglGetProcAddress ("eglQueryDisplayAttribEXT");
    query_device_string = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress ("eglQueryDeviceStringEXT");
    if (query_display_attrib == NULL || query_device_string == NULL)
        return FALSE;
    if (!query_display_attrib ((EGLDisplay) egl_display, EGL_DEVICE_EXT, &device_attrib))
        return FALSE;
    device = (EGLDeviceEXT) device_attrib;
    if (device == EGL_NO_DEVICE_EXT)
        return FALSE;

    device_ext = query_device_string (device, EGL_EXTENSIONS);
    if (uc_egl_extension_in_string (device_ext, "EGL_EXT_device_drm"))
        *primary = g_strdup (query_device_string (device, EGL_DRM_DEVICE_FILE_EXT));
    if (uc_egl_extension_in_string (device_ext, "EGL_EXT_device_drm_render_node"))
        *render = g_strdup (query_device_string (device, EGL_DRM_RENDER_NODE_FILE_EXT));
    if (*primary != NULL && **primary == '\0')
        g_clear_pointer (primary, g_free);
    if (*render != NULL && **render == '\0')
        g_clear_pointer (render, g_free);
    return TRUE;
}

UcFormatEntry *
uc_egl_query_dmabuf_formats (void *egl_display, guint *n_formats)
{
    const char *ext = uc_egl_display_extensions (egl_display);
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers = NULL;
    EGLint count = 0;
    EGLint *fourccs;
    UcFormatEntry *out;

    *n_formats = 0;
    if (egl_display == NULL || !uc_egl_extension_in_string (ext, "EGL_EXT_image_dma_buf_import"))
        return NULL;
    query_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC) eglGetProcAddress ("eglQueryDmaBufFormatsEXT");
    if (query_formats == NULL)
        return NULL;
    if (uc_egl_extension_in_string (ext, "EGL_EXT_image_dma_buf_import_modifiers"))
        query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC) eglGetProcAddress ("eglQueryDmaBufModifiersEXT");

    if (!query_formats ((EGLDisplay) egl_display, 0, NULL, &count) || count <= 0)
        return NULL;
    fourccs = g_new0 (EGLint, count);
    if (!query_formats ((EGLDisplay) egl_display, count, fourccs, &count) || count <= 0) {
        g_free (fourccs);
        return NULL;
    }

    out = g_new0 (UcFormatEntry, count);
    for (EGLint i = 0; i < count; i++) {
        EGLint n_mods = 0;
        out[i].fourcc = (guint32) fourccs[i];
        if (query_modifiers != NULL
            && query_modifiers ((EGLDisplay) egl_display, fourccs[i], 0, NULL, NULL, &n_mods) && n_mods > 0) {
            EGLuint64KHR *mods = g_new0 (EGLuint64KHR, n_mods);
            if (query_modifiers ((EGLDisplay) egl_display, fourccs[i], n_mods, mods, NULL, &n_mods) && n_mods > 0) {
                out[i].modifiers = g_new (guint64, n_mods);
                for (EGLint m = 0; m < n_mods; m++)
                    out[i].modifiers[m] = mods[m];
                out[i].n_modifiers = (guint) n_mods;
            }
            g_free (mods);
        }
    }
    g_free (fourccs);
    *n_formats = (guint) count;
    return out;
}

void
uc_egl_formats_free (UcFormatEntry *formats, guint n_formats)
{
    if (formats == NULL)
        return;
    for (guint i = 0; i < n_formats; i++)
        g_free (formats[i].modifiers);
    g_free (formats);
}
