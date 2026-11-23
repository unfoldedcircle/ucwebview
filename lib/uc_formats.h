/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * DMA-BUF format negotiation for libucwebview: which (fourcc, modifier) pairs the
 * host EGLDisplay can import are advertised to the web process as preferred
 * buffer formats (WPEDisplayClass::get_preferred_buffer_formats). GLib only.
 */
#ifndef UC_FORMATS_H
#define UC_FORMATS_H

#include <glib.h>

G_BEGIN_DECLS

/* drm_fourcc.h values (no libdrm dependency): fourcc_code('A','R','2','4') etc. */
#define UC_FOURCC_ARGB8888   0x34325241u /* 'AR24': [31:0] A:R:G:B 8:8:8:8 little endian */
#define UC_FOURCC_XRGB8888   0x34325258u /* 'XR24': [31:0] x:R:G:B 8:8:8:8 little endian */
#define UC_DRM_MOD_LINEAR    0x0ull
#define UC_DRM_MOD_INVALID   0x00ffffffffffffffull /* DRM_FORMAT_MOD_INVALID: no explicit modifier */

typedef struct {
    guint32  fourcc;
    guint64 *modifiers;   /* g_malloc'd, may be NULL when n_modifiers == 0 */
    guint    n_modifiers;
} UcFormatEntry;

/*
 * Selects what to advertise from the formats the EGL display reports
 * (eglQueryDmaBufFormatsEXT/eglQueryDmaBufModifiersEXT, in any order):
 *  - only ARGB8888 and XRGB8888 (the formats WebKit renders into and the host
 *    scene graph samples as RGBA), ARGB first;
 *  - the modifiers EGL reported for them; DRM_FORMAT_MOD_INVALID is dropped
 *    when explicit modifiers exist and kept alone otherwise (the web process
 *    then allocates without modifiers, AcceleratedSurface.cpp:289-290);
 *  - a format without any modifier gets LINEAR;
 *  - when EGL reports neither format (or nothing), both are advertised with
 *    LINEAR only, which every importer accepts.
 * Returns a GArray of UcFormatEntry; free with uc_formats_free().
 */
GArray *uc_formats_select (const UcFormatEntry *input, guint n_input);
void    uc_formats_free   (GArray *formats);

/* "AR24" style string for logging; buf must hold 5 bytes. Non-printable bytes become '?'. */
const char *uc_fourcc_to_string (guint32 fourcc, char buf[5]);

G_END_DECLS

#endif /* UC_FORMATS_H */
