/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host test of core/ucfl_host.c against the fake libraries of tests/fake-ucwebview.c (GLib's test framework only):
 * a test adapter collects replies, events, log lines and surface calls; the test plays the library's part through
 * the fake's control functions and the callbacks the host registered. Replies and events are compared byte for byte
 * with the golden files of tests/fixtures/, the library calls with the fake's call log.
 *
 * Covered: availability (missing, refused, failing library), initialisation, the view lifecycle, every method and its
 * library call, every event, the page's requests and their answers (also by default and before a navigation), the
 * replies that wait for the library, missing optional functions, dispose with pending requests and replies, teardown,
 * and that no secret reaches the log.
 */
#include "ucfl_host.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>

#include <glib.h>

#include "uc_webview.h"

/* ---- the fake's control functions ------------------------------------------------------------------------------- */

struct fake {
    void *handle;
    const char *(*log)(void);
    void (*log_clear)(void);
    uc_webview *(*view)(int n);
    int (*live_views)(void);
    const uc_webview_callbacks *(*callbacks)(uc_webview *wv, void **user, size_t *size);
    void (*fail)(const char *name);
    int (*rendered)(uc_webview *wv, int status);
    int (*done)(int status);
    void (*add_request)(uc_webview *wv, uint64_t id);
    void (*set_page)(uc_webview *wv, const char *uri, const char *title, int back, int forward);
    void (*reset)(void);
};

static void fake_open(struct fake *fake, const char *path)
{
    memset(fake, 0, sizeof(*fake));
    fake->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    g_assert_nonnull(fake->handle);
#define SYM(field) *(void **) &fake->field = dlsym(fake->handle, "fake_" #field)
    SYM(log);
    SYM(log_clear);
    SYM(view);
    SYM(live_views);
    SYM(callbacks);
    SYM(fail);
    SYM(rendered);
    SYM(done);
    SYM(add_request);
    SYM(set_page);
    SYM(reset);
#undef SYM
    g_assert_nonnull(fake->reset);
    fake->reset();
}

/* ---- the test adapter ------------------------------------------------------------------------------------------- */

struct reply {
    intptr_t token;
    GBytes *data; /* empty: the not-implemented reply */
};

struct surface {
    int64_t view_id;
    uc_webview *wv;
    bool hold;
    int frames;
    bool freed;
};

struct harness {
    struct ucfl_host *host;
    struct fake fake;
    const struct ucfl_lib *lib;
    GPtrArray *events;  /* GBytes */
    GPtrArray *replies; /* struct reply */
    GString *log;
    intptr_t next_token;
    struct surface surfaces[8];
    int n_surfaces;
    bool fail_surface;
    int after_calls;
};

static void send_event(void *ctx, const uint8_t *data, size_t size)
{
    struct harness *h = ctx;
    g_ptr_array_add(h->events, g_bytes_new(data, size));
}

static void respond(void *ctx, void *token, const uint8_t *data, size_t size)
{
    struct harness *h = ctx;
    struct reply *reply = g_new0(struct reply, 1);

    reply->token = (intptr_t) token;
    reply->data = g_bytes_new(data, size);
    g_ptr_array_add(h->replies, reply);
}

static void log_line(void *ctx, const char *line)
{
    struct harness *h = ctx;
    g_string_append_printf(h->log, "%s\n", line);
}

static int surface_new(void *ctx, int64_t view_id, uc_webview *wv, double refresh_rate, int64_t *texture_id,
                       void **surface)
{
    struct harness *h = ctx;
    struct surface *s;

    (void) refresh_rate;
    if (h->fail_surface || h->n_surfaces == G_N_ELEMENTS(h->surfaces))
        return -1;
    s = &h->surfaces[h->n_surfaces++];
    s->view_id = view_id;
    s->wv = wv;
    *texture_id = view_id + 1;
    *surface = s;
    return 0;
}

static void surface_frame(void *ctx, void *surface, const uc_webview_frame *frame)
{
    struct harness *h = ctx;
    struct surface *s = surface;

    s->frames++;
    /* shown and replaced at once */
    h->lib->frame_rendered(s->wv, frame->handle);
    h->lib->frame_release(s->wv, frame->handle);
}

static void surface_hold(void *ctx, void *surface, bool hold)
{
    ((struct surface *) surface)->hold = hold;
}

static void surface_free(void *ctx, void *surface)
{
    ((struct surface *) surface)->freed = true;
}

static void after_call(void *ctx)
{
    ((struct harness *) ctx)->after_calls++;
}

static size_t adapter_stats(void *ctx, struct ucfl_stat *out, size_t max)
{
    if (max < 1)
        return 0;
    out[0] = (struct ucfl_stat) { "adapterCounter", 7 };
    return 1;
}

static const struct ucfl_host_ops ops = {
    .send_event = send_event,
    .respond = respond,
    .log = log_line,
    .surface_new = surface_new,
    .surface_frame = surface_frame,
    .surface_hold = surface_hold,
    .surface_free = surface_free,
    .after_call = after_call,
    .stats = adapter_stats,
};

static void free_reply(gpointer p)
{
    struct reply *reply = p;
    g_bytes_unref(reply->data);
    g_free(reply);
}

static void harness_init_with(struct harness *h, const char *library, const char *unavailable)
{
    struct ucfl_host_config config = { .library_path = library, .egl_display = NULL, .unavailable = unavailable };

    memset(h, 0, sizeof(*h));
    h->events = g_ptr_array_new_with_free_func((GDestroyNotify) g_bytes_unref);
    h->replies = g_ptr_array_new_with_free_func(free_reply);
    h->log = g_string_new(NULL);
    if (library && g_file_test(library, G_FILE_TEST_EXISTS))
        fake_open(&h->fake, library);
    h->host = ucfl_host_new(&ops, h, &config);
    g_assert_nonnull(h->host);
}

static void harness_init(struct harness *h)
{
    harness_init_with(h, FAKE_DIR "/fake-ok.so", NULL);
}

static void harness_free(struct harness *h)
{
    if (h->host)
        ucfl_host_free(h->host);
    g_ptr_array_unref(h->events);
    g_ptr_array_unref(h->replies);
    g_string_free(h->log, TRUE);
    if (h->fake.handle)
        dlclose(h->fake.handle);
}

/* ---- messages --------------------------------------------------------------------------------------------------- */

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

/* sends a fixture's call on the control channel; returns its token */
static intptr_t send_fixture(struct harness *h, const char *name)
{
    g_autoptr(GBytes) call = fixture(name);
    gsize size;
    const uint8_t *data = g_bytes_get_data(call, &size);
    intptr_t token = ++h->next_token;

    ucfl_host_handle_control(h->host, data, size, (void *) token);
    if (!h->lib)
        h->lib = ucfl_host_lib(h->host);
    return token;
}

static intptr_t send_writer(struct harness *h, struct ucfl_writer *w)
{
    intptr_t token = ++h->next_token;

    g_assert_false(w->failed);
    ucfl_host_handle_control(h->host, w->data, w->size, (void *) token);
    ucfl_writer_free(w);
    if (!h->lib)
        h->lib = ucfl_host_lib(h->host);
    return token;
}

static void start_listening(struct harness *h)
{
    g_autoptr(GBytes) call = fixture("call-listen.bin");
    gsize size;
    const uint8_t *data = g_bytes_get_data(call, &size);

    ucfl_host_handle_events(h->host, data, size, (void *) (intptr_t) 999);
    g_assert_cmpuint(h->replies->len, >=, 1);
    g_ptr_array_remove_index(h->replies, h->replies->len - 1);
}

/* the reply of token, removed from the list; NULL without one */
static GBytes *take_reply(struct harness *h, intptr_t token)
{
    for (guint i = 0; i < h->replies->len; i++) {
        struct reply *reply = g_ptr_array_index(h->replies, i);
        if (reply->token == token) {
            GBytes *data = g_bytes_ref(reply->data);
            g_ptr_array_remove_index(h->replies, i);
            return data;
        }
    }
    return NULL;
}

static void assert_bytes(GBytes *actual, const char *name)
{
    g_autoptr(GBytes) expected = fixture(name);
    gsize a_size, e_size;
    const void *a = g_bytes_get_data(actual, &a_size), *e = g_bytes_get_data(expected, &e_size);

    g_assert_cmpmem(a, a_size, e, e_size);
}

static void assert_reply(struct harness *h, intptr_t token, const char *name)
{
    g_autoptr(GBytes) reply = take_reply(h, token);

    g_assert_nonnull(reply);
    assert_bytes(reply, name);
}

/* the reply of token is an error with code */
static void assert_error(struct harness *h, intptr_t token, const char *code)
{
    g_autoptr(GBytes) reply = take_reply(h, token);
    struct ucfl_value value;
    bool is_error;
    gsize size;
    const uint8_t *data;

    g_assert_nonnull(reply);
    data = g_bytes_get_data(reply, &size);
    g_assert_cmpint(ucfl_decode_envelope(data, size, &is_error, &value), ==, 0);
    g_assert_true(is_error);
    g_assert_true(ucfl_value_is_string(&value, code));
    ucfl_value_clear(&value);
}

/* decodes a reply that is no error */
static void decode_reply(GBytes *reply, struct ucfl_value *value)
{
    gsize size;
    const uint8_t *data;
    bool is_error;

    g_assert_nonnull(reply);
    data = g_bytes_get_data(reply, &size);
    g_assert_cmpint(ucfl_decode_envelope(data, size, &is_error, value), ==, 0);
    g_assert_false(is_error);
}

static void assert_null_reply(struct harness *h, intptr_t token)
{
    assert_reply(h, token, "reply-null.bin");
}

/* the oldest event, which must be the fixture's */
static void assert_event(struct harness *h, const char *name)
{
    g_assert_cmpuint(h->events->len, >=, 1);
    assert_bytes(g_ptr_array_index(h->events, 0), name);
    g_ptr_array_remove_index(h->events, 0);
}

/* the oldest event decoded */
static void take_event(struct harness *h, struct ucfl_value *value)
{
    bool is_error;
    gsize size;
    const uint8_t *data;

    g_assert_cmpuint(h->events->len, >=, 1);
    data = g_bytes_get_data(g_ptr_array_index(h->events, 0), &size);
    g_assert_cmpint(ucfl_decode_envelope(data, size, &is_error, value), ==, 0);
    g_assert_false(is_error);
    /* the strings point into the bytes, which the array keeps until the next removal: callers look at them first */
}

static void drop_event(struct harness *h)
{
    g_ptr_array_remove_index(h->events, 0);
}

static void assert_log_has(struct harness *h, const char *line)
{
    if (!strstr(h->fake.log(), line))
        g_error("the fake's log lacks \"%s\":\n%s", line, h->fake.log());
}

/* view n's callbacks as the library got them */
static const uc_webview_callbacks *cb_of(struct harness *h, int n, uc_webview **wv, void **user)
{
    size_t size;

    *wv = h->fake.view(n);
    g_assert_nonnull(*wv);
    return h->fake.callbacks(*wv, user, &size);
}

static void create_view(struct harness *h)
{
    intptr_t token = send_fixture(h, "call-create.bin");
    g_autoptr(GBytes) reply = take_reply(h, token);
    g_assert_nonnull(reply);
}

/* ---- tests ------------------------------------------------------------------------------------------------------ */

static void test_unavailable(void)
{
    struct harness h;
    intptr_t token;
    struct ucfl_value value;
    g_autoptr(GBytes) reply = NULL;

    harness_init_with(&h, NULL, "no library");
    assert_reply(&h, send_fixture(&h, "call-hello.bin"), "reply-hello-unavailable.bin");
    assert_error(&h, send_fixture(&h, "call-create.bin"), UCFL_ERROR_UNAVAILABLE);
    assert_error(&h, send_fixture(&h, "call-set-color-scheme.bin"), UCFL_ERROR_UNAVAILABLE);
    g_assert_null(ucfl_host_lib(h.host));
    harness_free(&h);

    /* a library that is not there: dlopen's reason */
    harness_init_with(&h, FAKE_DIR "/no-such-libucwebview.so.1", NULL);
    token = send_fixture(&h, "call-hello.bin");
    reply = take_reply(&h, token);
    decode_reply(reply, &value);
    g_assert_cmpint(ucfl_map_get(&value, "reason")->type, ==, UCFL_STRING);
    g_assert_cmpint(ucfl_map_get(&value, "available")->type, ==, UCFL_FALSE);
    ucfl_value_clear(&value);
    harness_free(&h);

    /* another API version */
    harness_init_with(&h, FAKE_DIR "/fake-abi2.so", NULL);
    assert_error(&h, send_fixture(&h, "call-initialize.bin"), UCFL_ERROR_UNAVAILABLE);
    g_assert_nonnull(strstr(h.log->str, "API version 2"));
    harness_free(&h);
}

static void test_hello_full(void)
{
    struct harness h;

    harness_init(&h);
    assert_reply(&h, send_fixture(&h, "call-hello.bin"), "reply-hello-full.bin");
    /* loading is not initialising */
    g_assert_null(strstr(h.fake.log(), "global_init"));
    harness_free(&h);
}

static void test_initialize(void)
{
    struct harness h;

    harness_init(&h);
    assert_reply(&h, send_fixture(&h, "call-initialize.bin"), "reply-initialize.bin");
    /* the refresh rate before the initialisation (WebKit takes it once) */
    g_assert_cmpstr(h.fake.log(), ==, "global_set_refresh_rate 48.61\nglobal_init (null) (null) (null)\n");
    h.fake.log_clear();
    assert_reply(&h, send_fixture(&h, "call-initialize.bin"), "reply-initialize.bin");
    g_assert_cmpstr(h.fake.log(), ==, ""); /* once */
    g_assert_cmpint(h.after_calls, ==, 2);
    harness_free(&h);

    /* a failing initialisation leaves the web view unavailable, as remote-ui */
    harness_init(&h);
    h.fake.fail("global_init");
    assert_error(&h, send_fixture(&h, "call-create.bin"), UCFL_ERROR_UNAVAILABLE);
    assert_error(&h, send_fixture(&h, "call-create.bin"), UCFL_ERROR_UNAVAILABLE);
    {
        g_autoptr(GBytes) r = take_reply(&h, send_fixture(&h, "call-hello.bin"));
        struct ucfl_value value;
        decode_reply(r, &value);
        g_assert_cmpint(ucfl_map_get(&value, "available")->type, ==, UCFL_FALSE);
        g_assert_true(ucfl_value_is_string(ucfl_map_get(&value, "reason"), "uc_webview_global_init failed"));
        ucfl_value_clear(&value);
    }
    harness_free(&h);
}

static void test_lifecycle(void)
{
    struct harness h;
    uc_webview *wv;
    void *user;
    size_t size;
    const uc_webview_callbacks *cb;

    harness_init(&h);
    start_listening(&h);
    assert_reply(&h, send_fixture(&h, "call-create.bin"), "reply-create.bin");
    assert_log_has(&h, "global_init");
    assert_log_has(&h, "new 1 480x800\nset_scale 1 1\nload_uri 1 https://example.com/\n");
    wv = h.fake.view(0);
    cb = h.fake.callbacks(wv, &user, &size);
    /* new_sized with the whole struct: pinch, requests and the failure detail reach the library */
    g_assert_cmpuint(size, ==, sizeof(uc_webview_callbacks));
    g_assert_nonnull(cb->frame_ready);
    g_assert_nonnull(cb->ime_surrounding);
    g_assert_nonnull(cb->pinch);
    g_assert_nonnull(cb->script_dialog);
    g_assert_nonnull(cb->response_refused);
    g_assert_nonnull(cb->load_failed_detail);
    g_assert_cmpint(h.n_surfaces, ==, 1);
    g_assert_cmpint(h.surfaces[0].view_id, ==, 1);

    /* a frame goes to the surface */
    cb->frame_ready(wv, &(uc_webview_frame) { .width = 480, .height = 800, .handle = (void *) 0x10 }, user);
    g_assert_cmpint(h.surfaces[0].frames, ==, 1);
    assert_log_has(&h, "frame_release 1 0x10");

    assert_null_reply(&h, send_fixture(&h, "call-dispose.bin"));
    assert_log_has(&h, "free 1");
    g_assert_true(h.surfaces[0].freed);
    g_assert_cmpint(h.fake.live_views(), ==, 0);
    /* the id is never reused */
    assert_reply(&h, send_fixture(&h, "call-dispose.bin"), "reply-error-no-view.bin");
    create_view(&h);
    g_assert_cmpint(h.surfaces[1].view_id, ==, 2);
    assert_error(&h, send_fixture(&h, "call-set-mapped.bin"), UCFL_ERROR_NO_VIEW);
    g_assert_cmpuint(h.events->len, ==, 0);
    harness_free(&h);

    /* pinch zoom and requests switched off by create */
    harness_init(&h);
    assert_reply(&h, send_fixture(&h, "call-create-full.bin"), "reply-create.bin");
    assert_log_has(&h, "global_set_refresh_rate 48.61\n");
    assert_log_has(&h, "set_scale 1 1.5\n");
    cb = cb_of(&h, 0, &wv, &user);
    g_assert_null(cb->pinch);
    g_assert_nonnull(cb->script_dialog);
    harness_free(&h);

    /* a surface that cannot be made: the view goes again */
    harness_init(&h);
    h.fail_surface = true;
    assert_error(&h, send_fixture(&h, "call-create.bin"), UCFL_ERROR_FAILED);
    assert_log_has(&h, "free 1");
    g_assert_cmpint(h.fake.live_views(), ==, 0);
    /* and a library that cannot create one */
    h.fake.fail("new");
    assert_error(&h, send_fixture(&h, "call-create.bin"), UCFL_ERROR_FAILED);
    harness_free(&h);
}

/* every method of a view reaches the library as such */
static void test_view_methods(void)
{
    static const struct {
        const char *fixture;
        const char *log;
    } calls[] = {
        { "call-resize.bin", "resize 1 800x480" },
        { "call-set-mapped.bin", "set_mapped 1 0" },
        { "call-set-focused.bin", "set_focused 1 1" },
        { "call-set-scale.bin", "set_scale 1 1.5" },
        { "call-set-background-color.bin", "set_background_rgba 1 0.200 0.400 0.600 1.000" },
        { "call-load-uri.bin", "load_uri 1 https://user:pw@example.com/a?token=t0k#f" },
        { "call-load-html.bin", "load_html 1 <p>Hi</p> https://example.com/" },
        { "call-reload.bin", "reload 1" },
        { "call-stop.bin", "stop 1" },
        { "call-go-back.bin", "go_back 1" },
        { "call-go-forward.bin", "go_forward 1" },
        { "call-run-javascript.bin", "run_javascript 1 window.scrollTo(0, 0)" },
        { "call-set-zoom.bin", "set_zoom 1 1.25" },
        { "call-set-magnification.bin", "set_magnification 1 2 240 400" },
        { "call-touch.bin", "touch 1 7 1 12.5 700.25 123456" },
        { "call-pointer-button.bin", "pointer_button 1 1 1 10 20 2 1000" },
        { "call-pointer-move.bin", "pointer_move 1 11.5 21.5 256 1010" },
        { "call-scroll.bin", "scroll 1 0 2 240 400 0 1020" },
        { "call-key-full.bin", "key 1 0xff09 23 1 2 1030" },
        { "call-ime-commit.bin", "ime_commit 1 Gr\xc3\xbc" "ezi \xf0\x9f\x91\x8b" },
        { "call-ime-preedit.bin", "ime_preedit 1 ab 2" },
        { "call-ime-delete-surrounding.bin", "ime_delete_surrounding 1 -1 1" },
        { "call-set-origin-headers.bin", "set_origin_headers 1 https://example.com 2\n  Authorization: Bearer t0ken\n"
                                         "  X-Api-Key: k3y" },
        { "call-set-user-agent.bin", "set_user_agent 1 3 MyAgent/1.0" },
        { "call-set-device-api.bin", "set_device_api 1 1" },
        { "call-set-device-state.bin", "set_device_state 1 {\"screen\":\"dim\",\"powerSource\":\"battery\",\"level\":42}" },
        { "call-device-event.bin", "device_event 1 resume 3600000" },
    };
    struct harness h;

    harness_init(&h);
    create_view(&h);
    for (size_t i = 0; i < G_N_ELEMENTS(calls); i++) {
        h.fake.log_clear();
        assert_null_reply(&h, send_fixture(&h, calls[i].fixture));
        assert_log_has(&h, calls[i].log);
    }
    /* the hold goes to the surface, not the library */
    h.fake.log_clear();
    assert_null_reply(&h, send_fixture(&h, "call-set-frame-hold.bin"));
    g_assert_true(h.surfaces[0].hold);
    g_assert_cmpstr(h.fake.log(), ==, "");
    /* a view the call does not name */
    assert_reply(&h, send_fixture(&h, "call-touch-int64.bin"), "reply-error-no-view.bin");
    harness_free(&h);
}

static void test_global_methods(void)
{
    struct harness h;

    harness_init(&h);
    assert_null_reply(&h, send_fixture(&h, "call-set-color-scheme.bin"));
    assert_log_has(&h, "global_set_color_scheme 1");
    assert_null_reply(&h, send_fixture(&h, "call-set-tls-errors-policy.bin"));
    assert_log_has(&h, "global_set_tls_errors_policy 1");
    assert_null_reply(&h, send_fixture(&h, "call-allow-certificate.bin"));
    assert_log_has(&h, "global_allow_certificate 192.168.1.20 0123456789abcdef");
    assert_null_reply(&h, send_fixture(&h, "call-set-refresh-rate.bin"));
    assert_log_has(&h, "global_set_refresh_rate 50");
    /* none of them initialises the library (they may come first, the library keeps the values) */
    g_assert_null(strstr(h.fake.log(), "global_init"));
    harness_free(&h);
}

static void test_get_state(void)
{
    struct harness h;
    uc_webview *wv;

    harness_init(&h);
    create_view(&h);
    wv = h.fake.view(0);
    h.fake.set_page(wv, "https://example.com/", "Example", 1, 0);
    assert_reply(&h, send_fixture(&h, "call-get-state.bin"), "reply-state.bin");
    harness_free(&h);
}

/* the library's callbacks become the events of the fixtures */
static void test_events(void)
{
    struct harness h;
    uc_webview *wv, *wv2;
    void *user, *user2;
    const uc_webview_callbacks *cb, *cb2;

    harness_init(&h);
    start_listening(&h);
    create_view(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);
    cb2 = cb_of(&h, 1, &wv2, &user2);

    cb->load_changed(wv, UC_WEBVIEW_LOAD_FINISHED, "about:blank", user);
    assert_event(&h, "event-load-changed.bin");
    cb->load_failed_detail(wv, "https://nx.example/", UC_WEBVIEW_FAIL_HOST_NOT_FOUND, "g-resolver-error-quark", 1, 0,
                           "Error resolving \xe2\x80\x9cnx.example\xe2\x80\x9d: Name or service not known", user);
    assert_event(&h, "event-load-failed.bin");
    cb->progress_changed(wv, 0.5, user);
    assert_event(&h, "event-progress.bin");
    /* a title change looks at the history too (a same-document navigation brings no load event) */
    h.fake.set_page(wv, "https://example.com/", "Example", 1, 0);
    cb->title_changed(wv, "Example", user);
    assert_event(&h, "event-title.bin");
    assert_event(&h, "event-history.bin");
    cb->title_changed(wv, "Example", user); /* the history did not change: no second event */
    assert_event(&h, "event-title.bin");
    g_assert_cmpuint(h.events->len, ==, 0);
    cb2->web_process_terminated(wv2, UC_WEBVIEW_TERMINATED_CRASHED, user2);
    assert_event(&h, "event-terminated.bin");
    cb->ime_focus(wv, 1, UC_WEBVIEW_IME_HINT_INHIBIT_OSK | UC_WEBVIEW_IME_HINT_SPELLCHECK, UC_WEBVIEW_IME_PURPOSE_EMAIL,
                  "a@b", 3, user);
    assert_event(&h, "event-ime-focus.bin");
    cb->ime_cursor_area(wv, 10, 20, 2, 18, user);
    assert_event(&h, "event-ime-cursor-area.bin");
    cb->ime_surrounding(wv, "hello", 5, 2, user);
    assert_event(&h, "event-ime-surrounding.bin");
    cb->cursor_changed(wv, "pointer", user);
    assert_event(&h, "event-cursor.bin");
    cb->pinch(wv, UC_WEBVIEW_PINCH_UPDATE, 1.5, 240.0, 400.0, user);
    assert_event(&h, "event-pinch.bin");
    cb->magnification_changed(wv, 2.0, user);
    assert_event(&h, "event-magnification.bin");
    cb->close_requested(wv, user);
    assert_event(&h, "event-close-requested.bin");
    cb->new_window(wv, "https://example.org/", UC_WEBVIEW_NEW_WINDOW_LOADED, user);
    assert_event(&h, "event-new-window.bin");
    cb->response_refused(wv, "https://example.com/file.zip", "application/zip", UC_WEBVIEW_RESPONSE_DOWNLOAD, user);
    assert_event(&h, "event-response-refused.bin");
    g_assert_cmpuint(h.events->len, ==, 0);

    /* nobody listens: dropped and counted */
    {
        g_autoptr(GBytes) cancel = fixture("call-cancel.bin");
        gsize size;
        const uint8_t *data = g_bytes_get_data(cancel, &size);
        g_autoptr(GBytes) stats = NULL;
        struct ucfl_value value;
        int64_t n;

        ucfl_host_handle_events(h.host, data, size, (void *) (intptr_t) 998);
        assert_null_reply(&h, 998);
        cb->progress_changed(wv, 0.7, user);
        g_assert_cmpuint(h.events->len, ==, 0);
        stats = take_reply(&h, send_fixture(&h, "call-stats.bin"));
        decode_reply(stats, &value);
        g_assert_true(ucfl_value_get_int(ucfl_map_get(&value, "views"), &n));
        g_assert_cmpint(n, ==, 2);
        g_assert_true(ucfl_value_get_int(ucfl_map_get(&value, "eventsDropped"), &n));
        g_assert_cmpint(n, ==, 1);
        g_assert_true(ucfl_value_get_int(ucfl_map_get(&value, "adapterCounter"), &n));
        g_assert_cmpint(n, ==, 7);
        ucfl_value_clear(&value);
    }
    harness_free(&h);
}

static void test_script_dialogs(void)
{
    struct harness h;
    uc_webview *wv;
    void *user;
    const uc_webview_callbacks *cb;
    struct ucfl_writer w;
    intptr_t token;
    struct ucfl_value value;

    harness_init(&h);
    start_listening(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);

    h.fake.add_request(wv, 77);
    cb->script_dialog(wv, 77, UC_WEBVIEW_DIALOG_PROMPT, "https://example.com", "Your name?", "Bob", user);
    assert_event(&h, "event-script-dialog.bin");
    /* a login's answer to a dialog */
    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, "replyAuth");
    ucfl_put_map(&w, 2);
    ucfl_put_string(&w, "viewId");
    ucfl_put_int(&w, 1);
    ucfl_put_string(&w, "requestId");
    ucfl_put_int(&w, 1);
    assert_error(&h, send_writer(&h, &w), UCFL_ERROR_NO_REQUEST);
    h.fake.log_clear();
    assert_null_reply(&h, send_fixture(&h, "call-reply-dialog.bin"));
    g_assert_cmpstr(h.fake.log(), ==, "request_reply 1 77 1 Alice\n");
    /* answered: once only */
    assert_reply(&h, send_fixture(&h, "call-reply-dialog.bin"), "reply-error-no-request.bin");

    /* the library ends a request: requestCancelled, and the id is gone */
    h.fake.add_request(wv, 78);
    cb->script_dialog(wv, 78, UC_WEBVIEW_DIALOG_ALERT, "https://example.com", "Hi", NULL, user);
    take_event(&h, &value);
    g_assert_true(ucfl_value_is_string(ucfl_map_get(&value, "dialogType"), "alert"));
    g_assert_true(ucfl_value_get_int(ucfl_map_get(&value, "requestId"), &(int64_t) { 0 }));
    ucfl_value_clear(&value);
    drop_event(&h);
    cb->request_cancelled(wv, 78, user);
    take_event(&h, &value);
    g_assert_true(ucfl_value_is_string(ucfl_map_get(&value, "type"), "requestCancelled"));
    g_assert_true(ucfl_value_is_string(ucfl_map_get(&value, "reason"), "page"));
    ucfl_value_clear(&value);
    drop_event(&h);
    cb->request_cancelled(wv, 78, user); /* twice: ignored */
    g_assert_cmpuint(h.events->len, ==, 0);

    /* a type of a newer library: "no" at once, no event */
    h.fake.log_clear();
    cb->script_dialog(wv, 79, 9, NULL, "?", NULL, user);
    g_assert_cmpstr(h.fake.log(), ==, "request_reply 1 79 0 (null)\n");
    g_assert_cmpuint(h.events->len, ==, 0);

    /* the library refuses the answer (an id it ended): still pending here, until request_cancelled */
    cb->script_dialog(wv, 80, UC_WEBVIEW_DIALOG_CONFIRM, NULL, "Sure?", NULL, user); /* not in the fake's list */
    drop_event(&h);
    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, "replyDialog");
    ucfl_put_map(&w, 3);
    ucfl_put_string(&w, "viewId");
    ucfl_put_int(&w, 1);
    ucfl_put_string(&w, "requestId");
    ucfl_put_int(&w, 3); /* 1 prompt, 2 alert, none for the unknown type */
    ucfl_put_string(&w, "accept");
    ucfl_put_bool(&w, true);
    token = send_writer(&h, &w);
    assert_error(&h, token, UCFL_ERROR_REFUSED);
    cb->request_cancelled(wv, 80, user);
    take_event(&h, &value);
    g_assert_true(ucfl_value_is_string(ucfl_map_get(&value, "type"), "requestCancelled"));
    ucfl_value_clear(&value);
    drop_event(&h);
    harness_free(&h);
}

/* the leave-page question: answered by the host's navigation calls, as remote-ui's item does */
static void test_leave_page(void)
{
    struct harness h;
    uc_webview *wv;
    void *user;
    const uc_webview_callbacks *cb;
    struct ucfl_value value;

    harness_init(&h);
    start_listening(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);

    h.fake.add_request(wv, 90);
    cb->script_dialog(wv, 90, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, "https://example.com", NULL, NULL, user);
    assert_event(&h, "event-script-dialog-before-unload.bin");
    h.fake.log_clear();
    assert_null_reply(&h, send_fixture(&h, "call-load-uri.bin"));
    /* leave, before the navigation reaches the library */
    g_assert_true(g_str_has_prefix(h.fake.log(), "request_reply 1 90 1 (null)\nload_uri 1 "));
    take_event(&h, &value);
    g_assert_true(ucfl_value_is_string(ucfl_map_get(&value, "reason"), "navigation"));
    ucfl_value_clear(&value);
    drop_event(&h);

    /* stop stays */
    h.fake.add_request(wv, 91);
    cb->script_dialog(wv, 91, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, "https://example.com", NULL, NULL, user);
    drop_event(&h);
    h.fake.log_clear();
    assert_null_reply(&h, send_fixture(&h, "call-stop.bin"));
    g_assert_cmpstr(h.fake.log(), ==, "request_reply 1 91 0 (null)\nstop 1\n");
    drop_event(&h);

    /* other dialogs are the library's business */
    h.fake.add_request(wv, 92);
    cb->script_dialog(wv, 92, UC_WEBVIEW_DIALOG_ALERT, NULL, "x", NULL, user);
    drop_event(&h);
    h.fake.log_clear();
    assert_null_reply(&h, send_fixture(&h, "call-reload.bin"));
    g_assert_cmpstr(h.fake.log(), ==, "reload 1\n");
    harness_free(&h);
}

static void test_auth(void)
{
    struct harness h;
    uc_webview *wv;
    void *user;
    const uc_webview_callbacks *cb;

    harness_init(&h);
    start_listening(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);

    h.fake.add_request(wv, 50);
    cb->authenticate(wv, 50, "https://example.com", "Area", UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, 0, user);
    assert_event(&h, "event-authenticate.bin");
    h.fake.add_request(wv, 51);
    cb->authenticate(wv, 51, "https://example.com", "Area", UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST,
                     UC_WEBVIEW_AUTH_RETRY | UC_WEBVIEW_AUTH_PROXY, user);
    {
        struct ucfl_value value;
        bool retry = false, proxy = false;
        take_event(&h, &value);
        g_assert_true(ucfl_value_get_bool(ucfl_map_get(&value, "retry"), &retry) && retry);
        g_assert_true(ucfl_value_get_bool(ucfl_map_get(&value, "proxy"), &proxy) && proxy);
        g_assert_true(ucfl_value_is_string(ucfl_map_get(&value, "scheme"), "digest"));
        ucfl_value_clear(&value);
        drop_event(&h);
    }
    h.fake.log_clear();
    assert_null_reply(&h, send_fixture(&h, "call-reply-auth.bin")); /* requestId 2: the digest login */
    g_assert_cmpstr(h.fake.log(), ==, "request_auth 1 51 alice p4ssw0rd\n");
    /* a dialog's answer to a login */
    {
        struct ucfl_writer w;
        ucfl_writer_init(&w);
        ucfl_put_method_call(&w, "replyDialog");
        ucfl_put_map(&w, 3);
        ucfl_put_string(&w, "viewId");
        ucfl_put_int(&w, 1);
        ucfl_put_string(&w, "requestId");
        ucfl_put_int(&w, 1);
        ucfl_put_string(&w, "accept");
        ucfl_put_bool(&w, false);
        assert_error(&h, send_writer(&h, &w), UCFL_ERROR_NO_REQUEST);
    }
    /* without credentials: user null */
    {
        struct ucfl_writer w;
        ucfl_writer_init(&w);
        ucfl_put_method_call(&w, "replyAuth");
        ucfl_put_map(&w, 3);
        ucfl_put_string(&w, "viewId");
        ucfl_put_int(&w, 1);
        ucfl_put_string(&w, "requestId");
        ucfl_put_int(&w, 1);
        ucfl_put_string(&w, "user");
        ucfl_put_null(&w);
        h.fake.log_clear();
        assert_null_reply(&h, send_writer(&h, &w));
        g_assert_cmpstr(h.fake.log(), ==, "request_auth 1 50 (null) (null)\n");
    }

    /* a scheme that takes no user name: without credentials at once, no event */
    h.fake.log_clear();
    cb->authenticate(wv, 52, "https://example.com", NULL, UC_WEBVIEW_AUTH_SCHEME_NEGOTIATE, 0, user);
    cb->authenticate(wv, 53, "https://example.com", NULL, UC_WEBVIEW_AUTH_SCHEME_SERVER_TRUST, 0, user);
    cb->authenticate(wv, 54, "https://example.com", NULL, 77, 0, user);
    g_assert_cmpstr(h.fake.log(), ==,
                    "request_auth 1 52 (null) (null)\nrequest_auth 1 53 (null) (null)\nrequest_auth 1 54 (null) (null)\n");
    g_assert_cmpuint(h.events->len, ==, 0);
    harness_free(&h);
}

/* requests while nobody listens get the library's defaults at once */
static void test_requests_unheard(void)
{
    struct harness h;
    uc_webview *wv;
    void *user;
    const uc_webview_callbacks *cb;

    harness_init(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);
    h.fake.log_clear();
    cb->script_dialog(wv, 1, UC_WEBVIEW_DIALOG_CONFIRM, NULL, "Sure?", NULL, user);
    cb->script_dialog(wv, 2, UC_WEBVIEW_DIALOG_BEFORE_UNLOAD, NULL, NULL, NULL, user);
    cb->authenticate(wv, 3, "https://example.com", "Area", UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, 0, user);
    g_assert_cmpstr(h.fake.log(), ==,
                    "request_reply 1 1 0 (null)\nrequest_reply 1 2 1 (null)\nrequest_auth 1 3 (null) (null)\n");
    harness_free(&h);
}

/* the replies that wait for the library */
static void test_pending_replies(void)
{
    struct harness h;
    intptr_t token, token2, dispose;
    uc_webview *wv;

    harness_init(&h);
    create_view(&h);
    wv = h.fake.view(0);

    token = send_fixture(&h, "call-when-rendered.bin");
    g_assert_null(take_reply(&h, token)); /* not yet */
    g_assert_cmpint(h.fake.rendered(wv, UC_WEBVIEW_RENDERED_DONE), ==, 0);
    assert_reply(&h, token, "reply-rendered-done.bin");

    token = send_fixture(&h, "call-set-cookies.bin");
    g_assert_null(take_reply(&h, token));
    assert_log_has(&h, "global_set_cookies https://example.com/ 2\n  session=s3cr3t-cookie domain example.com path / "
                       "secure 1 http_only 1 expires 1900000000\n  lang=en domain (null) path (null) secure 0 "
                       "http_only 0 expires -1\n");
    token2 = send_fixture(&h, "call-clear-data.bin");
    assert_log_has(&h, "global_clear_data 0xffffffff");
    g_assert_cmpint(h.fake.done(UC_WEBVIEW_DONE_OK), ==, 0);
    assert_reply(&h, token, "reply-true.bin");
    g_assert_cmpint(h.fake.done(UC_WEBVIEW_DONE_FAILED), ==, 0);
    assert_reply(&h, token2, "reply-false.bin");

    /* refused by the library: at once */
    h.fake.fail("global_set_cookies");
    assert_error(&h, send_fixture(&h, "call-set-cookies.bin"), UCFL_ERROR_REFUSED);
    h.fake.fail("when_rendered");
    assert_error(&h, send_fixture(&h, "call-when-rendered.bin"), UCFL_ERROR_REFUSED);
    h.fake.fail(NULL);

    /* dispose ends a waiting whenRendered with "cancelled" before its own reply */
    token = send_fixture(&h, "call-when-rendered.bin");
    dispose = send_fixture(&h, "call-dispose.bin");
    g_assert_cmpuint(h.replies->len, ==, 2);
    g_assert_cmpint(((struct reply *) g_ptr_array_index(h.replies, 0))->token, ==, token);
    assert_reply(&h, token, "reply-rendered-cancelled.bin");
    assert_null_reply(&h, dispose);
    harness_free(&h);
}

/* a library without the optional functions: the features' methods are "unsupported", the rest works */
static void test_old_library(void)
{
    static const char *const gated[] = {
        "call-set-refresh-rate.bin", "call-set-color-scheme.bin", "call-set-tls-errors-policy.bin",
        "call-allow-certificate.bin", "call-set-cookies.bin", "call-clear-data.bin", "call-set-magnification.bin",
        "call-when-rendered.bin", "call-reply-dialog.bin", "call-reply-auth.bin", "call-set-origin-headers.bin",
        "call-set-user-agent.bin", "call-set-device-api.bin", "call-set-device-state.bin", "call-device-event.bin",
    };
    struct harness h;
    uc_webview *wv;
    void *user;
    size_t size;
    const uc_webview_callbacks *cb;
    struct ucfl_value value;
    g_autoptr(GBytes) reply = NULL;

    harness_init_with(&h, FAKE_DIR "/fake-old.so", NULL);
    reply = take_reply(&h, send_fixture(&h, "call-hello.bin"));
    decode_reply(reply, &value);
    g_assert_cmpuint(ucfl_map_get(&value, "features")->list.count, ==, 0);
    g_assert_cmpint(ucfl_map_get(&value, "library")->type, ==, UCFL_NULL);
    ucfl_value_clear(&value);

    create_view(&h);
    wv = h.fake.view(0);
    cb = h.fake.callbacks(wv, &user, &size);
    /* uc_webview_new(): the nine callbacks of API version 1 only */
    g_assert_cmpuint(size, ==, offsetof(uc_webview_callbacks, pinch));
    g_assert_nonnull(cb->ime_surrounding);
    for (size_t i = 0; i < G_N_ELEMENTS(gated); i++)
        assert_error(&h, send_fixture(&h, gated[i]), UCFL_ERROR_UNSUPPORTED);
    {
        g_autoptr(GBytes) r = take_reply(&h, send_fixture(&h, "call-set-device-api.bin"));
        assert_bytes(r, "reply-error-unsupported.bin");
    }
    /* initialize without the refresh rate and drag threshold functions: WPE's 8 px, no rate */
    {
        g_autoptr(GBytes) r = take_reply(&h, send_fixture(&h, "call-initialize.bin"));
        int64_t px;
        decode_reply(r, &value);
        g_assert_true(ucfl_value_get_int(ucfl_map_get(&value, "dragThreshold"), &px));
        g_assert_cmpint(px, ==, 8);
        g_assert_cmpint(ucfl_map_get(&value, "refreshRate")->type, ==, UCFL_NULL);
        ucfl_value_clear(&value);
    }
    /* getState without get_magnification */
    {
        g_autoptr(GBytes) r = take_reply(&h, send_fixture(&h, "call-get-state.bin"));
        decode_reply(r, &value);
        g_assert_cmpint(ucfl_map_get(&value, "magnification")->type, ==, UCFL_NULL);
        ucfl_value_clear(&value);
    }
    assert_null_reply(&h, send_fixture(&h, "call-touch.bin"));
    harness_free(&h);

    /* one function of the device API missing: the whole feature is */
    harness_init_with(&h, FAKE_DIR "/fake-no-device-state.so", NULL);
    create_view(&h);
    assert_error(&h, send_fixture(&h, "call-set-device-api.bin"), UCFL_ERROR_UNSUPPORTED);
    assert_null_reply(&h, send_fixture(&h, "call-set-user-agent.bin"));
    harness_free(&h);
}

static void test_refused(void)
{
    struct harness h;
    struct ucfl_writer w;

    harness_init(&h);
    create_view(&h);
    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, "setDeviceState");
    ucfl_put_map(&w, 2);
    ucfl_put_string(&w, "viewId");
    ucfl_put_int(&w, 1);
    ucfl_put_string(&w, "json");
    ucfl_put_string(&w, "[]");
    assert_reply(&h, send_writer(&h, &w), "reply-error-refused.bin");
    h.fake.fail("set_magnification");
    assert_error(&h, send_fixture(&h, "call-set-magnification.bin"), UCFL_ERROR_REFUSED);
    harness_free(&h);
}

static void test_bad_messages(void)
{
    struct harness h;
    struct ucfl_writer w;
    static const uint8_t garbage[] = { 0xff, 0x01 };
    intptr_t token;
    g_autoptr(GBytes) reply = NULL;

    harness_init(&h);
    token = ++h.next_token;
    ucfl_host_handle_control(h.host, garbage, sizeof(garbage), (void *) token);
    assert_error(&h, token, UCFL_ERROR_BAD_MESSAGE);
    /* an unknown method: the empty reply (MissingPluginException) */
    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, "fly");
    ucfl_put_null(&w);
    token = send_writer(&h, &w);
    reply = take_reply(&h, token);
    g_assert_cmpuint(g_bytes_get_size(reply), ==, 0);
    /* a view method without its view */
    ucfl_writer_init(&w);
    ucfl_put_method_call(&w, "dispose");
    ucfl_put_null(&w);
    assert_reply(&h, send_writer(&h, &w), "reply-error-bad-arguments.bin");
    /* the event channel knows listen and cancel only */
    token = ++h.next_token;
    ucfl_host_handle_events(h.host, garbage, sizeof(garbage), (void *) token);
    assert_error(&h, token, UCFL_ERROR_BAD_MESSAGE);
    harness_free(&h);
}

/* dispose with pending requests and replies: requests end without events, the ids are gone */
static void test_dispose_pending(void)
{
    struct harness h;
    uc_webview *wv;
    void *user;
    const uc_webview_callbacks *cb;

    harness_init(&h);
    start_listening(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);
    h.fake.add_request(wv, 60);
    cb->script_dialog(wv, 60, UC_WEBVIEW_DIALOG_CONFIRM, NULL, "Sure?", NULL, user);
    h.fake.add_request(wv, 61);
    cb->authenticate(wv, 61, "https://example.com", "Area", UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, 0, user);
    g_ptr_array_set_size(h.events, 0);
    assert_null_reply(&h, send_fixture(&h, "call-dispose.bin"));
    g_assert_cmpuint(h.events->len, ==, 0); /* as uc_webview_free(): no callback, no event */
    assert_reply(&h, send_fixture(&h, "call-reply-dialog.bin"), "reply-error-no-view.bin");
    harness_free(&h);
}

/* teardown with views, requests and waiting replies: nothing is answered after the host is gone, late library
 * callbacks are harmless (ASan) */
static void test_teardown(void)
{
    struct harness h;
    uc_webview *wv;
    void *user;
    const uc_webview_callbacks *cb;
    struct fake fake;
    guint replies;

    harness_init(&h);
    start_listening(&h);
    create_view(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);
    h.fake.add_request(wv, 70);
    cb->script_dialog(wv, 70, UC_WEBVIEW_DIALOG_ALERT, NULL, "x", NULL, user);
    send_fixture(&h, "call-when-rendered.bin");
    send_fixture(&h, "call-set-cookies.bin");
    replies = h.replies->len;

    fake = h.fake;
    ucfl_host_free(h.host);
    h.host = NULL;
    g_assert_cmpuint(h.replies->len, ==, replies); /* the whenRendered cancelled inside free is not answered */
    g_assert_true(h.surfaces[0].freed && h.surfaces[1].freed);
    g_assert_cmpint(fake.live_views(), ==, 0);
    g_assert_cmpint(fake.done(UC_WEBVIEW_DONE_OK), ==, 0); /* late: frees its op only */
    g_assert_cmpuint(h.replies->len, ==, replies);
    harness_free(&h);
}

/* log lines carry origins, never secrets */
static void test_no_secrets_logged(void)
{
    static const char *const secrets[] = { "s3cr3t-cookie", "t0ken", "k3y", "p4ssw0rd", "token=t0k", "user:pw",
                                           "/a?", "Alice", "Your name", "Gr\xc3\xbc" "ezi", "<p>Hi" };
    struct harness h;
    uc_webview *wv;
    void *user;
    const uc_webview_callbacks *cb;

    harness_init(&h);
    start_listening(&h);
    create_view(&h);
    cb = cb_of(&h, 0, &wv, &user);
    send_fixture(&h, "call-set-cookies.bin");
    h.fake.done(UC_WEBVIEW_DONE_FAILED);
    send_fixture(&h, "call-set-origin-headers.bin");
    send_fixture(&h, "call-load-uri.bin");
    send_fixture(&h, "call-load-html.bin");
    send_fixture(&h, "call-ime-commit.bin");
    h.fake.add_request(wv, 1);
    cb->script_dialog(wv, 1, UC_WEBVIEW_DIALOG_PROMPT, "https://example.com", "Your name?", "Bob", user);
    h.fake.add_request(wv, 2);
    cb->authenticate(wv, 2, "https://example.com", "Area", UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC, 0, user);
    send_fixture(&h, "call-reply-dialog.bin");
    send_fixture(&h, "call-reply-auth.bin");
    cb->load_changed(wv, UC_WEBVIEW_LOAD_FAILED, "https://user:pw@example.com/a?token=t0k", user);
    g_assert_nonnull(strstr(h.log->str, "https://example.com"));
    for (size_t i = 0; i < G_N_ELEMENTS(secrets); i++) {
        if (strstr(h.log->str, secrets[i]))
            g_error("the log has \"%s\":\n%s", secrets[i], h.log->str);
    }
    harness_free(&h);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/host/unavailable", test_unavailable);
    g_test_add_func("/host/hello-full", test_hello_full);
    g_test_add_func("/host/initialize", test_initialize);
    g_test_add_func("/host/lifecycle", test_lifecycle);
    g_test_add_func("/host/view-methods", test_view_methods);
    g_test_add_func("/host/global-methods", test_global_methods);
    g_test_add_func("/host/get-state", test_get_state);
    g_test_add_func("/host/events", test_events);
    g_test_add_func("/host/script-dialogs", test_script_dialogs);
    g_test_add_func("/host/leave-page", test_leave_page);
    g_test_add_func("/host/auth", test_auth);
    g_test_add_func("/host/requests-unheard", test_requests_unheard);
    g_test_add_func("/host/pending-replies", test_pending_replies);
    g_test_add_func("/host/old-library", test_old_library);
    g_test_add_func("/host/refused", test_refused);
    g_test_add_func("/host/bad-messages", test_bad_messages);
    g_test_add_func("/host/dispose-pending", test_dispose_pending);
    g_test_add_func("/host/teardown", test_teardown);
    g_test_add_func("/host/no-secrets-logged", test_no_secrets_logged);
    return g_test_run();
}
