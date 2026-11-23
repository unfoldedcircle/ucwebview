/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Forward compatibility of the structs in uc_webview.h (GLib only, host testable).
 *
 * The host and the library come from different releases of uc_webview.h: remote-ui custom UIs and the firmware
 * that ships libucwebview.so.1 update independently, both within UC_WEBVIEW_API_VERSION 1. A struct the host
 * allocates and passes in can therefore be shorter (older host) or longer (newer host) than the library's own
 * definition. The library never copies such a struct with its own sizeof: it reads at most the size the host's
 * struct has, and a field the host's struct lacks is NULL/0 (uc_webview.h, "Compatibility").
 *
 * The frozen copies below are the layouts of API version 1 as released in library 0.1.0 (unchanged up to 0.3.x).
 * uc_abi.c checks the public structs against them at compile time.
 */
#ifndef UC_ABI_H
#define UC_ABI_H

#include "uc_webview.h"

#include <glib.h>
#include <stddef.h>

G_BEGIN_DECLS

/* uc_webview_callbacks of API version 1 (libraries 0.1.0 to 0.3.x): what a host built against such a header
 * passes to uc_webview_new(). Never change it; new callbacks go to the end of uc_webview_callbacks only. */
typedef struct {
    void (*frame_ready)(uc_webview *, const uc_webview_frame *, void *user);
    void (*load_changed)(uc_webview *, int event, const char *uri, void *user);
    void (*title_changed)(uc_webview *, const char *title, void *user);
    void (*progress_changed)(uc_webview *, double progress, void *user);
    void (*web_process_terminated)(uc_webview *, int reason, void *user);
    void (*ime_focus)(uc_webview *, int focused, unsigned hints, unsigned purpose,
                      const char *surrounding, int cursor, void *user);
    void (*ime_cursor_area)(uc_webview *, int x, int y, int w, int h, void *user);
    void (*cursor_changed)(uc_webview *, const char *css_name, void *user);
    void (*ime_surrounding)(uc_webview *, const char *text, int cursor, int anchor, void *user);
} UcAbiCallbacksV1;

/* Bytes of the API version 1 callbacks (frame_ready .. ime_surrounding): what uc_webview_new() reads, and the
 * smallest cb_size uc_webview_new_sized() accepts. A field appended to uc_webview_callbacks does not change it. */
#define UC_ABI_CALLBACKS_V1_SIZE \
    (offsetof (UcAbiCallbacksV1, ime_surrounding) + sizeof (((UcAbiCallbacksV1 *) NULL)->ime_surrounding))

/* Bytes of uc_webview_callbacks as released in libraries 0.6.0 and 0.6.1 (the API version 1 callbacks, pinch and
 * magnification_changed): what remote-ui 0.83.0-20 passes to uc_webview_new_sized(). Its request callbacks (0.7.0)
 * stay NULL. uc_abi.c asserts that the fields after it are the 0.7.0 ones. */
#define UC_ABI_CALLBACKS_060_SIZE \
    (offsetof (uc_webview_callbacks, magnification_changed) + sizeof (((uc_webview_callbacks *) NULL)->magnification_changed))

/* Bytes of uc_webview_callbacks as released in libraries 0.7.0 to 0.9.x (up to response_refused): what a host of such
 * a header passes to uc_webview_new_sized(). Its load_failed_detail (0.10.0) stays NULL. */
#define UC_ABI_CALLBACKS_070_SIZE \
    (offsetof (uc_webview_callbacks, response_refused) + sizeof (((uc_webview_callbacks *) NULL)->response_refused))

/* uc_webview_frame of API version 1. The library allocates frames and the host copies them with the sizeof of its
 * own header, which the library cannot know, so the frame keeps this layout for all of API version 1 (a field
 * appended for a newer host would be read past the end of an older library's frame). */
typedef struct {
    int width, height;
    void *egl_image;
    uint32_t fourcc;
    uint64_t modifier;
    int n_planes;
    void *handle;
    const void *pixels;
    int stride;
} UcAbiFrameV1;

/* uc_webview_cookie as released in library 0.8.0. The host allocates arrays of it and the library steps through them
 * with its own sizeof, so it keeps this layout for all of API version 1 (an appended field would shift every element
 * after the first of an older host's array). */
typedef struct {
    const char *name, *value, *domain, *path;
    int secure, http_only;
    int64_t expires;
} UcAbiCookieV1;

typedef enum {
    UC_ABI_COPY_OK,         /* src_size bytes copied (all of dst, or a prefix with the rest zeroed); or src NULL */
    UC_ABI_COPY_LONGER,     /* the host's struct is longer (newer header): dst_size bytes copied, the rest unused */
    UC_ABI_COPY_TOO_SMALL,  /* src_size < min_size: refused, nothing read, dst all zero */
} UcAbiCopy;

/*
 * Copies a struct the host passed together with its size into the library's definition of it: dst is zeroed, then
 * min(src_size, dst_size) bytes of src are copied. No byte of src at or past src_size is read, and none past
 * dst_size. src NULL leaves dst zeroed (UC_ABI_COPY_OK). Precondition: dst_size >= min_size.
 */
UcAbiCopy uc_abi_copy_sized (void *dst, size_t dst_size, const void *src, size_t src_size, size_t min_size);

/*
 * The two ways of the host's callbacks into a view's table, uc_abi_copy_sized() with the library's
 * sizeof (uc_webview_callbacks) and UC_ABI_CALLBACKS_V1_SIZE as the minimum:
 *  - uc_webview_new(): exactly the API version 1 callbacks (UC_ABI_CALLBACKS_V1_SIZE bytes), whatever header the
 *    host was built against; later fields of dst are NULL;
 *  - uc_webview_new_sized(): src_size bytes, the host's sizeof (uc_webview_callbacks); UC_ABI_COPY_TOO_SMALL below
 *    UC_ABI_CALLBACKS_V1_SIZE, UC_ABI_COPY_LONGER for a host newer than the library.
 */
UcAbiCopy uc_abi_callbacks_copy_v1 (uc_webview_callbacks *dst, const uc_webview_callbacks *src);
UcAbiCopy uc_abi_callbacks_copy    (uc_webview_callbacks *dst, const uc_webview_callbacks *src, size_t src_size);

G_END_DECLS

#endif /* UC_ABI_H */
