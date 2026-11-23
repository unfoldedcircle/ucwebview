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

/*
 * The row stride of the pixels wpe_buffer_import_to_pixels() returned for a DMA-BUF buffer of width x height (4 bytes
 * per pixel, the only formats it maps are ARGB8888 and XRGB8888), from the size of the GBytes (library 0.10.3). The
 * GBytes is height whole rows in every release, but the rows changed:
 *  - WPE WebKit 2.54.2 and older: the gbm_bo_map() mapping itself, height rows of the map's stride
 *    (WPEBufferDMABuf.cpp:264-283 in 2.54.2). That stride is the mapping's, which need not be the plane's
 *    (wpe_buffer_dma_buf_get_stride()): a driver may map a tiled or compressed buffer through a linear copy.
 *  - 2.54.3: rows without padding, width * 4 bytes; a mapping with another stride is copied row by row
 *    (WPEBufferDMABuf.cpp:265-296, wpeBufferCopyPixelsWithoutRowPadding() in WPEBuffer.cpp:285-296, and the
 *    function's documentation, WPEBuffer.cpp:298-309, "The rows of the returned data are not padded").
 * The size tells both apart without the WebKit version. -1 when it is not height rows of at least width * 4 bytes
 * (or the stride does not fit an int): the frame cannot be described.
 */
int uc_formats_pixels_stride (gsize size, int width, int height);

/* "AR24" style string for logging; buf must hold 5 bytes. Non-printable bytes become '?'. */
const char *uc_fourcc_to_string (guint32 fourcc, char buf[5]);

G_END_DECLS

#endif /* UC_FORMATS_H */
