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
    g_assert_false(call.flag.value);
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


/* every call of the protocol parses into what Dart sent (the fixtures of gen-fixtures.py) */
static void test_all_calls(void)
{
    struct ucfl_call call;

#define PARSE(name, m)                                                                                                 \
    do {                                                                                                               \
        g_assert_cmpint(parse_fixture(name, false, &call), ==, 0);                                                     \
        g_assert_cmpint(call.method, ==, (m));                                                                         \
    } while (0)

    PARSE("call-initialize.bin", UCFL_METHOD_INITIALIZE);
    g_assert_cmpfloat(call.initialize.refresh_rate, ==, 48.61);
    ucfl_call_clear(&call);
    PARSE("call-set-refresh-rate.bin", UCFL_METHOD_SET_REFRESH_RATE);
    g_assert_cmpfloat(call.set_refresh_rate.hz, ==, 50.0);
    ucfl_call_clear(&call);
    PARSE("call-set-color-scheme.bin", UCFL_METHOD_SET_COLOR_SCHEME);
    g_assert_cmpint(call.set_color_scheme.scheme, ==, UC_WEBVIEW_COLOR_SCHEME_DARK);
    ucfl_call_clear(&call);
    PARSE("call-set-tls-errors-policy.bin", UCFL_METHOD_SET_TLS_ERRORS_POLICY);
    g_assert_true(call.set_tls_errors_policy.ignore);
    ucfl_call_clear(&call);
    PARSE("call-allow-certificate.bin", UCFL_METHOD_ALLOW_CERTIFICATE);
    g_assert_cmpstr(call.allow_certificate.host, ==, "192.168.1.20");
    g_assert_cmpuint(strlen(call.allow_certificate.sha256), ==, 64);
    ucfl_call_clear(&call);

    PARSE("call-set-cookies.bin", UCFL_METHOD_SET_COOKIES);
    g_assert_cmpstr(call.set_cookies.url, ==, "https://example.com/");
    g_assert_cmpuint(call.set_cookies.count, ==, 2);
    g_assert_cmpstr(call.set_cookies.cookies[0].name, ==, "session");
    g_assert_cmpstr(call.set_cookies.cookies[0].value, ==, "s3cr3t-cookie");
    g_assert_cmpstr(call.set_cookies.cookies[0].domain, ==, "example.com");
    g_assert_cmpstr(call.set_cookies.cookies[0].path, ==, "/");
    g_assert_cmpint(call.set_cookies.cookies[0].secure, ==, 1);
    g_assert_cmpint(call.set_cookies.cookies[0].http_only, ==, 1);
    g_assert_cmpint(call.set_cookies.cookies[0].expires, ==, 1900000000);
    g_assert_cmpstr(call.set_cookies.cookies[1].name, ==, "lang");
    g_assert_null(call.set_cookies.cookies[1].domain);
    g_assert_null(call.set_cookies.cookies[1].path);
    g_assert_cmpint(call.set_cookies.cookies[1].secure, ==, 0);
    g_assert_cmpint(call.set_cookies.cookies[1].expires, ==, -1); /* null: a session cookie */
    ucfl_call_clear(&call);

    PARSE("call-clear-data.bin", UCFL_METHOD_CLEAR_DATA);
    g_assert_cmpuint(call.clear_data.types, ==, UC_WEBVIEW_DATA_ALL);
    ucfl_call_clear(&call);
    PARSE("call-stats.bin", UCFL_METHOD_STATS);
    ucfl_call_clear(&call);

    PARSE("call-create-full.bin", UCFL_METHOD_CREATE);
    g_assert_cmpfloat(call.create.pixel_ratio, ==, 1.5);
    g_assert_cmpfloat(call.create.refresh_rate, ==, 48.61);
    g_assert_false(call.create.pinch_zoom);
    g_assert_true(call.create.requests);
    ucfl_call_clear(&call);
    PARSE("call-create.bin", UCFL_METHOD_CREATE); /* defaults */
    g_assert_true(call.create.pinch_zoom);
    g_assert_true(call.create.requests);
    ucfl_call_clear(&call);

    PARSE("call-resize.bin", UCFL_METHOD_RESIZE);
    g_assert_cmpint(call.view_id, ==, 1);
    g_assert_cmpint(call.resize.width, ==, 800);
    g_assert_cmpint(call.resize.height, ==, 480);
    ucfl_call_clear(&call);
    PARSE("call-set-focused.bin", UCFL_METHOD_SET_FOCUSED);
    g_assert_true(call.flag.value);
    ucfl_call_clear(&call);
    PARSE("call-set-scale.bin", UCFL_METHOD_SET_SCALE);
    g_assert_cmpfloat(call.number.value, ==, 1.5);
    ucfl_call_clear(&call);
    PARSE("call-set-background-color.bin", UCFL_METHOD_SET_BACKGROUND_COLOR);
    g_assert_cmpfloat(call.color.a, ==, 1.0f);
    g_assert_cmpfloat(call.color.r, ==, 0x33 / 255.0f);
    g_assert_cmpfloat(call.color.g, ==, 0x66 / 255.0f);
    g_assert_cmpfloat(call.color.b, ==, 0x99 / 255.0f);
    ucfl_call_clear(&call);
    PARSE("call-set-frame-hold.bin", UCFL_METHOD_SET_FRAME_HOLD);
    g_assert_true(call.flag.value);
    ucfl_call_clear(&call);
    PARSE("call-load-uri.bin", UCFL_METHOD_LOAD_URI);
    g_assert_cmpstr(call.load_uri.uri, ==, "https://user:pw@example.com/a?token=t0k#f");
    ucfl_call_clear(&call);
    PARSE("call-load-html.bin", UCFL_METHOD_LOAD_HTML);
    g_assert_cmpstr(call.load_html.html, ==, "<p>Hi</p>");
    g_assert_cmpstr(call.load_html.base_uri, ==, "https://example.com/");
    ucfl_call_clear(&call);
    PARSE("call-reload.bin", UCFL_METHOD_RELOAD);
    ucfl_call_clear(&call);
    PARSE("call-stop.bin", UCFL_METHOD_STOP);
    ucfl_call_clear(&call);
    PARSE("call-go-back.bin", UCFL_METHOD_GO_BACK);
    ucfl_call_clear(&call);
    PARSE("call-go-forward.bin", UCFL_METHOD_GO_FORWARD);
    ucfl_call_clear(&call);
    PARSE("call-get-state.bin", UCFL_METHOD_GET_STATE);
    ucfl_call_clear(&call);
    PARSE("call-run-javascript.bin", UCFL_METHOD_RUN_JAVASCRIPT);
    g_assert_cmpstr(call.run_javascript.script, ==, "window.scrollTo(0, 0)");
    ucfl_call_clear(&call);
    PARSE("call-set-zoom.bin", UCFL_METHOD_SET_ZOOM);
    g_assert_cmpfloat(call.number.value, ==, 1.25);
    ucfl_call_clear(&call);
    PARSE("call-set-magnification.bin", UCFL_METHOD_SET_MAGNIFICATION);
    g_assert_cmpfloat(call.set_magnification.scale, ==, 2.0);
    g_assert_cmpfloat(call.set_magnification.x, ==, 240.0);
    g_assert_cmpfloat(call.set_magnification.y, ==, 400.0);
    ucfl_call_clear(&call);
    PARSE("call-when-rendered.bin", UCFL_METHOD_WHEN_RENDERED);
    ucfl_call_clear(&call);

    PARSE("call-pointer-button.bin", UCFL_METHOD_POINTER_BUTTON);
    g_assert_cmpint(call.pointer_button.button, ==, UC_WEBVIEW_BUTTON_PRIMARY);
    g_assert_true(call.pointer_button.pressed);
    g_assert_cmpfloat(call.pointer_button.x, ==, 10.0);
    g_assert_cmpuint(call.pointer_button.modifiers, ==, UC_WEBVIEW_MOD_SHIFT);
    g_assert_cmpuint(call.pointer_button.time_ms, ==, 1000);
    ucfl_call_clear(&call);
    PARSE("call-pointer-move.bin", UCFL_METHOD_POINTER_MOVE);
    g_assert_cmpfloat(call.pointer_move.y, ==, 21.5);
    g_assert_cmpuint(call.pointer_move.modifiers, ==, UC_WEBVIEW_MOD_BUTTON1);
    ucfl_call_clear(&call);
    PARSE("call-scroll.bin", UCFL_METHOD_SCROLL);
    g_assert_cmpfloat(call.scroll.dy, ==, 2.0);
    g_assert_false(call.scroll.precise);
    g_assert_cmpuint(call.scroll.time_ms, ==, 1020);
    ucfl_call_clear(&call);
    PARSE("call-key-full.bin", UCFL_METHOD_KEY);
    g_assert_cmpuint(call.key.keysym, ==, 0xff09);
    g_assert_cmpuint(call.key.modifiers, ==, UC_WEBVIEW_MOD_SHIFT);
    g_assert_cmpuint(call.key.time_ms, ==, 1030);
    ucfl_call_clear(&call);
    PARSE("call-ime-commit.bin", UCFL_METHOD_IME_COMMIT);
    g_assert_cmpstr(call.ime_commit.text, ==, "Gr\xc3\xbc" "ezi \xf0\x9f\x91\x8b");
    ucfl_call_clear(&call);
    PARSE("call-ime-preedit.bin", UCFL_METHOD_IME_PREEDIT);
    g_assert_cmpstr(call.ime_preedit.text, ==, "ab");
    g_assert_cmpint(call.ime_preedit.cursor, ==, 2);
    ucfl_call_clear(&call);
    PARSE("call-ime-delete-surrounding.bin", UCFL_METHOD_IME_DELETE_SURROUNDING);
    g_assert_cmpint(call.ime_delete_surrounding.offset, ==, -1);
    g_assert_cmpint(call.ime_delete_surrounding.count, ==, 1);
    ucfl_call_clear(&call);

    PARSE("call-reply-dialog.bin", UCFL_METHOD_REPLY_DIALOG);
    g_assert_cmpint(call.reply_dialog.request_id, ==, 1);
    g_assert_true(call.reply_dialog.accept);
    g_assert_cmpstr(call.reply_dialog.text, ==, "Alice");
    ucfl_call_clear(&call);
    PARSE("call-reply-auth.bin", UCFL_METHOD_REPLY_AUTH);
    g_assert_cmpint(call.reply_auth.request_id, ==, 2);
    g_assert_cmpstr(call.reply_auth.user, ==, "alice");
    g_assert_cmpstr(call.reply_auth.password, ==, "p4ssw0rd");
    ucfl_call_clear(&call);
    PARSE("call-set-origin-headers.bin", UCFL_METHOD_SET_ORIGIN_HEADERS);
    g_assert_cmpstr(call.set_origin_headers.origin, ==, "https://example.com");
    g_assert_cmpuint(call.set_origin_headers.count, ==, 2);
    g_assert_cmpstr(call.set_origin_headers.names[0], ==, "Authorization");
    g_assert_cmpstr(call.set_origin_headers.values[0], ==, "Bearer t0ken");
    g_assert_cmpstr(call.set_origin_headers.names[1], ==, "X-Api-Key");
    g_assert_cmpstr(call.set_origin_headers.values[1], ==, "k3y");
    ucfl_call_clear(&call);
    PARSE("call-set-user-agent.bin", UCFL_METHOD_SET_USER_AGENT);
    g_assert_cmpint(call.set_user_agent.preset, ==, UC_WEBVIEW_USER_AGENT_CUSTOM);
    g_assert_cmpstr(call.set_user_agent.custom, ==, "MyAgent/1.0");
    ucfl_call_clear(&call);
    PARSE("call-set-device-api.bin", UCFL_METHOD_SET_DEVICE_API);
    g_assert_true(call.flag.value);
    ucfl_call_clear(&call);
    PARSE("call-set-device-state.bin", UCFL_METHOD_SET_DEVICE_STATE);
    g_assert_cmpstr(call.set_device_state.json, ==, "{\"screen\":\"dim\",\"powerSource\":\"battery\",\"level\":42}");
    ucfl_call_clear(&call);
    PARSE("call-device-event.bin", UCFL_METHOD_DEVICE_EVENT);
    g_assert_cmpstr(call.device_event.type, ==, "resume");
    g_assert_cmpint(call.device_event.slept_ms, ==, 3600000);
    ucfl_call_clear(&call);

    g_assert_cmpint(parse_fixture("call-cancel.bin", true, &call), ==, 0);
    g_assert_cmpint(call.method, ==, UCFL_METHOD_CANCEL);
    ucfl_call_clear(&call);
#undef PARSE

    /* the names round trip */
    for (int m = UCFL_METHOD_UNKNOWN + 1; m < UCFL_METHOD_COUNT; m++)
        g_assert_cmpstr(ucfl_method_name((enum ucfl_method) m), !=, "");
    g_assert_cmpstr(ucfl_method_name(UCFL_METHOD_UNKNOWN), ==, "");
    g_assert_true(ucfl_method_has_view(UCFL_METHOD_TOUCH));
    g_assert_false(ucfl_method_has_view(UCFL_METHOD_CREATE));
}

/* a call of method with viewId 1 and the given string arguments */
static int parse_map(const char *method, const char *const *keys, const char *const *strings, size_t n,
                     const char **error)
{
    struct ucfl_writer w;
    struct ucfl_call call;
    int err;

    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, method);
    ucfl_put_map(&w, n + 1);
    ucfl_put_string(&w, "viewId");
    ucfl_put_int(&w, 1);
    for (size_t i = 0; i < n; i++) {
        ucfl_put_string(&w, keys[i]);
        ucfl_put_string(&w, strings[i]);
    }
    err = ucfl_parse_call(w.data, w.size, false, &call, error);
    ucfl_call_clear(&call);
    ucfl_writer_free(&w);
    return err;
}

static void test_invalid_enums(void)
{
    const char *error;
    const char *key[] = { "preset" }, *bad[] = { "tablet" };
    const char *custom_keys[] = { "preset" }, *custom[] = { "custom" };
    const char *type_keys[] = { "type" }, *type[] = { "hibernate" };

    g_assert_cmpint(parse_map("setUserAgent", key, bad, 1, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "preset"));
    /* custom without its text */
    g_assert_cmpint(parse_map("setUserAgent", custom_keys, custom, 1, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "custom"));
    g_assert_cmpint(parse_map("deviceEvent", type_keys, type, 1, &error), ==, EINVAL);
    g_assert_cmpint(parse_map("replyDialog", NULL, NULL, 0, &error), ==, EINVAL);
    g_assert_nonnull(strstr(error, "requestId"));
    g_assert_cmpint(parse_map("setOriginHeaders", NULL, NULL, 0, &error), ==, EINVAL);
    g_assert_cmpint(parse_map("setMagnification", NULL, NULL, 0, &error), ==, EINVAL);
    g_assert_cmpint(parse_map("imeCommit", NULL, NULL, 0, &error), ==, EINVAL);
    /* a header value that is no string after one that is: the copies so far are freed (ASan) */
    {
        struct ucfl_writer w;
        struct ucfl_call call;
        ucfl_writer_init(&w);
        ucfl_put_method_call(&w, "setOriginHeaders");
        ucfl_put_map(&w, 3);
        ucfl_put_string(&w, "viewId");
        ucfl_put_int(&w, 1);
        ucfl_put_string(&w, "origin");
        ucfl_put_string(&w, "https://example.com");
        ucfl_put_string(&w, "headers");
        ucfl_put_map(&w, 2);
        ucfl_put_string(&w, "A");
        ucfl_put_string(&w, "secret");
        ucfl_put_string(&w, "B");
        ucfl_put_int(&w, 1);
        g_assert_cmpint(ucfl_parse_call(w.data, w.size, false, &call, &error), ==, EINVAL);
        g_assert_nonnull(strstr(error, "headers"));
        ucfl_writer_free(&w);
    }
    /* listen and cancel ignore their arguments, whatever they are */
    {
        struct ucfl_writer w;
        struct ucfl_call call;
        ucfl_writer_init(&w);
        ucfl_put_method_call(&w, "listen");
        ucfl_put_list(&w, 1);
        ucfl_put_int(&w, 7);
        g_assert_cmpint(ucfl_parse_call(w.data, w.size, true, &call, &error), ==, 0);
        g_assert_cmpint(call.method, ==, UCFL_METHOD_LISTEN);
        ucfl_call_clear(&call);
        ucfl_writer_free(&w);
    }
    /* a list where the arguments map belongs */
    {
        struct ucfl_writer w;
        struct ucfl_call call;
        ucfl_writer_init(&w);
        ucfl_put_method_call(&w, "hello");
        ucfl_put_list(&w, 0);
        g_assert_cmpint(ucfl_parse_call(w.data, w.size, false, &call, &error), ==, EINVAL);
        ucfl_writer_free(&w);
    }
}

static void test_all_replies_and_events(void)
{
    static const char *const all[] = { "refreshRate",     "dragThreshold", "newSized",  "whenRendered",
                                       "pinchZoom",       "requests",      "originHeaders", "userAgent",
                                       "cookies",         "allowCertificate", "tlsErrorsPolicy", "colorScheme",
                                       "clearData",       "deviceApi" };
    struct ucfl_writer w;

#define CHECK(name, put)                                                                                               \
    do {                                                                                                               \
        ucfl_writer_init(&w);                                                                                          \
        put;                                                                                                           \
        assert_fixture(&w, name);                                                                                      \
    } while (0)

    CHECK("reply-hello-full.bin",
          ucfl_put_hello_reply(&w, &(struct ucfl_hello) { .available = true, .library = "9.9.9-fake", .abi = 1,
                                                          .features = all, .n_features = G_N_ELEMENTS(all) }));
    CHECK("reply-initialize.bin", ucfl_put_initialize_reply(&w, 16, 48.61));
    CHECK("reply-state.bin",
          ucfl_put_state_reply(&w, &(struct ucfl_view_state) { .uri = "https://example.com/", .title = "Example",
                                                               .can_go_back = true, .has_magnification = true,
                                                               .magnification = 1.0 }));
    CHECK("reply-true.bin", ucfl_put_bool_reply(&w, true));
    CHECK("reply-false.bin", ucfl_put_bool_reply(&w, false));
    CHECK("reply-rendered-done.bin", ucfl_put_string_reply(&w, ucfl_rendered_name(UC_WEBVIEW_RENDERED_DONE)));
    CHECK("reply-rendered-cancelled.bin",
          ucfl_put_string_reply(&w, ucfl_rendered_name(UC_WEBVIEW_RENDERED_CANCELLED)));
    CHECK("reply-error-no-view.bin", ucfl_put_error_envelope(&w, UCFL_ERROR_NO_VIEW, "unknown viewId"));
    CHECK("reply-error-no-request.bin",
          ucfl_put_error_envelope(&w, UCFL_ERROR_NO_REQUEST, "unknown or ended requestId"));
    CHECK("reply-error-bad-arguments.bin",
          ucfl_put_error_envelope(&w, UCFL_ERROR_BAD_ARGUMENTS, "viewId: a positive int is required"));
    CHECK("reply-error-unsupported.bin",
          ucfl_put_error_envelope(&w, UCFL_ERROR_UNSUPPORTED, "the library lacks the feature deviceApi"));
    CHECK("reply-error-refused.bin",
          ucfl_put_error_envelope(&w, UCFL_ERROR_REFUSED, "the library refused the device state"));

    CHECK("event-load-failed.bin",
          ucfl_put_event_load_failed(&w, 1, "https://nx.example/", UC_WEBVIEW_FAIL_HOST_NOT_FOUND,
                                     "g-resolver-error-quark", 1, 0,
                                     "Error resolving \xe2\x80\x9cnx.example\xe2\x80\x9d: Name or service not known"));
    CHECK("event-title.bin", ucfl_put_event_title(&w, 1, "Example"));
    CHECK("event-history.bin", ucfl_put_event_history(&w, 1, true, false));
    CHECK("event-ime-focus.bin",
          ucfl_put_event_ime_focus(&w, 1, true, UC_WEBVIEW_IME_HINT_INHIBIT_OSK | UC_WEBVIEW_IME_HINT_SPELLCHECK,
                                   UC_WEBVIEW_IME_PURPOSE_EMAIL, "a@b", 3));
    CHECK("event-ime-cursor-area.bin", ucfl_put_event_ime_cursor_area(&w, 1, 10, 20, 2, 18));
    CHECK("event-ime-surrounding.bin", ucfl_put_event_ime_surrounding(&w, 1, "hello", 5, 2));
    CHECK("event-cursor.bin", ucfl_put_event_cursor(&w, 1, "pointer"));
    CHECK("event-pinch.bin", ucfl_put_event_pinch(&w, 1, UC_WEBVIEW_PINCH_UPDATE, 1.5, 240.0, 400.0));
    CHECK("event-magnification.bin", ucfl_put_event_magnification(&w, 1, 2.0));
    CHECK("event-script-dialog.bin",
          ucfl_put_event_script_dialog(&w, 1, 1, UC_WEBVIEW_DIALOG_PROMPT, "https://example.com", "Your name?", "Bob"));
    CHECK("event-script-dialog-before-unload.bin",
          ucfl_put_event_script_dialog(&w, 1, 1, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, "https://example.com", NULL, NULL));
    CHECK("event-authenticate.bin",
          ucfl_put_event_authenticate(&w, 1, 1, "https://example.com", "Area", UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, 0));
    CHECK("event-request-cancelled.bin", ucfl_put_event_request_cancelled(&w, 1, 1, "page"));
    CHECK("event-close-requested.bin", ucfl_put_event_close_requested(&w, 1));
    CHECK("event-new-window.bin",
          ucfl_put_event_new_window(&w, 1, "https://example.org/", UC_WEBVIEW_NEW_WINDOW_LOADED));
    CHECK("event-response-refused.bin",
          ucfl_put_event_response_refused(&w, 1, "https://example.com/file.zip", "application/zip",
                                          UC_WEBVIEW_RESPONSE_DOWNLOAD));
#undef CHECK
}

static void test_more_names(void)
{
    g_assert_cmpstr(ucfl_load_failure_name(UC_WEBVIEW_FAIL_SCHEME_REFUSED), ==, "schemeRefused");
    g_assert_cmpstr(ucfl_load_failure_name(42), ==, "unknown");
    g_assert_cmpstr(ucfl_ime_purpose_name(UC_WEBVIEW_IME_PURPOSE_SEARCH), ==, "search");
    g_assert_cmpstr(ucfl_ime_purpose_name(12), ==, "unknown");
    g_assert_cmpstr(ucfl_ime_purpose_name(0xffffffffu), ==, "unknown");
    g_assert_cmpstr(ucfl_pinch_phase_name(UC_WEBVIEW_PINCH_CANCEL), ==, "cancel");
    g_assert_cmpstr(ucfl_dialog_type_name(UC_WEBVIEW_DIALOG_ALERT), ==, "alert");
    g_assert_null(ucfl_dialog_type_name(4));
    g_assert_null(ucfl_dialog_type_name(-1));
    g_assert_cmpstr(ucfl_auth_scheme_name(UC_WEBVIEW_AUTH_SCHEME_NTLM), ==, "ntlm");
    g_assert_cmpstr(ucfl_auth_scheme_name(UC_WEBVIEW_AUTH_SCHEME_UNKNOWN), ==, "unknown");
    g_assert_cmpstr(ucfl_response_refused_name(UC_WEBVIEW_RESPONSE_UNSUPPORTED), ==, "unsupported");
    g_assert_cmpstr(ucfl_rendered_name(UC_WEBVIEW_RENDERED_TIMEOUT), ==, "timeout");
}

static void test_log_origin(void)
{
    char out[64];

    ucfl_log_origin("https://user:pw@example.com:8443/a/b?token=x#frag", out, sizeof(out));
    g_assert_cmpstr(out, ==, "https://example.com:8443");
    ucfl_log_origin("http://example.com", out, sizeof(out));
    g_assert_cmpstr(out, ==, "http://example.com");
    ucfl_log_origin("http://a@b@example.com/?x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "http://example.com");
    ucfl_log_origin("https://[::1]:8080#x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "https://[::1]:8080");
    ucfl_log_origin("data:text/html,<p>secret</p>", out, sizeof(out));
    g_assert_cmpstr(out, ==, "data:");
    ucfl_log_origin("about:blank", out, sizeof(out));
    g_assert_cmpstr(out, ==, "about:");
    ucfl_log_origin("example.com/path", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    ucfl_log_origin(":x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    /* user info with / ? # or a backslash in it: nothing of it is logged */
    ucfl_log_origin("https://user:pa/ss@host/x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    ucfl_log_origin("https://user:pa?ss@host/x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    ucfl_log_origin("https://user:pa#ss@host/x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    ucfl_log_origin("https://user:pa\\ss@host/x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    ucfl_log_origin("https://user/ss@host", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    /* an authority that is no host[:port] */
    ucfl_log_origin("https://us er:x/", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    /* the price: an '@' in a path or query hides the origin too */
    ucfl_log_origin("https://example.com/?mail=a@b", out, sizeof(out));
    g_assert_cmpstr(out, ==, "(invalid)");
    ucfl_log_origin("file:///etc/x", out, sizeof(out));
    g_assert_cmpstr(out, ==, "file://");
    ucfl_log_origin(NULL, out, sizeof(out));
    g_assert_cmpstr(out, ==, "(none)");
    ucfl_log_origin("https://a-very-long-host-name.example.com/", out, 12);
    g_assert_cmpstr(out, ==, "https://a-v");
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
    g_test_add_func("/protocol/all-calls", test_all_calls);
    g_test_add_func("/protocol/invalid-enums", test_invalid_enums);
    g_test_add_func("/protocol/all-replies-and-events", test_all_replies_and_events);
    g_test_add_func("/protocol/more-names", test_more_names);
    g_test_add_func("/protocol/log-origin", test_log_origin);
    return g_test_run();
}
