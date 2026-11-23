/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test of core/ucfl_protocol.c (GLib's test framework only): the calls of the golden files parse into the
 * values Dart sent, invalid arguments are refused with a reason, and the replies and events are byte for byte the
 * golden files.
 */
#include "ucfl_protocol.h"

#include <errno.h>
#include <string.h>

#include <glib.h>

#include "uc_webview.h"

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

static int parse_fixture(const char *name, bool events, struct ucfl_call *call)
{
    g_autoptr(GBytes) bytes = fixture(name);
    gsize size;
    const uint8_t *msg = g_bytes_get_data(bytes, &size);
    const char *error;

    return ucfl_parse_call(msg, size, events, call, &error);
}

static void assert_fixture(struct ucfl_writer *w, const char *name)
{
    g_autoptr(GBytes) expected = fixture(name);
    gsize size;
    const uint8_t *data = g_bytes_get_data(expected, &size);

    g_assert_false(w->failed);
    g_assert_cmpmem(w->data, w->size, data, size);
    ucfl_writer_free(w);
}

static void test_calls(void)
{
    struct ucfl_call call;

    g_assert_cmpint(parse_fixture("call-hello.bin", false, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_HELLO);
    ucfl_call_clear(&call);

    g_assert_cmpint(parse_fixture("call-create.bin", false, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_CREATE);
    g_assert_cmpint(call.create.width, ==, 480);
    g_assert_cmpint(call.create.height, ==, 800);
    g_assert_cmpstr(call.create.url, ==, "https://example.com/");
    g_assert_cmpfloat(call.create.pixel_ratio, ==, 1.0);
    g_assert_cmpfloat(call.create.refresh_rate, ==, 0.0);
    ucfl_call_clear(&call);
    g_assert_null(call.create.url);

    g_assert_cmpint(parse_fixture("call-create-long-url.bin", false, &call), ==, 0);
    g_assert_cmpuint(strlen(call.create.url), ==, 300);
    ucfl_call_clear(&call);

    g_assert_cmpint(parse_fixture("call-touch.bin", false, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_TOUCH);
    g_assert_cmpint(call.view_id, ==, 1);
    g_assert_cmpint(call.touch.slot, ==, 7);
    g_assert_cmpint(call.touch.type, ==, UC_WEBVIEW_TOUCH_MOVE);
    g_assert_cmpfloat(call.touch.x, ==, 12.5);
    g_assert_cmpfloat(call.touch.y, ==, 700.25);
    g_assert_cmpuint(call.touch.time_ms, ==, 123456);
    ucfl_call_clear(&call);

    /* Dart's ints beyond int32: the view id as it is, the pointer id reduced to the library's int */
    g_assert_cmpint(parse_fixture("call-touch-int64.bin", false, &call), ==, 0);
    g_assert_cmpint(call.view_id, ==, G_GINT64_CONSTANT(1) << 40);
    g_assert_cmpint(call.touch.slot, ==, 5);
    g_assert_cmpint(call.touch.type, ==, UC_WEBVIEW_TOUCH_CANCEL);
    g_assert_cmpuint(call.touch.time_ms, ==, 0);
    ucfl_call_clear(&call);

    g_assert_cmpint(parse_fixture("call-key.bin", false, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_KEY);
    g_assert_cmpint(call.view_id, ==, 3);
    g_assert_cmpuint(call.key.keysym, ==, 0xff0d);
    g_assert_cmpuint(call.key.keycode, ==, 36);
    g_assert_true(call.key.pressed);
    ucfl_call_clear(&call);

    g_assert_cmpint(parse_fixture("call-set-mapped.bin", false, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_SET_MAPPED);
    g_assert_false(call.set_mapped.mapped);
    ucfl_call_clear(&call);

    g_assert_cmpint(parse_fixture("call-dispose.bin", false, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_DISPOSE);
    g_assert_cmpint(call.view_id, ==, 1);
    ucfl_call_clear(&call);
}

static void test_channels(void)
{
    struct ucfl_call call;

    g_assert_cmpint(parse_fixture("call-listen.bin", true, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_LISTEN);
    /* each channel knows only its own methods: an unknown one is "not implemented" */
    g_assert_cmpint(parse_fixture("call-listen.bin", false, &call), ==, ENOTSUP);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_UNKNOWN);
    g_assert_cmpint(parse_fixture("call-hello.bin", true, &call), ==, ENOTSUP);
    /* a reply is no call */
    g_assert_cmpint(parse_fixture("reply-null.bin", false, &call), ==, EBADMSG);
}

/* method with a map of arguments built by build() */
static int parse_built(const char *method, void (*build)(struct ucfl_writer *), const char **error)
{
    struct ucfl_writer w;
    struct ucfl_call call;
    int err;

    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, method);
    build(&w);
    g_assert_false(w.failed);
    err = ucfl_parse_call(w.data, w.size, false, &call, error);
    ucfl_call_clear(&call);
    ucfl_writer_free(&w);
    return err;
}

static void args_null(struct ucfl_writer *w)
{
    ucfl_put_null(w);
}
static void create_no_height(struct ucfl_writer *w)
{
    ucfl_put_map(w, 1);
    ucfl_put_string(w, "width");
    ucfl_put_int(w, 10);
}
static void create_width_zero(struct ucfl_writer *w)
{
    ucfl_put_map(w, 2);
    ucfl_put_string(w, "width");
    ucfl_put_int(w, 0);
    ucfl_put_string(w, "height");
    ucfl_put_int(w, 10);
}
static void create_width_double(struct ucfl_writer *w)
{
    ucfl_put_map(w, 2);
    ucfl_put_string(w, "width");
    ucfl_put_double(w, 10.0);
    ucfl_put_string(w, "height");
    ucfl_put_int(w, 10);
}
static void create_url_int(struct ucfl_writer *w)
{
    ucfl_put_map(w, 3);
    ucfl_put_string(w, "width");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "height");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "url");
    ucfl_put_int(w, 1);
}
static void create_url_nul(struct ucfl_writer *w)
{
    ucfl_put_map(w, 3);
    ucfl_put_string(w, "width");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "height");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "url");
    ucfl_put_string_len(w, "a\0b", 3);
}
static void create_ratio_negative(struct ucfl_writer *w)
{
    ucfl_put_map(w, 3);
    ucfl_put_string(w, "width");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "height");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "pixelRatio");
    ucfl_put_double(w, -1.0);
}
static void create_optional_null(struct ucfl_writer *w)
{
    ucfl_put_map(w, 5);
    ucfl_put_string(w, "width");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "height");
    ucfl_put_int(w, 10);
    ucfl_put_string(w, "url");
    ucfl_put_null(w);
    ucfl_put_string(w, "pixelRatio");
    ucfl_put_null(w);
    ucfl_put_string(w, "refreshRate");
    ucfl_put_int(w, 60); /* an int where a double is named */
}
static void touch_type_4(struct ucfl_writer *w)
{
    ucfl_put_map(w, 5);
    ucfl_put_string(w, "viewId");
    ucfl_put_int(w, 1);
    ucfl_put_string(w, "slot");
    ucfl_put_int(w, 1);
    ucfl_put_string(w, "type");
    ucfl_put_int(w, 4);
    ucfl_put_string(w, "x");
    ucfl_put_int(w, 1);
    ucfl_put_string(w, "y");
    ucfl_put_int(w, 1);
}
static void touch_x_string(struct ucfl_writer *w)
{
    ucfl_put_map(w, 5);
    ucfl_put_string(w, "viewId");
    ucfl_put_int(w, 1);
    ucfl_put_string(w, "slot");
    ucfl_put_int(w, 1);
    ucfl_put_string(w, "type");
    ucfl_put_int(w, 0);
    ucfl_put_string(w, "x");
    ucfl_put_string(w, "1");
    ucfl_put_string(w, "y");
    ucfl_put_int(w, 1);
}
static void key_no_pressed(struct ucfl_writer *w)
{
    ucfl_put_map(w, 2);
    ucfl_put_string(w, "viewId");
    ucfl_put_int(w, 1);
    ucfl_put_string(w, "keysym");
    ucfl_put_int(w, 0x61);
}
static void view_zero(struct ucfl_writer *w)
{
    ucfl_put_map(w, 1);
    ucfl_put_string(w, "viewId");
    ucfl_put_int(w, 0);
}

static void test_invalid_arguments(void)
{
    const char *error;

    g_assert_cmpint(parse_built("create", args_null, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "width"));
    g_assert_cmpint(parse_built("create", create_no_height, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "height"));
    g_assert_cmpint(parse_built("create", create_width_zero, &error), ==, EINVAL);
    g_assert_cmpint(parse_built("create", create_width_double, &error), ==, EINVAL);
    g_assert_cmpint(parse_built("create", create_url_int, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "url"));
    g_assert_cmpint(parse_built("create", create_url_nul, &error), ==, EINVAL);
    g_assert_cmpint(parse_built("create", create_ratio_negative, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "pixelRatio"));
    g_assert_cmpint(parse_built("create", create_optional_null, &error), ==, 0);
    g_assert_cmpint(parse_built("touch", touch_type_4, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "type"));
    g_assert_cmpint(parse_built("touch", touch_x_string, &error), ==, EINVAL);
    g_assert_cmpint(parse_built("key", key_no_pressed, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "pressed"));
    g_assert_cmpint(parse_built("setMapped", view_zero, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "viewId"));
    g_assert_cmpint(parse_built("dispose", args_null, &error), ==, EINVAL);
    g_assert_cmpint(parse_built("stats", args_null, &error), ==, 0);
}

static void test_replies_and_events(void)
{
    static const char *const features[] = { "refreshRate", "newSized" };
    struct ucfl_writer w;

    ucfl_writer_init(&w);
    ucfl_put_hello_reply(&w, &(struct ucfl_hello) { .available = false, .reason = "no library", .abi = -1 });
    assert_fixture(&w, "reply-hello-unavailable.bin");

    ucfl_writer_init(&w);
    ucfl_put_hello_reply(&w, &(struct ucfl_hello) { .available = true, .abi = 1, .features = features, .n_features = 2 });
    assert_fixture(&w, "reply-hello-available.bin");

    ucfl_writer_init(&w);
    ucfl_put_create_reply(&w, 1, 2);
    assert_fixture(&w, "reply-create.bin");

    ucfl_writer_init(&w);
    ucfl_put_null_reply(&w);
    assert_fixture(&w, "reply-null.bin");

    ucfl_writer_init(&w);
    ucfl_put_event_load_changed(&w, 1, UC_WEBVIEW_LOAD_FINISHED, "about:blank");
    assert_fixture(&w, "event-load-changed.bin");

    ucfl_writer_init(&w);
    ucfl_put_event_progress(&w, 1, 0.5);
    assert_fixture(&w, "event-progress.bin");

    ucfl_writer_init(&w);
    ucfl_put_event_frame(&w, 1, 480, 800, true);
    assert_fixture(&w, "event-frame.bin");

    ucfl_writer_init(&w);
    ucfl_put_event_terminated(&w, 2, UC_WEBVIEW_TERMINATED_CRASHED);
    assert_fixture(&w, "event-terminated.bin");
}

static void test_names(void)
{
    g_assert_cmpstr(ucfl_load_event_name(UC_WEBVIEW_LOAD_STARTED), ==, "started");
    g_assert_cmpstr(ucfl_load_event_name(UC_WEBVIEW_LOAD_REDIRECTED), ==, "redirected");
    g_assert_cmpstr(ucfl_load_event_name(UC_WEBVIEW_LOAD_COMMITTED), ==, "committed");
    g_assert_cmpstr(ucfl_load_event_name(UC_WEBVIEW_LOAD_FAILED), ==, "failed");
    g_assert_cmpstr(ucfl_load_event_name(99), ==, "unknown");
    g_assert_cmpstr(ucfl_termination_name(UC_WEBVIEW_TERMINATED_EXCEEDED_MEMORY_LIMIT), ==, "exceededMemoryLimit");
    g_assert_cmpstr(ucfl_termination_name(UC_WEBVIEW_TERMINATED_BY_API), ==, "byApi");
    g_assert_cmpstr(ucfl_termination_name(-1), ==, "unknown");
}

static void test_stats(void)
{
    static const struct ucfl_stat stats[] = { { "a", 1 }, { "b", G_GINT64_CONSTANT(1) << 40 } };
    struct ucfl_writer w;
    struct ucfl_value v;
    bool is_error;
    int64_t i;

    ucfl_writer_init(&w);
    ucfl_put_stats_reply(&w, stats, 2);
    g_assert_cmpint(ucfl_decode_envelope(w.data, w.size, &is_error, &v), ==, 0);
    g_assert_false(is_error);
    g_assert_true(ucfl_value_get_int(ucfl_map_get(&v, "b"), &i));
    g_assert_cmpint(i, ==, G_GINT64_CONSTANT(1) << 40);
    ucfl_value_clear(&v);
    ucfl_writer_free(&w);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/protocol/calls", test_calls);
    g_test_add_func("/protocol/channels", test_channels);
    g_test_add_func("/protocol/invalid-arguments", test_invalid_arguments);
    g_test_add_func("/protocol/replies-and-events", test_replies_and_events);
    g_test_add_func("/protocol/names", test_names);
    g_test_add_func("/protocol/stats", test_stats);
    return g_test_run();
}
