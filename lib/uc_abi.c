/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 */
#include "uc_abi.h"

#include <string.h>

/* A field of the public struct must keep the offset and type it had in API version 1; otherwise a host built
 * against an older header passes something else there, which needs UC_WEBVIEW_API_VERSION 2. */
#define UC_ABI_SAME_FIELD(public_type, frozen_type, field) \
    _Static_assert (offsetof (public_type, field) == offsetof (frozen_type, field) \
                    && __builtin_types_compatible_p (__typeof__ (((public_type *) NULL)->field), \
                                                     __typeof__ (((frozen_type *) NULL)->field)), \
                    #public_type "." #field " moved or changed type: an incompatible change of API version 1")

UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, frame_ready);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, load_changed);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, title_changed);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, progress_changed);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, web_process_terminated);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, ime_focus);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, ime_cursor_area);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, cursor_changed);
UC_ABI_SAME_FIELD (uc_webview_callbacks, UcAbiCallbacksV1, ime_surrounding);
_Static_assert (UC_ABI_CALLBACKS_V1_SIZE == sizeof (UcAbiCallbacksV1),
                "API version 1 callbacks: nine fields, no trailing padding");
_Static_assert (sizeof (uc_webview_callbacks) >= UC_ABI_CALLBACKS_V1_SIZE,
                "uc_webview_callbacks lost API version 1 fields");
/* Library 0.6.0 appended pinch and magnification_changed right after them; a host of a 0.6 header passes them there
 * from now on, so they keep these places like the API version 1 fields keep theirs. */
_Static_assert (offsetof (uc_webview_callbacks, pinch) == UC_ABI_CALLBACKS_V1_SIZE,
                "uc_webview_callbacks.pinch moved: an incompatible change of API version 1");
_Static_assert (offsetof (uc_webview_callbacks, magnification_changed)
                == UC_ABI_CALLBACKS_V1_SIZE + sizeof (void (*) (void)),
                "uc_webview_callbacks.magnification_changed moved: an incompatible change of API version 1");
_Static_assert (UC_ABI_CALLBACKS_060_SIZE == UC_ABI_CALLBACKS_V1_SIZE + 2 * sizeof (void (*) (void)),
                "the struct of libraries 0.6.x: API version 1 + pinch + magnification_changed");
/* Library 0.7.0 appended the six request callbacks after them, in this order. */
#define UC_ABI_APPENDED(field, slot) \
    _Static_assert (offsetof (uc_webview_callbacks, field) == UC_ABI_CALLBACKS_V1_SIZE + (slot) * sizeof (void (*) (void)), \
                    "uc_webview_callbacks." #field " moved: an incompatible change of API version 1")
UC_ABI_APPENDED (script_dialog, 2);
UC_ABI_APPENDED (authenticate, 3);
UC_ABI_APPENDED (request_cancelled, 4);
UC_ABI_APPENDED (close_requested, 5);
UC_ABI_APPENDED (new_window, 6);
UC_ABI_APPENDED (response_refused, 7);
_Static_assert (UC_ABI_CALLBACKS_070_SIZE == UC_ABI_CALLBACKS_V1_SIZE + 8 * sizeof (void (*) (void)),
                "the struct of libraries 0.7.0 to 0.9.x: API version 1 + 2 (0.6.0) + 6 (0.7.0)");
/* Library 0.10.0 appended load_failed_detail after them. */
UC_ABI_APPENDED (load_failed_detail, 8);
_Static_assert (sizeof (uc_webview_callbacks) == UC_ABI_CALLBACKS_V1_SIZE + 9 * sizeof (void (*) (void)),
                "uc_webview_callbacks of library 0.10.0: the API version 1 callbacks + 2 (0.6.0) + 6 (0.7.0) + 1 (0.10.0), "
                "no padding; a new callback goes to the end and updates this check, the tests and the README table");

UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, width);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, height);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, egl_image);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, fourcc);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, modifier);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, n_planes);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, handle);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, pixels);
UC_ABI_SAME_FIELD (uc_webview_frame, UcAbiFrameV1, stride);
_Static_assert (sizeof (uc_webview_frame) == sizeof (UcAbiFrameV1),
                "uc_webview_frame does not grow within API version 1: hosts copy it with the sizeof of their own "
                "header (uc_webview.h, Compatibility); add a function that takes the frame handle instead");
/* The sizeof check misses a small field in the frame's tail padding (an int after stride on LP64). A positional
 * initializer of the nine API version 1 fields catches every added field: "missing initializer for field ...". */
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wmissing-field-initializers"
static const uc_webview_frame uc_abi_frame_v1_fields G_GNUC_UNUSED = { 0, 0, NULL, 0, 0, 0, NULL, NULL, 0 };
#pragma GCC diagnostic pop

UC_ABI_SAME_FIELD (uc_webview_cookie, UcAbiCookieV1, name);
UC_ABI_SAME_FIELD (uc_webview_cookie, UcAbiCookieV1, value);
UC_ABI_SAME_FIELD (uc_webview_cookie, UcAbiCookieV1, domain);
UC_ABI_SAME_FIELD (uc_webview_cookie, UcAbiCookieV1, path);
UC_ABI_SAME_FIELD (uc_webview_cookie, UcAbiCookieV1, secure);
UC_ABI_SAME_FIELD (uc_webview_cookie, UcAbiCookieV1, http_only);
UC_ABI_SAME_FIELD (uc_webview_cookie, UcAbiCookieV1, expires);
_Static_assert (sizeof (uc_webview_cookie) == sizeof (UcAbiCookieV1),
                "uc_webview_cookie does not grow within API version 1: hosts pass arrays of it (uc_webview.h, "
                "Compatibility); a new attribute needs a new function");
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wmissing-field-initializers"
static const uc_webview_cookie uc_abi_cookie_v1_fields G_GNUC_UNUSED = { NULL, NULL, NULL, NULL, 0, 0, 0 };
#pragma GCC diagnostic pop

UcAbiCopy
uc_abi_copy_sized (void *dst, size_t dst_size, const void *src, size_t src_size, size_t min_size)
{
    memset (dst, 0, dst_size);
    if (src == NULL)
        return UC_ABI_COPY_OK;
    if (src_size < min_size)
        return UC_ABI_COPY_TOO_SMALL;
    memcpy (dst, src, MIN (src_size, dst_size));
    return src_size > dst_size ? UC_ABI_COPY_LONGER : UC_ABI_COPY_OK;
}

UcAbiCopy
uc_abi_callbacks_copy_v1 (uc_webview_callbacks *dst, const uc_webview_callbacks *src)
{
    return uc_abi_copy_sized (dst, sizeof *dst, src, UC_ABI_CALLBACKS_V1_SIZE, UC_ABI_CALLBACKS_V1_SIZE);
}

UcAbiCopy
uc_abi_callbacks_copy (uc_webview_callbacks *dst, const uc_webview_callbacks *src, size_t src_size)
{
    return uc_abi_copy_sized (dst, sizeof *dst, src, src_size, UC_ABI_CALLBACKS_V1_SIZE);
}
