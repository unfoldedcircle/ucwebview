/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * EGL helpers for libucwebview: extension string parsing, the DRM nodes of the
 * host EGLDisplay (EGL_EXT_device_query + EGL_EXT_device_drm(_render_node), as
 * the upstream Qt 6 binding does in WPEDisplayQtQuick.cpp:67-95, with a fixed
 * path fallback), and the DMA-BUF formats the display can import
 * (EGL_EXT_image_dma_buf_import_modifiers). Function pointers come from
 * eglGetProcAddress; libEGL is the only link dependency.
 */
#ifndef UC_EGL_H
#define UC_EGL_H

#include "uc_formats.h"

#include <glib.h>

G_BEGIN_DECLS

/* Pure: whole-word search in a space separated EGL extension string (NULL safe). */
gboolean uc_egl_extension_in_string (const char *extensions, const char *name);

typedef gboolean (*UcPathExists) (const char *path, gpointer user_data);

/*
 * Pure: picks the DRM nodes to hand to the web process. egl_primary/egl_render
 * are what EGL reported (NULL/"" when unknown). Preference: EGL's render node
 * (primary may stay NULL), then the fixed /dev/dri/renderD128 + /dev/dri/card0
 * if exists() says so. Returns FALSE with both outputs NULL when no node is
 * known (software rendering: the web process falls back to shared memory).
 * Outputs are g_malloc'd.
 */
gboolean uc_egl_choose_drm_nodes (const char *egl_primary, const char *egl_render,
                                  UcPathExists exists, gpointer user_data,
                                  char **primary, char **render);

/* Display level extension string of egl_display ("" if unavailable). */
const char *uc_egl_display_extensions (void *egl_display);

/* EGL_DRM_DEVICE_FILE_EXT / EGL_DRM_RENDER_NODE_FILE_EXT of the display's EGLDeviceEXT, g_malloc'd or
 * NULL each; returns FALSE when EGL_EXT_device_query is missing or the display has no device. */
gboolean uc_egl_query_drm_nodes (void *egl_display, char **primary, char **render);

/* Formats and modifiers eglQueryDmaBufFormatsEXT/eglQueryDmaBufModifiersEXT report for egl_display;
 * NULL (and *n_formats = 0) when EGL_EXT_image_dma_buf_import is missing. Free with uc_egl_formats_free(). */
UcFormatEntry *uc_egl_query_dmabuf_formats (void *egl_display, guint *n_formats);
void           uc_egl_formats_free         (UcFormatEntry *formats, guint n_formats);

G_END_DECLS

#endif /* UC_EGL_H */
