/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_abi.c (GLib only): run with `make check`; `make check-asan` runs it again built with
 * AddressSanitizer and UBSan.
 *
 * A host built against the header of libraries 0.1.0 to 0.3.x passes a callback struct of UC_ABI_CALLBACKS_V1_SIZE
 * bytes. The tests put such structs at the end of a page followed by a PROT_NONE page, so that reading a single
 * byte past them kills the process with SIGSEGV, and in heap blocks of exactly their size, which AddressSanitizer
 * checks (it does not watch mmap()ed memory). The guard-bites tests are the negative controls: one byte past the
 * end, and the whole-struct copy of library 0.3.0 (wv->cb = *cb) once the library's struct has grown, both crash.
 */
#include "uc_abi.h"

#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

/* The library's uc_webview_callbacks of a later release: two callbacks appended to today's struct. */
typedef struct {
    uc_webview_callbacks current;
    void (*added_a)(uc_webview *, void *user);
    void (*added_b)(uc_webview *, int value, void *user);
} FutureLibraryCallbacks;

/* A host built against a header newer than the library: one callback the library does not know. */
typedef struct {
    uc_webview_callbacks current;
    void (*newer)(uc_webview *, void *user);
} NewerHostCallbacks;

typedef struct {
    guint8 *base;
    size_t len;
} Guard;

/* size bytes that end where a PROT_NONE page starts */
static void *
guard_alloc (Guard *guard, size_t size)
{
    size_t page = (size_t) sysconf (_SC_PAGESIZE);

    g_assert_cmpuint (size, <=, page);
    g_assert_cmpuint (size % sizeof (void *), ==, 0);   /* the struct stays pointer aligned */
    guard->len = 2 * page;
    guard->base = mmap (NULL, guard->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_assert_true (guard->base != MAP_FAILED);
    g_assert_cmpint (mprotect (guard->base + page, page, PROT_NONE), ==, 0);
    return guard->base + page - size;
}

static void
guard_free (Guard *guard)
{
    munmap (guard->base, guard->len);
}

/* non-zero bytes, different per seed: stand-ins for function pointers that are compared, never called */
static void
fill_pattern (void *p, size_t n, guint8 seed)
{
    guint8 *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (guint8) (1 + (seed + i) % 255);
}

static gboolean
all_zero (const void *p, size_t n)
{
    const guint8 *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0)
            return FALSE;
    return TRUE;
}

/* A host struct of exactly the API version 1 size (the 0.3.0 header): at a page end, or on the heap. */
static const uc_webview_callbacks *
v1_host_struct (Guard *guard)
{
    void *src = guard != NULL ? guard_alloc (guard, UC_ABI_CALLBACKS_V1_SIZE) : g_malloc (UC_ABI_CALLBACKS_V1_SIZE);
    fill_pattern (src, UC_ABI_CALLBACKS_V1_SIZE, 7);
    return src;
}

static void
v1_host_struct_free (Guard *guard, const uc_webview_callbacks *src)
{
    if (guard != NULL)
        guard_free (guard);
    else
        g_free ((gpointer) src);
}

/* the struct of library 0.7.0: the six request callbacks right after those of 0.6.0, in the order of the header */
static void
test_layout_070 (void)
{
    const size_t fp = sizeof (void (*) (void));

    g_assert_cmpuint (UC_ABI_CALLBACKS_060_SIZE, ==, 11 * fp);
    g_assert_cmpuint (offsetof (uc_webview_callbacks, script_dialog), ==, 11 * fp);
    g_assert_cmpuint (offsetof (uc_webview_callbacks, authenticate), ==, 12 * fp);
    g_assert_cmpuint (offsetof (uc_webview_callbacks, request_cancelled), ==, 13 * fp);
    g_assert_cmpuint (offsetof (uc_webview_callbacks, close_requested), ==, 14 * fp);
    g_assert_cmpuint (offsetof (uc_webview_callbacks, new_window), ==, 15 * fp);
    g_assert_cmpuint (offsetof (uc_webview_callbacks, response_refused), ==, 16 * fp);
    g_assert_cmpuint (UC_ABI_CALLBACKS_070_SIZE, ==, 17 * fp);
    g_test_message ("callbacks: 0.6.x %zu bytes, 0.7.0 to 0.9.x %zu bytes", (size_t) UC_ABI_CALLBACKS_060_SIZE,
                    (size_t) UC_ABI_CALLBACKS_070_SIZE);
}

/* the struct of library 0.10.0: load_failed_detail right after the request callbacks of 0.7.0 */
static void
test_layout_0100 (void)
{
    const size_t fp = sizeof (void (*) (void));

    g_assert_cmpuint (offsetof (uc_webview_callbacks, load_failed_detail), ==, 17 * fp);
    g_assert_cmpuint (sizeof (uc_webview_callbacks), ==, 18 * fp);
    g_test_message ("callbacks: 0.10.0 %zu bytes", sizeof (uc_webview_callbacks));
}

static void
test_v1_layout (void)
{
    /* nine function pointers, frame_ready .. ime_surrounding, as released in 0.1.0 to 0.3.x */
    g_assert_cmpuint (UC_ABI_CALLBACKS_V1_SIZE, ==, 9 * sizeof (void (*) (void)));
    g_assert_cmpuint (offsetof (uc_webview_callbacks, ime_surrounding), ==, 8 * sizeof (void (*) (void)));
    g_assert_cmpuint (sizeof (uc_webview_callbacks), >=, UC_ABI_CALLBACKS_V1_SIZE);
    g_assert_cmpuint (sizeof (uc_webview_frame), ==, sizeof (UcAbiFrameV1));
    g_test_message ("callbacks: API version 1 %zu bytes, this header %zu bytes; frame %zu bytes",
                    (size_t) UC_ABI_CALLBACKS_V1_SIZE, sizeof (uc_webview_callbacks), sizeof (uc_webview_frame));
}

static void
assert_no_request_callbacks (const uc_webview_callbacks *cb)
{
    g_assert_null (cb->script_dialog);
    g_assert_null (cb->authenticate);
    g_assert_null (cb->request_cancelled);
    g_assert_null (cb->close_requested);
    g_assert_null (cb->new_window);
    g_assert_null (cb->response_refused);
    /* and the one of 0.10.0: FAILED without a reason, as before */
    g_assert_null (cb->load_failed_detail);
}

/* uc_webview_new(): reads the API version 1 bytes and nothing after them; later fields of the table are NULL */
static void
check_plain_new (Guard *guard)
{
    const uc_webview_callbacks *src = v1_host_struct (guard);
    uc_webview_callbacks dst;

    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy_v1 (&dst, src), ==, UC_ABI_COPY_OK);
    g_assert_cmpmem (&dst, UC_ABI_CALLBACKS_V1_SIZE, src, UC_ABI_CALLBACKS_V1_SIZE);
    g_assert_true (all_zero ((const guint8 *) &dst + UC_ABI_CALLBACKS_V1_SIZE,
                             sizeof dst - UC_ABI_CALLBACKS_V1_SIZE));
    g_assert_true (dst.frame_ready != NULL && dst.ime_surrounding != NULL);
    /* the callbacks of library 0.6.0 stay off: no pinch recognition for such a host */
    g_assert_null (dst.pinch);
    g_assert_null (dst.magnification_changed);
    /* and those of 0.7.0: the library's request defaults, no request reaches the host */
    assert_no_request_callbacks (&dst);
    v1_host_struct_free (guard, src);
}

/* uc_webview_new_sized() with cb_size = the 0.3.0 sizeof, and with this header's sizeof */
static void
check_sized (Guard *guard)
{
    const uc_webview_callbacks *src = v1_host_struct (guard);
    uc_webview_callbacks dst;
    Guard guard2;
    uc_webview_callbacks *full;

    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy (&dst, src, UC_ABI_CALLBACKS_V1_SIZE), ==, UC_ABI_COPY_OK);
    g_assert_cmpmem (&dst, UC_ABI_CALLBACKS_V1_SIZE, src, UC_ABI_CALLBACKS_V1_SIZE);
    g_assert_true (all_zero ((const guint8 *) &dst + UC_ABI_CALLBACKS_V1_SIZE,
                             sizeof dst - UC_ABI_CALLBACKS_V1_SIZE));
    g_assert_null (dst.pinch);
    g_assert_null (dst.magnification_changed);
    assert_no_request_callbacks (&dst);
    v1_host_struct_free (guard, src);

    full = guard != NULL ? guard_alloc (&guard2, sizeof *full) : g_malloc (sizeof *full);
    fill_pattern (full, sizeof *full, 11);
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy (&dst, full, sizeof *full), ==, UC_ABI_COPY_OK);
    g_assert_cmpmem (&dst, sizeof dst, full, sizeof *full);
    if (guard != NULL)
        guard_free (&guard2);
    else
        g_free (full);
}

/* uc_webview_new_sized() from a host of a 0.6 header (remote-ui 0.83.0-20): pinch and magnification_changed are its,
 * the request callbacks of 0.7.0 stay NULL, and nothing after its 88 bytes (aarch64) is read */
static void
check_sized_060 (Guard *guard)
{
    uc_webview_callbacks *src = guard != NULL ? guard_alloc (guard, UC_ABI_CALLBACKS_060_SIZE)
                                              : g_malloc (UC_ABI_CALLBACKS_060_SIZE);
    uc_webview_callbacks dst;

    fill_pattern (src, UC_ABI_CALLBACKS_060_SIZE, 13);
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy (&dst, src, UC_ABI_CALLBACKS_060_SIZE), ==, UC_ABI_COPY_OK);
    g_assert_cmpmem (&dst, UC_ABI_CALLBACKS_060_SIZE, src, UC_ABI_CALLBACKS_060_SIZE);
    g_assert_nonnull (dst.pinch);
    g_assert_nonnull (dst.magnification_changed);
    assert_no_request_callbacks (&dst);
    g_assert_true (all_zero ((const guint8 *) &dst + UC_ABI_CALLBACKS_060_SIZE, sizeof dst - UC_ABI_CALLBACKS_060_SIZE));
    if (guard != NULL)
        guard_free (guard);
    else
        g_free (src);
}

/* uc_webview_new_sized() from a host of a 0.7.0 to 0.9.x header (remote-ui with the request API): its callbacks up to
 * response_refused, load_failed_detail (0.10.0) stays NULL, and nothing after its 136 bytes (aarch64) is read */
static void
check_sized_070 (Guard *guard)
{
    uc_webview_callbacks *src = guard != NULL ? guard_alloc (guard, UC_ABI_CALLBACKS_070_SIZE)
                                              : g_malloc (UC_ABI_CALLBACKS_070_SIZE);
    uc_webview_callbacks dst;

    fill_pattern (src, UC_ABI_CALLBACKS_070_SIZE, 17);
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy (&dst, src, UC_ABI_CALLBACKS_070_SIZE), ==, UC_ABI_COPY_OK);
    g_assert_cmpmem (&dst, UC_ABI_CALLBACKS_070_SIZE, src, UC_ABI_CALLBACKS_070_SIZE);
    g_assert_nonnull (dst.script_dialog);
    g_assert_nonnull (dst.response_refused);
    g_assert_null (dst.load_failed_detail);
    g_assert_true (all_zero ((const guint8 *) &dst + UC_ABI_CALLBACKS_070_SIZE, sizeof dst - UC_ABI_CALLBACKS_070_SIZE));
    if (guard != NULL)
        guard_free (guard);
    else
        g_free (src);
}

/*
 * A later library whose uc_webview_callbacks has grown (FutureLibraryCallbacks) gets the 0.3.0 host's struct:
 * both paths copy the API version 1 bytes and leave the appended callbacks NULL. uc_abi_callbacks_copy_v1() and
 * uc_abi_callbacks_copy() are uc_abi_copy_sized() with sizeof of the library's struct as dst_size, which is what
 * this calls with the grown struct.
 */
static void
check_future_library (Guard *guard)
{
    const uc_webview_callbacks *src = v1_host_struct (guard);
    FutureLibraryCallbacks dst;

    /* uc_webview_new() */
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_copy_sized (&dst, sizeof dst, src, UC_ABI_CALLBACKS_V1_SIZE, UC_ABI_CALLBACKS_V1_SIZE),
                     ==, UC_ABI_COPY_OK);
    g_assert_cmpmem (&dst, UC_ABI_CALLBACKS_V1_SIZE, src, UC_ABI_CALLBACKS_V1_SIZE);
    g_assert_true (all_zero ((const guint8 *) &dst + UC_ABI_CALLBACKS_V1_SIZE, sizeof dst - UC_ABI_CALLBACKS_V1_SIZE));
    g_assert_null (dst.added_a);
    g_assert_null (dst.added_b);

    /* uc_webview_new_sized() from a host built against an older header than the library's */
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_copy_sized (&dst, sizeof dst, src, UC_ABI_CALLBACKS_V1_SIZE, UC_ABI_CALLBACKS_V1_SIZE),
                     ==, UC_ABI_COPY_OK);
    g_assert_null (dst.added_a);
    g_assert_null (dst.added_b);
    g_assert_true (dst.current.ime_surrounding != NULL);
    v1_host_struct_free (guard, src);
}

/* uc_webview_new_sized() from a host newer than the library: the library's part is used, the rest is not read */
static void
check_newer_host (Guard *guard)
{
    NewerHostCallbacks *src = guard != NULL ? guard_alloc (guard, sizeof *src) : g_malloc (sizeof *src);
    uc_webview_callbacks dst;

    fill_pattern (src, sizeof *src, 23);
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy (&dst, &src->current, sizeof *src), ==, UC_ABI_COPY_LONGER);
    g_assert_cmpmem (&dst, sizeof dst, &src->current, sizeof src->current);
    if (guard != NULL)
        guard_free (guard);
    else
        g_free (src);
}

static void
test_plain_new_guard (void)       { Guard g; check_plain_new (&g); }
static void
test_plain_new_heap (void)        { check_plain_new (NULL); }
static void
test_sized_guard (void)           { Guard g; check_sized (&g); }
static void
test_sized_heap (void)            { check_sized (NULL); }
static void
test_sized_060_guard (void)       { Guard g; check_sized_060 (&g); }
static void
test_sized_060_heap (void)        { check_sized_060 (NULL); }
static void
test_sized_070_guard (void)       { Guard g; check_sized_070 (&g); }
static void
test_sized_070_heap (void)        { check_sized_070 (NULL); }
static void
test_future_library_guard (void)  { Guard g; check_future_library (&g); }
static void
test_future_library_heap (void)   { check_future_library (NULL); }
static void
test_newer_host_guard (void)      { Guard g; check_newer_host (&g); }
static void
test_newer_host_heap (void)       { check_newer_host (NULL); }

/* below API version 1: refused without reading the host's memory, the table stays empty */
static void
test_too_small (void)
{
    const size_t sizes[] = { 0, sizeof (void *), UC_ABI_CALLBACKS_V1_SIZE - sizeof (void *) };

    for (size_t i = 0; i < G_N_ELEMENTS (sizes); i++) {
        Guard guard;
        /* sizes[i] bytes before the guard page (for 0 the pointer is the guard page itself) */
        const uc_webview_callbacks *src = guard_alloc (&guard, sizes[i]);
        uc_webview_callbacks dst;

        if (sizes[i] > 0)
            fill_pattern ((void *) src, sizes[i], 3);
        memset (&dst, 0xa5, sizeof dst);
        g_assert_cmpint (uc_abi_callbacks_copy (&dst, src, sizes[i]), ==, UC_ABI_COPY_TOO_SMALL);
        g_assert_true (all_zero (&dst, sizeof dst));
        guard_free (&guard);
    }
}

/* cb NULL: no callbacks, whatever the size */
static void
test_null (void)
{
    uc_webview_callbacks dst;

    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy_v1 (&dst, NULL), ==, UC_ABI_COPY_OK);
    g_assert_true (all_zero (&dst, sizeof dst));
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy (&dst, NULL, 0), ==, UC_ABI_COPY_OK);
    g_assert_true (all_zero (&dst, sizeof dst));
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_callbacks_copy (&dst, NULL, sizeof dst), ==, UC_ABI_COPY_OK);
    g_assert_true (all_zero (&dst, sizeof dst));
}

/* a newer host on a newer library: the generic copy between two sizes above API version 1 */
static void
test_between_releases (void)
{
    Guard guard;
    /* host: today's struct + newer (one appended); library: today's struct + added_a + added_b (two appended) */
    NewerHostCallbacks *src = guard_alloc (&guard, sizeof *src);
    FutureLibraryCallbacks dst;

    fill_pattern (src, sizeof *src, 31);
    memset (&dst, 0xa5, sizeof dst);
    g_assert_cmpint (uc_abi_copy_sized (&dst, sizeof dst, src, sizeof *src, UC_ABI_CALLBACKS_V1_SIZE), ==, UC_ABI_COPY_OK);
    g_assert_cmpmem (&dst, sizeof *src, src, sizeof *src);
    g_assert_null (dst.added_b);
    guard_free (&guard);
}

/* ---- negative controls: the guard page catches an over-read ---------------------------------------------- */

static FutureLibraryCallbacks sink;

static void
no_core_dump (void)
{
    prctl (PR_SET_DUMPABLE, 0, 0, 0, 0);   /* the expected SIGSEGV leaves no core file behind */
}

static void
test_guard_bites_one_byte (void)
{
    if (g_test_subprocess ()) {
        Guard guard;
        const volatile guint8 *src = (const void *) v1_host_struct (&guard);
        no_core_dump ();
        g_test_message ("byte after the struct: %u", src[UC_ABI_CALLBACKS_V1_SIZE]);
        return;   /* not reached */
    }
    g_test_trap_subprocess (NULL, 0, (GTestSubprocessFlags) 0);
    g_test_trap_assert_failed ();
}

/* library 0.3.0's wv->cb = *cb, once uc_webview_callbacks has grown, on a host struct of the 0.3.0 size */
static void
test_guard_bites_struct_copy (void)
{
    if (g_test_subprocess ()) {
        Guard guard;
        const void *src = v1_host_struct (&guard);
        no_core_dump ();
        sink = *(const FutureLibraryCallbacks *) src;
        g_test_message ("appended callback read as %p", (void *) sink.added_a);
        return;   /* not reached */
    }
    g_test_trap_subprocess (NULL, 0, (GTestSubprocessFlags) 0);
    g_test_trap_assert_failed ();
}

#ifdef __SANITIZE_ADDRESS__
/* the same copy on an exact-size heap block: AddressSanitizer reports the over-read (make check-asan only) */
static void
test_asan_bites_struct_copy (void)
{
    if (g_test_subprocess ()) {
        const void *src = v1_host_struct (NULL);
        sink = *(const FutureLibraryCallbacks *) src;
        g_test_message ("appended callback read as %p", (void *) sink.added_a);
        return;   /* not reached */
    }
    g_test_trap_subprocess (NULL, 0, (GTestSubprocessFlags) 0);
    g_test_trap_assert_failed ();
    g_test_trap_assert_stderr ("*AddressSanitizer*READ of size*");
}
#endif

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/abi/v1-layout", test_v1_layout);
    g_test_add_func ("/abi/plain-new/guard-page", test_plain_new_guard);
    g_test_add_func ("/abi/plain-new/heap", test_plain_new_heap);
    g_test_add_func ("/abi/sized/guard-page", test_sized_guard);
    g_test_add_func ("/abi/sized/heap", test_sized_heap);
    g_test_add_func ("/abi/layout-0.7.0", test_layout_070);
    g_test_add_func ("/abi/sized-0.6-host/guard-page", test_sized_060_guard);
    g_test_add_func ("/abi/sized-0.6-host/heap", test_sized_060_heap);
    g_test_add_func ("/abi/layout-0.10.0", test_layout_0100);
    g_test_add_func ("/abi/sized-0.9-host/guard-page", test_sized_070_guard);
    g_test_add_func ("/abi/sized-0.9-host/heap", test_sized_070_heap);
    g_test_add_func ("/abi/future-library/guard-page", test_future_library_guard);
    g_test_add_func ("/abi/future-library/heap", test_future_library_heap);
    g_test_add_func ("/abi/newer-host/guard-page", test_newer_host_guard);
    g_test_add_func ("/abi/newer-host/heap", test_newer_host_heap);
    g_test_add_func ("/abi/too-small", test_too_small);
    g_test_add_func ("/abi/null", test_null);
    g_test_add_func ("/abi/between-releases", test_between_releases);
    g_test_add_func ("/abi/guard-bites/one-byte-past", test_guard_bites_one_byte);
    g_test_add_func ("/abi/guard-bites/struct-copy-of-0.3.0", test_guard_bites_struct_copy);
#ifdef __SANITIZE_ADDRESS__
    g_test_add_func ("/abi/asan-bites/struct-copy-of-0.3.0", test_asan_bites_struct_copy);
#endif
    return g_test_run ();
}
