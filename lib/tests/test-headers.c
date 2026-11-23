/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test for uc_headers.c (GLib only), the origin headers of library 0.8.0 that the library and its web
 * process extension share: run with `make check` (also in `make check-asan`).
 */
#include "uc_headers.h"

#include <string.h>

static void
assert_origin (const char *uri, const char *expected)
{
    char *origin = uc_headers_origin (uri);
    g_assert_cmpstr (origin, ==, expected);
    g_free (origin);
}

static void
test_origin (void)
{
    assert_origin ("https://example.com/a/b?c=d#e", "https://example.com");
    assert_origin ("https://example.com", "https://example.com");
    assert_origin ("https://example.com/", "https://example.com");
    assert_origin ("HTTPS://Example.COM:443/x", "https://example.com");
    assert_origin ("http://example.com:80/", "http://example.com");
    assert_origin ("http://example.com:8080/", "http://example.com:8080");
    assert_origin ("https://example.com:80/", "https://example.com:80");
    assert_origin ("http://192.168.1.10:8123/lovelace", "http://192.168.1.10:8123");
    assert_origin ("http://[::1]:8080/", "http://[::1]:8080");
    assert_origin ("http://[FE80::1]/", "http://[fe80::1]");
    assert_origin ("https://user:secret@example.com/", "https://example.com");
    assert_origin ("https://bücher.example/", "https://xn--bcher-kva.example");
    assert_origin ("ftp://example.com/", NULL);
    assert_origin ("file:///etc/passwd", NULL);
    assert_origin ("about:blank", NULL);
    assert_origin ("data:text/html,x", NULL);
    assert_origin ("example.com", NULL);
    assert_origin ("null", NULL);
    assert_origin ("", NULL);
    assert_origin (NULL, NULL);
}

static void
test_names_values (void)
{
    g_assert_true (uc_headers_name_valid ("Authorization"));
    g_assert_true (uc_headers_name_valid ("X-Api-Key"));
    g_assert_true (uc_headers_name_valid ("CF-Access-Client-Id"));
    g_assert_true (uc_headers_name_valid ("x_y.z~!#$%&'*+^`|"));
    g_assert_false (uc_headers_name_valid (""));
    g_assert_false (uc_headers_name_valid (NULL));
    g_assert_false (uc_headers_name_valid ("X Api"));
    g_assert_false (uc_headers_name_valid ("X-Api:"));
    g_assert_false (uc_headers_name_valid ("X-Api\r\nEvil"));
    g_assert_false (uc_headers_name_valid ("Host"));
    g_assert_false (uc_headers_name_valid ("cookie"));
    g_assert_false (uc_headers_name_valid ("Content-Length"));
    g_assert_false (uc_headers_name_valid ("Transfer-Encoding"));
    /* the Core-API spec's forbidden names and a few more (library 0.8.3) */
    {
        static const char *const reserved[] = {
            "Content-Type", "Connection", "Upgrade", "Origin", "Referer", "User-Agent", "Proxy-Authorization",
            "proxy-connection", "PROXY-X", "Sec-Fetch-Dest", "sec-ch-ua", "Sec-", "Proxy-", "Cookie2", "Expect",
        };
        for (gsize i = 0; i < G_N_ELEMENTS (reserved); i++) {
            if (uc_headers_name_valid (reserved[i]))
                g_error ("reserved header name %s accepted", reserved[i]);
        }
        g_assert_true (uc_headers_name_valid ("Secret"));         /* "Sec" without the dash */
        g_assert_true (uc_headers_name_valid ("Proxyless"));
        g_assert_true (uc_headers_name_valid ("X-Referer-Id"));
    }
    {
        char *long_name = g_strnfill (UC_HEADERS_MAX_NAME_BYTES + 1, 'a');
        g_assert_false (uc_headers_name_valid (long_name));
        long_name[UC_HEADERS_MAX_NAME_BYTES] = '\0';
        g_assert_true (uc_headers_name_valid (long_name));
        g_free (long_name);
    }

    g_assert_true (uc_headers_value_valid ("Bearer abc.def-ghi_jkl="));
    g_assert_true (uc_headers_value_valid (""));
    g_assert_true (uc_headers_value_valid ("a b\tc"));
    g_assert_false (uc_headers_value_valid (NULL));
    g_assert_false (uc_headers_value_valid ("a\r\nX-Evil: 1"));
    g_assert_false (uc_headers_value_valid ("a\nb"));
    g_assert_false (uc_headers_value_valid ("\x01"));
    g_assert_false (uc_headers_value_valid (" leading"));
    g_assert_false (uc_headers_value_valid ("trailing\t"));
    g_assert_false (uc_headers_value_valid ("Gr\xc3\xbc" "ezi"));
    {
        char *long_value = g_strnfill (UC_HEADERS_MAX_VALUE_BYTES + 1, 'v');
        g_assert_false (uc_headers_value_valid (long_value));
        long_value[UC_HEADERS_MAX_VALUE_BYTES] = '\0';
        g_assert_true (uc_headers_value_valid (long_value));
        g_free (long_value);
    }
}

static void
test_list (void)
{
    const char *names[] = { "Authorization", "X-Api-Key", "authorization" };
    const char *values[] = { "Bearer t", "k", "Bearer u" };
    const char *bad_values[] = { "Bearer t", "k\r\n" };
    const char *reason = NULL;

    g_assert_true (uc_headers_list_valid (names, values, 2, &reason));
    g_assert_null (reason);
    g_assert_true (uc_headers_list_valid (NULL, NULL, 0, &reason));
    g_assert_false (uc_headers_list_valid (names, values, 3, &reason));
    g_assert_nonnull (strstr (reason, "twice"));
    g_assert_false (uc_headers_list_valid (names, bad_values, 2, &reason));
    g_assert_nonnull (strstr (reason, "value"));
    g_assert_false (uc_headers_list_valid (NULL, values, 1, &reason));
    g_assert_false (uc_headers_list_valid (names, values, UC_HEADERS_MAX_COUNT + 1, &reason));
    {
        const char *host[] = { "Host" };
        g_assert_false (uc_headers_list_valid (host, values, 1, &reason));
        g_assert_nonnull (strstr (reason, "name"));
    }
}

static const char *const auth_names[] = { "Authorization", "X-Extra" };
static const char *const auth_values[] = { "Bearer token-a", "1" };

static UcHeadersTable *
sample_table (void)
{
    UcHeadersTable *table = uc_headers_table_new ();
    const char *names_b[] = { "X-Api-Key" };
    const char *values_b[] = { "key-b" };

    uc_headers_table_set (table, "http://a.local:8123", auth_names, auth_values, 2, 1);
    uc_headers_table_set (table, "https://b.example", names_b, values_b, 1, 2);
    return table;
}

static void
test_load_validation (void)
{
    /* defence in depth: what the extension takes from the library's message */
    GVariantBuilder origins, good, bad, other;
    GVariant *variant;
    UcHeadersTable *table = uc_headers_table_new ();
    guint32 generation = 0;

    g_variant_builder_init (&origins, G_VARIANT_TYPE ("a{sa(ss)}"));
    g_variant_builder_init (&good, G_VARIANT_TYPE ("a(ss)"));
    g_variant_builder_add (&good, "(ss)", "Authorization", "Bearer t");
    g_variant_builder_add (&origins, "{sa(ss)}", "https://good.example", &good);
    g_variant_builder_init (&bad, G_VARIANT_TYPE ("a(ss)"));
    g_variant_builder_add (&bad, "(ss)", "Host", "evil.example");
    g_variant_builder_add (&origins, "{sa(ss)}", "https://bad.example", &bad);
    g_variant_builder_init (&other, G_VARIANT_TYPE ("a(ss)"));
    g_variant_builder_add (&other, "(ss)", "X-Api-Key", "k\r\nX: y");
    g_variant_builder_add (&origins, "{sa(ss)}", "HTTPS://Other.Example:443", &other);
    variant = g_variant_ref_sink (g_variant_new ("(ua{sa(ss)})", 7, &origins));
    g_assert_true (uc_headers_table_load (table, variant, &generation));
    g_assert_cmpuint (generation, ==, 7);
    g_assert_cmpuint (uc_headers_table_size (table), ==, 1);
    g_assert_nonnull (uc_headers_table_lookup (table, "https://good.example"));
    g_assert_null (uc_headers_table_lookup (table, "https://bad.example"));
    g_variant_unref (variant);
    uc_headers_table_free (table);
}

static void
test_table (void)
{
    UcHeadersTable *table = sample_table ();
    const UcHeaderList *list = uc_headers_table_lookup (table, "http://a.local:8123");

    g_assert_cmpuint (uc_headers_table_size (table), ==, 2);
    g_assert_nonnull (list);
    g_assert_cmpuint (list->n, ==, 2);
    g_assert_cmpstr (list->names[0], ==, "Authorization");
    g_assert_cmpstr (list->values[0], ==, "Bearer token-a");
    g_assert_null (uc_headers_table_lookup (table, "http://a.local"));
    g_assert_null (uc_headers_table_lookup (table, NULL));
    /* n 0 removes */
    uc_headers_table_set (table, "https://b.example", NULL, NULL, 0, 3);
    g_assert_null (uc_headers_table_lookup (table, "https://b.example"));
    g_assert_cmpuint (uc_headers_table_size (table), ==, 1);
    uc_headers_table_free (table);
    uc_headers_table_free (NULL);
}

static void
test_merge (void)
{
    UcHeadersTable *view1 = uc_headers_table_new (), *view2 = uc_headers_table_new (), *merged = uc_headers_table_new ();
    const char *n1[] = { "X-One" }, *v1[] = { "1" }, *n2[] = { "X-Two" }, *v2[] = { "2" };

    uc_headers_table_set (view1, "https://same.example", n1, v1, 1, 5);
    uc_headers_table_set (view2, "https://same.example", n2, v2, 1, 7);
    uc_headers_table_set (view1, "https://only1.example", n1, v1, 1, 6);
    uc_headers_table_merge (merged, view1);
    uc_headers_table_merge (merged, view2);
    /* the later call wins, whatever the order of the merge */
    g_assert_cmpstr (uc_headers_table_lookup (merged, "https://same.example")->names[0], ==, "X-Two");
    uc_headers_table_clear (merged);
    uc_headers_table_merge (merged, view2);
    uc_headers_table_merge (merged, view1);
    g_assert_cmpstr (uc_headers_table_lookup (merged, "https://same.example")->names[0], ==, "X-Two");
    g_assert_cmpstr (uc_headers_table_lookup (merged, "https://only1.example")->names[0], ==, "X-One");
    /* view2 goes: view1's entry comes back */
    uc_headers_table_clear (merged);
    uc_headers_table_merge (merged, view1);
    g_assert_cmpstr (uc_headers_table_lookup (merged, "https://same.example")->names[0], ==, "X-One");
    uc_headers_table_free (view1);
    uc_headers_table_free (view2);
    uc_headers_table_free (merged);
}

static void
test_serialize (void)
{
    UcHeadersTable *table = sample_table (), *copy = uc_headers_table_new ();
    GVariant *variant = g_variant_ref_sink (uc_headers_table_serialize (table, 3));
    guint32 generation = 0;
    const UcHeaderList *list;

    g_assert_true (g_variant_is_of_type (variant, G_VARIANT_TYPE (UC_HEADERS_VARIANT_TYPE)));
    g_assert_true (uc_headers_table_load (copy, variant, &generation));
    g_assert_cmpuint (generation, ==, 3);
    g_assert_cmpuint (uc_headers_table_size (copy), ==, 2);
    list = uc_headers_table_lookup (copy, "https://b.example");
    g_assert_cmpuint (list->n, ==, 1);
    g_assert_cmpstr (list->values[0], ==, "key-b");
    /* the same or an older generation is ignored (a broadcast queued before newer initialization data) */
    uc_headers_table_set (table, "http://a.local:8123", NULL, NULL, 0, 9);
    {
        GVariant *older = g_variant_ref_sink (uc_headers_table_serialize (table, 2));
        GVariant *same = g_variant_ref_sink (uc_headers_table_serialize (table, 3));
        GVariant *newer = g_variant_ref_sink (uc_headers_table_serialize (table, 4));
        g_assert_false (uc_headers_table_load (copy, older, &generation));
        g_assert_false (uc_headers_table_load (copy, same, &generation));
        g_assert_cmpuint (uc_headers_table_size (copy), ==, 2);
        g_assert_true (uc_headers_table_load (copy, newer, &generation));
        g_assert_cmpuint (generation, ==, 4);
        g_assert_cmpuint (uc_headers_table_size (copy), ==, 1);
        g_variant_unref (older);
        g_variant_unref (same);
        g_variant_unref (newer);
    }
    /* wrong types and NULL change nothing */
    {
        GVariant *wrong = g_variant_ref_sink (g_variant_new_string ("x"));
        g_assert_false (uc_headers_table_load (copy, wrong, &generation));
        g_assert_false (uc_headers_table_load (copy, NULL, &generation));
        g_assert_cmpuint (uc_headers_table_size (copy), ==, 1);
        g_variant_unref (wrong);
    }
    /* an empty table is a valid table */
    {
        UcHeadersTable *empty = uc_headers_table_new ();
        GVariant *v = g_variant_ref_sink (uc_headers_table_serialize (empty, 5));
        g_assert_true (uc_headers_table_load (copy, v, &generation));
        g_assert_cmpuint (uc_headers_table_size (copy), ==, 0);
        g_variant_unref (v);
        uc_headers_table_free (empty);
    }
    g_variant_unref (variant);
    uc_headers_table_free (table);
    uc_headers_table_free (copy);
}

typedef struct {
    const char *what;
    const char *request, *page, *redirect_from, *origin_header;
    const char *strip;      /* first name of the stripped list, NULL: none */
    const char *add;        /* first name of the added list, NULL: none */
} DecideCase;

static void
test_decide (void)
{
    static const DecideCase cases[] = {
        { "main resource of the origin", "http://a.local:8123/", "http://a.local:8123/", NULL, NULL,
          NULL, "Authorization" },
        { "main resource, fragment differs", "http://a.local:8123/x", "http://a.local:8123/x#top", NULL, NULL,
          NULL, "Authorization" },
        { "subresource under the origin's page", "http://a.local:8123/app.js", "http://a.local:8123/", NULL, NULL,
          NULL, "Authorization" },
        { "fetch() of the page, same origin", "http://a.local:8123/api/states", "http://a.local:8123/lovelace", NULL,
          NULL, NULL, "Authorization" },
        { "same-origin POST (Origin header of the origin)", "http://a.local:8123/api", "http://a.local:8123/", NULL,
          "http://a.local:8123", NULL, "Authorization" },
        { "page of another site embeds the origin", "http://a.local:8123/img.png", "https://evil.example/", NULL,
          NULL, NULL, NULL },
        { "cross-origin fetch from the origin's page to another origin", "https://cdn.example/x.js",
          "http://a.local:8123/", NULL, NULL, NULL, NULL },
        { "another site posts a form to the origin (CSRF)", "http://a.local:8123/api/delete", "http://a.local:8123/api/delete",
          NULL, "https://evil.example", NULL, NULL },
        { "a frame of another site fetches from the origin", "http://a.local:8123/api", "http://a.local:8123/", NULL,
          "https://frame.example", NULL, NULL },
        { "opaque origin (sandboxed frame)", "http://a.local:8123/api", "http://a.local:8123/", NULL, "null",
          NULL, NULL },
        { "link of another site to the origin (GET, no Origin)", "http://a.local:8123/", "http://a.local:8123/",
          NULL, NULL, NULL, "Authorization" },
        { "a redirect of another site into the origin within one process (same-site, or a view's first navigation): "
          "no headers since 0.8.3; WPE swaps the process for a cross-site one, see the next case",
          "http://a.local:8123/login", "https://sso.example/cb", "https://sso.example/cb", NULL, NULL, NULL },
        { "main frame redirect, WebKit already has the new URI", "http://a.local:8123/login",
          "http://a.local:8123/login", "https://sso.example/cb", NULL, NULL, "Authorization" },
        { "cross-site: an image of another site's page with the page's own URL, redirected into the origin",
          "http://a.local:8123/api/action", "https://evil.example/cb", "https://evil.example/cb", NULL, NULL, NULL },
        { "cross-site: the same as a frame or a no-cors fetch, the page URI with a fragment",
          "http://a.local:8123/api/action", "https://evil.example/cb#x", "https://evil.example/cb", NULL, NULL, NULL },
        { "redirect within the origin", "http://a.local:8123/b", "http://a.local:8123/a", "http://a.local:8123/a",
          NULL, NULL, "Authorization" },
        { "main frame redirect away from the origin", "https://sso.example/login", "http://a.local:8123/",
          "http://a.local:8123/", NULL, "Authorization", NULL },
        { "subresource redirect away from the origin", "https://cdn.example/img", "http://a.local:8123/",
          "http://a.local:8123/img", NULL, "Authorization", NULL },
        { "redirect from one decorated origin to another", "https://b.example/x", "https://b.example/x",
          "http://a.local:8123/go", NULL, "Authorization", "X-Api-Key" },
        { "origin with another port is another origin", "http://a.local/", "http://a.local/", NULL, NULL,
          NULL, NULL },
        { "no page URI yet, first request of the process", "http://a.local:8123/app.js", NULL, NULL, NULL,
          NULL, NULL },
        { "not http", "data:text/plain,x", "http://a.local:8123/", NULL, NULL, NULL, NULL },
    };
    UcHeadersTable *table = sample_table ();

    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        const DecideCase *c = &cases[i];
        const UcHeaderList *strip, *add;

        uc_headers_decide (table, c->request, c->page, c->redirect_from, c->origin_header, &strip, &add);
        if (g_test_verbose ())
            g_printerr ("%s\n", c->what);
        g_assert_cmpstr (strip != NULL ? strip->names[0] : NULL, ==, c->strip);
        g_assert_cmpstr (add != NULL ? add->names[0] : NULL, ==, c->add);
    }
    /* an empty table and NULL decide nothing */
    {
        UcHeadersTable *empty = uc_headers_table_new ();
        const UcHeaderList *strip = (const UcHeaderList *) 1, *add = (const UcHeaderList *) 1;
        uc_headers_decide (empty, "http://a.local:8123/", "http://a.local:8123/", NULL, NULL, &strip, &add);
        g_assert_null (strip);
        g_assert_null (add);
        uc_headers_decide (NULL, "http://a.local:8123/", NULL, NULL, NULL, &strip, &add);
        g_assert_null (strip);
        g_assert_null (add);
        uc_headers_table_free (empty);
    }
    uc_headers_table_free (table);
}

/* ---- uc_headers_apply(): the request's headers through a fake of SoupMessageHeaders --------------------------- */

static const char *
fake_get (gpointer data, const char *name)
{
    char *key = g_ascii_strdown (name, -1);
    const char *value = g_hash_table_lookup (data, key);
    g_free (key);
    return value;
}

static void
fake_remove (gpointer data, const char *name)
{
    char *key = g_ascii_strdown (name, -1);
    g_hash_table_remove (data, key);
    g_free (key);
}

static void
fake_set (gpointer data, const char *name, const char *value)
{
    g_hash_table_replace (data, g_ascii_strdown (name, -1), g_strdup (value));
}

static const UcHeaderOps fake_ops = { fake_get, fake_remove, fake_set };

/* "name: value" pairs, NULL terminated */
static GHashTable *
headers_of (const char *const *pairs)
{
    GHashTable *h = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    for (gsize i = 0; pairs != NULL && pairs[i] != NULL; i++) {
        const char *colon = strchr (pairs[i], ':');
        char *name = g_strndup (pairs[i], (gsize) (colon - pairs[i]));
        fake_set (h, name, colon + 2);
        g_free (name);
    }
    return h;
}

/* the headers after the call as sorted "name: value" lines joined with "|" (names lower case) */
static char *
headers_text (GHashTable *h)
{
    GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
    GHashTableIter iter;
    gpointer key, value;
    char *text;

    g_hash_table_iter_init (&iter, h);
    while (g_hash_table_iter_next (&iter, &key, &value))
        g_ptr_array_add (lines, g_strdup_printf ("%s: %s", (char *) key, (char *) value));
    g_ptr_array_sort_values (lines, (GCompareFunc) g_strcmp0);
    g_ptr_array_add (lines, NULL);
    text = g_strjoinv ("|", (char **) lines->pdata);
    g_ptr_array_unref (lines);
    return text;
}

typedef struct {
    const char *what;
    const char *request, *page, *redirect_from;
    const char *before[6];
    const char *after;      /* headers_text() */
    guint removed;          /* by the value rule */
} ApplyCase;

static void
test_apply (void)
{
    static const ApplyCase cases[] = {
        { "WPE process swap: the new process loads the redirected request as a first request (the 0.8.0 leak)",
          "https://other.example/landed", "https://other.example/landed", NULL,
          { "X-Extra: 1", "Accept: */*", NULL }, "accept: */*", 1 },
        { "the same with WebKit's Authorization copy (WebKit removes it itself, but a copy is removed anyway)",
          "https://other.example/landed", "https://other.example/landed", NULL,
          { "Authorization: Bearer token-a", "X-Extra: 1", NULL }, "", 2 },
        { "a page of another site sends its own value under a configured name: kept",
          "https://other.example/api", "https://other.example/", NULL,
          { "X-Extra: 2", "Authorization: Bearer own", NULL }, "authorization: Bearer own|x-extra: 2", 0 },
        { "the origin's page: the headers are set",
          "http://a.local:8123/api", "http://a.local:8123/", NULL,
          { "Accept: */*", NULL }, "accept: */*|authorization: Bearer token-a|x-extra: 1", 0 },
        { "the origin's page: a page header of the same name is replaced",
          "http://a.local:8123/api", "http://a.local:8123/", NULL,
          { "X-Extra: page", NULL }, "authorization: Bearer token-a|x-extra: 1", 0 },
        { "a redirect away in the same process: removed by name (also a page's value)",
          "https://other.example/x", "http://a.local:8123/", "http://a.local:8123/go",
          { "Authorization: Bearer token-a", "X-Extra: page", "X-Page: p", NULL }, "x-page: p", 0 },
        { "a chain into a second decorated origin, the first one's copy carried along",
          "https://b.example/end", "https://b.example/end", NULL,
          { "X-Extra: 1", NULL }, "x-api-key: key-b", 1 },
        { "a subresource redirect hop between two other sites, a copy carried from the first hop",
          "https://third.example/img", "http://a.local:8123/", "https://other.example/img",
          { "X-Extra: 1", NULL }, "", 1 },
        { "a form another site posts to the origin with a copy: no headers, the copy goes",
          "http://a.local:8123/post", "http://a.local:8123/post", NULL,
          { "Origin: https://evil.example", "X-Extra: 1", NULL }, "origin: https://evil.example", 1 },
        { "not http (the extension gets no headers for it): no headers set, a copy goes anyway",
          "data:text/plain,x", "http://a.local:8123/", NULL,
          { "X-Extra: 1", NULL }, "", 1 },
    };
    UcHeadersTable *table = sample_table ();

    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        const ApplyCase *c = &cases[i];
        GHashTable *h = headers_of (c->before);
        guint removed;
        char *text;

        if (g_test_verbose ())
            g_printerr ("%s\n", c->what);
        removed = uc_headers_apply (table, c->request, c->page, c->redirect_from, &fake_ops, h);
        text = headers_text (h);
        g_assert_cmpstr (text, ==, c->after);
        g_assert_cmpuint (removed, ==, c->removed);
        g_free (text);
        g_hash_table_unref (h);
    }
    uc_headers_table_free (table);
}

static void
test_apply_shared_value (void)
{
    /* two origins configured with the same header and value: the entitled request keeps it, another loses it */
    UcHeadersTable *table = uc_headers_table_new ();
    const char *names[] = { "X-Shared" }, *values[] = { "s" };
    GHashTable *h;
    char *text;

    uc_headers_table_set (table, "https://one.example", names, values, 1, 1);
    uc_headers_table_set (table, "https://two.example", names, values, 1, 2);
    h = headers_of ((const char *const[]) { "X-Shared: s", NULL });
    uc_headers_apply (table, "https://two.example/a", "https://two.example/", NULL, &fake_ops, h);
    text = headers_text (h);
    g_assert_cmpstr (text, ==, "x-shared: s");
    g_free (text);
    g_hash_table_unref (h);
    h = headers_of ((const char *const[]) { "X-Shared: s", NULL });
    g_assert_cmpuint (uc_headers_apply (table, "https://three.example/a", "https://three.example/", NULL, &fake_ops, h),
                      ==, 1);
    g_assert_cmpuint (g_hash_table_size (h), ==, 0);
    g_hash_table_unref (h);
    /* an empty table and NULL change nothing */
    uc_headers_table_clear (table);
    h = headers_of ((const char *const[]) { "X-Shared: s", NULL });
    g_assert_cmpuint (uc_headers_apply (table, "https://three.example/a", NULL, NULL, &fake_ops, h), ==, 0);
    g_assert_cmpuint (uc_headers_apply (NULL, "https://three.example/a", NULL, NULL, &fake_ops, h), ==, 0);
    g_assert_cmpuint (g_hash_table_size (h), ==, 1);
    g_hash_table_unref (h);
    uc_headers_table_free (table);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/headers/origin", test_origin);
    g_test_add_func ("/headers/names-values", test_names_values);
    g_test_add_func ("/headers/list", test_list);
    g_test_add_func ("/headers/table", test_table);
    g_test_add_func ("/headers/load-validation", test_load_validation);
    g_test_add_func ("/headers/merge", test_merge);
    g_test_add_func ("/headers/serialize", test_serialize);
    g_test_add_func ("/headers/decide", test_decide);
    g_test_add_func ("/headers/apply", test_apply);
    g_test_add_func ("/headers/apply-shared-value", test_apply_shared_value);
    return g_test_run ();
}
