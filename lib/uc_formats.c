/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 */
#include "uc_formats.h"

static void
entry_clear (gpointer data)
{
    UcFormatEntry *entry = data;
    g_free (entry->modifiers);
    entry->modifiers = NULL;
    entry->n_modifiers = 0;
}

static void
append_linear (GArray *out, guint32 fourcc)
{
    UcFormatEntry entry = { fourcc, g_new (guint64, 1), 1 };
    entry.modifiers[0] = UC_DRM_MOD_LINEAR;
    g_array_append_val (out, entry);
}

static gboolean
append_from_input (GArray *out, const UcFormatEntry *input, guint n_input, guint32 fourcc)
{
    for (guint i = 0; i < n_input; i++) {
        const UcFormatEntry *in = &input[i];
        UcFormatEntry entry = { fourcc, NULL, 0 };
        guint explicit_count = 0;

        if (in->fourcc != fourcc)
            continue;

        for (guint m = 0; m < in->n_modifiers; m++)
            if (in->modifiers[m] != UC_DRM_MOD_INVALID)
                explicit_count++;

        if (explicit_count > 0) {
            entry.modifiers = g_new (guint64, explicit_count);
            for (guint m = 0; m < in->n_modifiers; m++)
                if (in->modifiers[m] != UC_DRM_MOD_INVALID)
                    entry.modifiers[entry.n_modifiers++] = in->modifiers[m];
        } else if (in->n_modifiers > 0) {
            /* only DRM_FORMAT_MOD_INVALID reported: implicit modifiers */
            entry.modifiers = g_new (guint64, 1);
            entry.modifiers[0] = UC_DRM_MOD_INVALID;
            entry.n_modifiers = 1;
        } else {
            entry.modifiers = g_new (guint64, 1);
            entry.modifiers[0] = UC_DRM_MOD_LINEAR;
            entry.n_modifiers = 1;
        }
        g_array_append_val (out, entry);
        return TRUE;
    }
    return FALSE;
}

GArray *
uc_formats_select (const UcFormatEntry *input, guint n_input)
{
    static const guint32 wanted[] = { UC_FOURCC_ARGB8888, UC_FOURCC_XRGB8888 };
    GArray *out = g_array_new (FALSE, FALSE, sizeof (UcFormatEntry));

    g_array_set_clear_func (out, entry_clear);

    for (gsize i = 0; i < G_N_ELEMENTS (wanted); i++)
        append_from_input (out, input, input != NULL ? n_input : 0, wanted[i]);

    if (out->len == 0)
        for (gsize i = 0; i < G_N_ELEMENTS (wanted); i++)
            append_linear (out, wanted[i]);

    return out;
}

void
uc_formats_free (GArray *formats)
{
    if (formats != NULL)
        g_array_unref (formats);
}

const char *
uc_fourcc_to_string (guint32 fourcc, char buf[5])
{
    for (int i = 0; i < 4; i++) {
        unsigned char c = (fourcc >> (8 * i)) & 0xff;
        buf[i] = g_ascii_isprint (c) ? (char) c : '?';
    }
    buf[4] = '\0';
    return buf;
}

int
uc_formats_pixels_stride (gsize size, int width, int height)
{
    gsize row, stride;

    if (width <= 0 || height <= 0 || (gsize) width > G_MAXSIZE / 4)
        return -1;
    row = (gsize) width * 4;
    if (size % (gsize) height != 0)
        return -1;
    stride = size / (gsize) height;
    if (stride < row || stride > G_MAXINT)
        return -1;
    return (int) stride;
}
