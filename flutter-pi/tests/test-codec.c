/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test of core/ucfl_codec.c (GLib's test framework only): the golden byte files of tests/fixtures decode,
 * the writer reproduces them byte for byte, every value type survives a round trip, and truncated or malformed
 * messages are refused without reading past the end (`make check-asan` runs it with AddressSanitizer and UBSan).
 */
#include "ucfl_codec.h"

#include <errno.h>
#include <string.h>

#include <glib.h>

static GBytes *fixture(const char *name)
{
    g_autofree char *path = g_build_filename(FIXTURES_DIR, name, NULL);
    g_autoptr(GError) error = NULL;
    char *data;
    gsize size;

    g_assert_true(g_file_get_contents(path, &data, &size, &error));
    g_assert_no_error(error);
    return g_bytes_new_take(data, size);
}

/* the message as an exact-size heap block, so that ASan catches a read one byte past it */
static uint8_t *exact_copy(GBytes *bytes, size_t n)
{
    uint8_t *copy = g_malloc(n ? n : 1);
    memcpy(copy, g_bytes_get_data(bytes, NULL), n);
    return copy;
}

static void assert_bytes(const struct ucfl_writer *w, const char *name)
{
    g_autoptr(GBytes) expected = fixture(name);
    gsize size;
    const uint8_t *data = g_bytes_get_data(expected, &size);

    g_assert_false(w->failed);
    g_assert_cmpmem(w->data, w->size, data, size);
}

static void test_decode_create(void)
{
    g_autoptr(GBytes) bytes = fixture("call-create.bin");
    gsize size;
    const uint8_t *msg = g_bytes_get_data(bytes, &size);
    struct ucfl_value method, args;
    int64_t i;
    double d;
    g_autofree char *url = NULL;

    g_assert_cmpint(ucfl_decode_method_call(msg, size, &method, &args), ==, 0);
    g_assert_true(ucfl_value_is_string(&method, "create"));
    g_assert_cmpint(args.type, ==, UCFL_MAP);
    g_assert_cmpuint(args.map.count, ==, 4);
    g_assert_true(ucfl_value_get_int(ucfl_map_get(&args, "width"), &i));
    g_assert_cmpint(i, ==, 480);
    g_assert_true(ucfl_value_get_int(ucfl_map_get(&args, "height"), &i));
    g_assert_cmpint(i, ==, 800);
    url = ucfl_value_dup_string(ucfl_map_get(&args, "url"));
    g_assert_cmpstr(url, ==, "https://example.com/");
    g_assert_cmpint(ucfl_map_get(&args, "pixelRatio")->type, ==, UCFL_FLOAT64);
    g_assert_true(ucfl_value_get_number(ucfl_map_get(&args, "pixelRatio"), &d));
    g_assert_cmpfloat(d, ==, 1.0);
    g_assert_null(ucfl_map_get(&args, "refreshRate"));
    g_assert_false(ucfl_value_get_int(ucfl_map_get(&args, "pixelRatio"), &i)); /* a double is no int */
    ucfl_value_clear(&method);
    ucfl_value_clear(&args);
}

static void test_decode_touch_alignment(void)
{
    /* the two doubles sit at different offsets before their padding: 47 -> 48 (none) and 59 -> 64 (four bytes) */
    g_autoptr(GBytes) bytes = fixture("call-touch.bin");
    gsize size;
    const uint8_t *msg = g_bytes_get_data(bytes, &size);
    struct ucfl_value method, args;
    double x, y;

    g_assert_cmpint(ucfl_decode_method_call(msg, size, &method, &args), ==, 0);
    g_assert_true(ucfl_value_get_number(ucfl_map_get(&args, "x"), &x));
    g_assert_true(ucfl_value_get_number(ucfl_map_get(&args, "y"), &y));
    g_assert_cmpfloat(x, ==, 12.5);
    g_assert_cmpfloat(y, ==, 700.25);
    ucfl_value_clear(&method);
    ucfl_value_clear(&args);
}

static void test_decode_int64_and_long_string(void)
{
    g_autoptr(GBytes) touch = fixture("call-touch-int64.bin");
    g_autoptr(GBytes) create = fixture("call-create-long-url.bin");
    gsize size;
    const uint8_t *msg = g_bytes_get_data(touch, &size);
    struct ucfl_value method, args;
    int64_t i;

    g_assert_cmpint(ucfl_decode_method_call(msg, size, &method, &args), ==, 0);
    g_assert_cmpint(ucfl_map_get(&args, "viewId")->type, ==, UCFL_INT64);
    g_assert_true(ucfl_value_get_int(ucfl_map_get(&args, "viewId"), &i));
    g_assert_cmpint(i, ==, G_GINT64_CONSTANT(1) << 40);
    ucfl_value_clear(&method);
    ucfl_value_clear(&args);

    msg = g_bytes_get_data(create, &size);
    g_assert_cmpint(ucfl_decode_method_call(msg, size, &method, &args), ==, 0);
    g_assert_cmpuint(ucfl_map_get(&args, "url")->str.len, ==, 300); /* size 254 + uint16 */
    ucfl_value_clear(&method);
    ucfl_value_clear(&args);
}

static void test_writer_matches_fixtures(void)
{
    struct ucfl_writer w;

    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, "touch");
    ucfl_put_map(&w, 6);
    ucfl_put_string(&w, "viewId");
    ucfl_put_int(&w, 1);
    ucfl_put_string(&w, "slot");
    ucfl_put_int(&w, 7);
    ucfl_put_string(&w, "type");
    ucfl_put_int(&w, 1);
    ucfl_put_string(&w, "x");
    ucfl_put_double(&w, 12.5);
    ucfl_put_string(&w, "y");
    ucfl_put_double(&w, 700.25);
    ucfl_put_string(&w, "timeMs");
    ucfl_put_int(&w, 123456);
    assert_bytes(&w, "call-touch.bin");
    ucfl_writer_free(&w);

    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, "touch");
    ucfl_put_map(&w, 5);
    ucfl_put_string(&w, "viewId");
    ucfl_put_int(&w, G_GINT64_CONSTANT(1) << 40);
    ucfl_put_string(&w, "slot");
    ucfl_put_int(&w, (G_GINT64_CONSTANT(1) << 33) + 5);
    ucfl_put_string(&w, "type");
    ucfl_put_int(&w, 3);
    ucfl_put_string(&w, "x");
    ucfl_put_double(&w, -1.0);
    ucfl_put_string(&w, "y");
    ucfl_put_double(&w, 0.0);
    assert_bytes(&w, "call-touch-int64.bin");
    ucfl_writer_free(&w);

    ucfl_writer_init(&w);
    ucfl_put_error_envelope(&w, "noView", "unknown viewId");
    assert_bytes(&w, "reply-error-no-view.bin");
    ucfl_writer_free(&w);
}

static void test_round_trip(void)
{
    static const int64_t ints[] = { 0, 1, -1, INT32_MAX, INT32_MIN, (int64_t) INT32_MAX + 1, (int64_t) INT32_MIN - 1,
                                    INT64_MAX, INT64_MIN };
    static const size_t string_sizes[] = { 0, 253, 254, 0xffff, 0x10000 };
    struct ucfl_writer w;
    struct ucfl_value v;
    size_t pos = 0;

    ucfl_writer_init(&w);
    ucfl_put_list(&w, G_N_ELEMENTS(ints) + G_N_ELEMENTS(string_sizes) + 6);
    for (size_t i = 0; i < G_N_ELEMENTS(ints); i++)
        ucfl_put_int(&w, ints[i]);
    for (size_t i = 0; i < G_N_ELEMENTS(string_sizes); i++) {
        g_autofree char *s = g_strnfill(string_sizes[i], 'x');
        ucfl_put_string(&w, s);
    }
    ucfl_put_null(&w);
    ucfl_put_bool(&w, true);
    ucfl_put_bool(&w, false);
    ucfl_put_double(&w, -0.125);
    ucfl_put_map(&w, 1);
    ucfl_put_string(&w, "nested");
    ucfl_put_list(&w, 1);
    ucfl_put_double(&w, 3.5);
    ucfl_put_string(&w, "\xc3\xa4"); /* UTF-8 passes as bytes */
    g_assert_false(w.failed);

    g_assert_cmpint(ucfl_decode_value(w.data, w.size, &pos, &v), ==, 0);
    g_assert_cmpuint(pos, ==, w.size);
    g_assert_cmpint(v.type, ==, UCFL_LIST);
    size_t k = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(ints); i++, k++) {
        int64_t got;
        g_assert_true(ucfl_value_get_int(&v.list.items[k], &got));
        g_assert_cmpint(got, ==, ints[i]);
        g_assert_cmpint(v.list.items[k].type, ==, ints[i] >= INT32_MIN && ints[i] <= INT32_MAX ? UCFL_INT32 : UCFL_INT64);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(string_sizes); i++, k++)
        g_assert_cmpuint(v.list.items[k].str.len, ==, string_sizes[i]);
    g_assert_cmpint(v.list.items[k++].type, ==, UCFL_NULL);
    g_assert_cmpint(v.list.items[k++].type, ==, UCFL_TRUE);
    g_assert_cmpint(v.list.items[k++].type, ==, UCFL_FALSE);
    g_assert_cmpfloat(v.list.items[k++].d, ==, -0.125);
    const struct ucfl_value *nested = ucfl_map_get(&v.list.items[k++], "nested");
    g_assert_nonnull(nested);
    g_assert_cmpfloat(nested->list.items[0].d, ==, 3.5);
    g_assert_true(ucfl_value_is_string(&v.list.items[k++], "\xc3\xa4"));
    ucfl_value_clear(&v);
    ucfl_writer_free(&w);
}

static void test_typed_lists(void)
{
    /* Int32List [1, -2] at offset 0: type, size, two bytes of padding to 4, then the elements */
    static const uint8_t int32_list[] = { 9, 2, 0, 0, 1, 0, 0, 0, 0xfe, 0xff, 0xff, 0xff };
    static const uint8_t float64_list[] = { 11, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f };
    struct ucfl_value v;
    size_t pos = 0;

    g_assert_cmpint(ucfl_decode_value(int32_list, sizeof(int32_list), &pos, &v), ==, 0);
    g_assert_cmpint(v.type, ==, UCFL_INT32_LIST);
    g_assert_cmpuint(v.array.count, ==, 2);
    g_assert_true(v.array.data == int32_list + 4);
    pos = 0;
    g_assert_cmpint(ucfl_decode_value(float64_list, sizeof(float64_list), &pos, &v), ==, 0);
    g_assert_cmpint(v.type, ==, UCFL_FLOAT64_LIST);
    g_assert_true(v.array.data == float64_list + 8);
    pos = 0;
    g_assert_cmpint(ucfl_decode_value(float64_list, sizeof(float64_list) - 1, &pos, &v), ==, EBADMSG);
}

/* every prefix of every fixture is refused, none reads past its end */
static void test_truncated(void)
{
    static const char *const calls[] = { "call-hello.bin", "call-create.bin", "call-create-long-url.bin", "call-touch.bin",
                                         "call-touch-int64.bin", "call-key.bin", "call-set-mapped.bin", "call-dispose.bin",
                                         "call-listen.bin" };
    static const char *const envelopes[] = { "reply-hello-unavailable.bin", "reply-hello-available.bin",
                                             "reply-create.bin", "reply-null.bin", "reply-error-no-view.bin",
                                             "event-load-changed.bin", "event-progress.bin", "event-frame.bin",
                                             "event-terminated.bin" };

    for (size_t f = 0; f < G_N_ELEMENTS(calls) + G_N_ELEMENTS(envelopes); f++) {
        bool is_call = f < G_N_ELEMENTS(calls);
        g_autoptr(GBytes) bytes = fixture(is_call ? calls[f] : envelopes[f - G_N_ELEMENTS(calls)]);
        gsize size = g_bytes_get_size(bytes);

        for (size_t n = 0; n <= size; n++) {
            uint8_t *msg = exact_copy(bytes, n);
            struct ucfl_value a, b;
            bool is_error;
            int err = is_call ? ucfl_decode_method_call(msg, n, &a, &b) : ucfl_decode_envelope(msg, n, &is_error, &a);

            g_assert_cmpint(err, ==, n == size ? 0 : EBADMSG);
            if (!err && !is_call)
                g_assert_cmpint(is_error, ==, msg[0] == 1);
            ucfl_value_clear(&a);
            if (is_call)
                ucfl_value_clear(&b);
            g_free(msg);
        }
    }
}

static void test_malformed(void)
{
    static const uint8_t unknown_type[] = { 7, 1, 'm', 15 };
    static const uint8_t large_int[] = { 7, 1, 'm', 5, 1, '1' };
    static const uint8_t method_not_string[] = { 3, 1, 0, 0, 0, 0 };
    static const uint8_t trailing[] = { 7, 1, 'm', 0, 0 };
    /* a list of 0xffffffff elements in a 9 byte message: refused before allocating */
    static const uint8_t huge_list[] = { 7, 1, 'm', 12, 255, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t huge_map[] = { 7, 1, 'm', 13, 255, 0xff, 0xff, 0xff, 0x7f };
    static const uint8_t huge_string[] = { 7, 255, 0xff, 0xff, 0xff, 0xff, 'm' };
    static const uint8_t bad_envelope[] = { 2, 0 };
    struct ucfl_value a, b;
    bool is_error;

    g_assert_cmpint(ucfl_decode_method_call(unknown_type, sizeof(unknown_type), &a, &b), ==, EBADMSG);
    ucfl_value_clear(&a);
    g_assert_cmpint(ucfl_decode_method_call(large_int, sizeof(large_int), &a, &b), ==, EBADMSG);
    ucfl_value_clear(&a);
    g_assert_cmpint(ucfl_decode_method_call(method_not_string, sizeof(method_not_string), &a, &b), ==, EBADMSG);
    ucfl_value_clear(&a);
    g_assert_cmpint(ucfl_decode_method_call(trailing, sizeof(trailing), &a, &b), ==, EBADMSG);
    ucfl_value_clear(&a);
    ucfl_value_clear(&b);
    g_assert_cmpint(ucfl_decode_method_call(huge_list, sizeof(huge_list), &a, &b), ==, EBADMSG);
    ucfl_value_clear(&a);
    g_assert_cmpint(ucfl_decode_method_call(huge_map, sizeof(huge_map), &a, &b), ==, EBADMSG);
    ucfl_value_clear(&a);
    g_assert_cmpint(ucfl_decode_method_call(huge_string, sizeof(huge_string), &a, &b), ==, EBADMSG);
    g_assert_cmpint(ucfl_decode_envelope(bad_envelope, sizeof(bad_envelope), &is_error, &a), ==, EBADMSG);
    g_assert_cmpint(ucfl_decode_envelope(NULL, 0, &is_error, &a), ==, EBADMSG);
    g_assert_null(ucfl_value_dup_string(NULL));
}

/* method "m", then nested lists at the depths 0..max_depth (each holds the next, the innermost is empty) */
static size_t nested_call(uint8_t *msg, int max_depth)
{
    size_t n = 0;

    msg[n++] = UCFL_STRING;
    msg[n++] = 1;
    msg[n++] = 'm';
    for (int depth = 0; depth <= max_depth; depth++) {
        msg[n++] = UCFL_LIST;
        msg[n++] = depth == max_depth ? 0 : 1;
    }
    return n;
}

static void test_depth_limit(void)
{
    g_autofree uint8_t *msg = g_malloc(3 + 2 * (UCFL_MAX_DEPTH + 2));
    struct ucfl_value a, b;
    size_t n;

    n = nested_call(msg, UCFL_MAX_DEPTH);
    g_assert_cmpint(ucfl_decode_method_call(msg, n, &a, &b), ==, 0);
    ucfl_value_clear(&a);
    ucfl_value_clear(&b);

    n = nested_call(msg, UCFL_MAX_DEPTH + 1);
    g_assert_cmpint(ucfl_decode_method_call(msg, n, &a, &b), ==, EBADMSG);
    ucfl_value_clear(&a);
    ucfl_value_clear(&b);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/codec/decode-create", test_decode_create);
    g_test_add_func("/codec/decode-touch-alignment", test_decode_touch_alignment);
    g_test_add_func("/codec/decode-int64-long-string", test_decode_int64_and_long_string);
    g_test_add_func("/codec/writer-matches-fixtures", test_writer_matches_fixtures);
    g_test_add_func("/codec/round-trip", test_round_trip);
    g_test_add_func("/codec/typed-lists", test_typed_lists);
    g_test_add_func("/codec/truncated", test_truncated);
    g_test_add_func("/codec/malformed", test_malformed);
    g_test_add_func("/codec/depth-limit", test_depth_limit);
    return g_test_run();
}
