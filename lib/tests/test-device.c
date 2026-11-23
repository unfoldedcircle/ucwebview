/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Device state for web pages (library 0.9.0), the part without WebKit (uc_device.c): the state JSON (valid, partial,
 * invalid members, refused input), the start script with the state, the changes the library sends to its web process
 * extension and the JSON the extension makes of them, the event names, and that the extension's compiled-in shim is
 * the file's bytes. With WebKitGTK on the host also the hidden page throttling features in WebKit's own feature list
 * (GModule, no build dependency).
 */
#include "uc_device.h"
#include "uc_ext_protocol.h"

#include <gmodule.h>
#include <locale.h>
#include <string.h>

/* device-host-shim.js as the extension has it (uc_ext_device.c) */
#include "uc_device_shim.h"

static const char full_json[] =
    "{\"screen\":\"dim\",\"powerSource\":\"battery\",\"powerProfile\":\"power-saver\",\"level\":42}";

static void
test_defaults (void)
{
    UcDeviceState s;
    char *text;

    uc_device_state_init (&s);
    g_assert_cmpint (s.screen, ==, UC_DEVICE_SCREEN_ON);
    g_assert_cmpint (s.source, ==, UC_DEVICE_SOURCE_AC);
    g_assert_cmpint (s.profile, ==, UC_DEVICE_PROFILE_BALANCED);
    g_assert_cmpint (s.level, ==, 100);
    text = uc_device_state_describe (&s);
    g_assert_cmpstr (text, ==, "screen on, ac, balanced, 100 %");
    g_free (text);
}

static void
test_full_state (void)
{
    UcDeviceState s;
    GString *ignored = g_string_new (NULL);
    const char *error = NULL;
    char *text;

    uc_device_state_init (&s);
    g_assert_true (uc_device_state_merge_json (&s, full_json, ignored, &error));
    g_assert_null (error);
    g_assert_cmpstr (ignored->str, ==, "");
    g_assert_cmpint (s.screen, ==, UC_DEVICE_SCREEN_DIM);
    g_assert_cmpint (s.source, ==, UC_DEVICE_SOURCE_BATTERY);
    g_assert_cmpint (s.profile, ==, UC_DEVICE_PROFILE_POWER_SAVER);
    g_assert_cmpint (s.level, ==, 42);
    text = uc_device_state_describe (&s);
    g_assert_cmpstr (text, ==, "screen dim, battery, power-saver, 42 %");
    g_free (text);
    g_string_free (ignored, TRUE);
}

static void
test_partial_and_unknown (void)
{
    UcDeviceState s, before;
    GString *ignored = g_string_new (NULL);

    uc_device_state_init (&s);
    g_assert_true (uc_device_state_merge_json (&s, full_json, NULL, NULL));
    before = s;
    /* empty object, unknown members (a newer host), whitespace: nothing changes, nothing is reported */
    g_assert_true (uc_device_state_merge_json (&s, " \t\r\n{ } \n", ignored, NULL));
    g_assert_true (uc_device_state_merge_json (&s, "{\"thermal\":\"critical\",\"x\":[1,{\"y\":null}],\"z\":null,"
                                               "\"battery\":{\"charging\":true}}", ignored, NULL));
    g_assert_true (uc_device_state_equal (&s, &before));
    g_assert_cmpstr (ignored->str, ==, "");
    /* one member at a time */
    g_assert_true (uc_device_state_merge_json (&s, "{\"screen\":\"off\"}", ignored, NULL));
    g_assert_cmpint (s.screen, ==, UC_DEVICE_SCREEN_OFF);
    g_assert_cmpint (s.source, ==, UC_DEVICE_SOURCE_BATTERY);
    g_assert_true (uc_device_state_merge_json (&s, "{\"level\":100}", ignored, NULL));
    g_assert_cmpint (s.level, ==, 100);
    g_assert_true (uc_device_state_merge_json (&s, "{\"powerProfile\":\"performance\",\"powerSource\":\"ac\"}", ignored,
                                               NULL));
    g_assert_cmpint (s.profile, ==, UC_DEVICE_PROFILE_PERFORMANCE);
    g_assert_cmpint (s.source, ==, UC_DEVICE_SOURCE_AC);
    /* an integer in every JSON form; -0 is 0 */
    g_assert_true (uc_device_state_merge_json (&s, "{\"level\":4.2e1}", NULL, NULL));
    g_assert_cmpint (s.level, ==, 42);
    g_assert_true (uc_device_state_merge_json (&s, "{\"level\":7.0}", NULL, NULL));
    g_assert_cmpint (s.level, ==, 7);
    g_assert_true (uc_device_state_merge_json (&s, "{\"level\":-0}", NULL, NULL));
    g_assert_cmpint (s.level, ==, 0);
    g_assert_true (uc_device_state_merge_json (&s, "{\"level\":1E2}", NULL, NULL));
    g_assert_cmpint (s.level, ==, 100);
    /* escapes in names and values */
    g_assert_true (uc_device_state_merge_json (&s, "{\"scr\\u0065en\":\"d\\u0069m\"}", NULL, NULL));
    g_assert_cmpint (s.screen, ==, UC_DEVICE_SCREEN_DIM);
    g_assert_true (uc_device_state_merge_json (&s, "{\"note\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t \\ud83d\\ude00 \xc3\xa4\"}",
                                               ignored, NULL));
    g_assert_cmpstr (ignored->str, ==, "");
    g_string_free (ignored, TRUE);
}

typedef struct {
    const char *json;
    const char *ignored;
} IgnoredCase;

static void
test_invalid_members (void)
{
    static const IgnoredCase cases[] = {
        { "{\"screen\":\"ON\"}", "screen" },
        { "{\"screen\":1}", "screen" },
        { "{\"screen\":null}", "screen" },
        { "{\"screen\":[\"on\"]}", "screen" },
        { "{\"powerSource\":\"solar\"}", "powerSource" },
        { "{\"powerSource\":true}", "powerSource" },
        { "{\"powerProfile\":\"powersave\"}", "powerProfile" },
        { "{\"powerProfile\":\"low-power\"}", "powerProfile" },
        { "{\"level\":101}", "level" },
        { "{\"level\":-1}", "level" },
        { "{\"level\":42.5}", "level" },
        { "{\"level\":0.42}", "level" },
        { "{\"level\":\"42\"}", "level" },
        { "{\"level\":null}", "level" },
        { "{\"level\":{\"value\":42}}", "level" },
        { "{\"screen\":\"bright\",\"powerSource\":\"ac\",\"powerProfile\":1,\"level\":1e3}",
          "screen, powerProfile, level" },
        /* a string that would break out of the script's literal is not one of the names */
        { "{\"screen\":\"\\\"});alert(1);//\"}", "screen" },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        UcDeviceState s, before;
        GString *ignored = g_string_new (NULL);

        uc_device_state_init (&s);
        g_assert_true (uc_device_state_merge_json (&s, full_json, NULL, NULL));
        before = s;
        g_test_message ("%s", cases[i].json);
        g_assert_true (uc_device_state_merge_json (&s, cases[i].json, ignored, NULL));
        g_assert_cmpstr (ignored->str, ==, cases[i].ignored);
        if (strstr (cases[i].json, "\"powerSource\":\"ac\"") != NULL)
            g_assert_cmpint (s.source, ==, UC_DEVICE_SOURCE_AC);    /* the valid member of the mixed case */
        else
            g_assert_true (uc_device_state_equal (&s, &before));
        g_string_free (ignored, TRUE);
    }
}

static void
test_refused (void)
{
    static const char *const cases[] = {
        "", " ", "[]", "[{\"screen\":\"on\"}]", "\"on\"", "1", "true", "null",
        "{", "}", "{\"screen\"}", "{\"screen\":}", "{\"screen\":\"on\",}", "{,}", "{\"screen\" \"on\"}",
        "{'screen':'on'}", "{screen:\"on\"}", "{\"screen\":\"on\"} x", "{\"screen\":\"on\"}{}",
        "{\"screen\":\"on\",\"screen\":\"off\"}",                       /* a name twice */
        "{\"a\":\"\x01\"}", "{\"a\":\"line\nbreak\"}",                    /* control characters */
        "{\"a\":\"\\x41\"}", "{\"a\":\"\\u12\"}", "{\"a\":\"\\ud800\"}", "{\"a\":\"\\udc00\"}",
        "{\"a\":\"\\ud800\\u0041\"}", "{\"a\":\"\\u0000\"}", "{\"a\":\"open}",
        "{\"a\":01}", "{\"a\":1.}", "{\"a\":.5}", "{\"a\":-}", "{\"a\":1e}", "{\"a\":+1}", "{\"a\":1e999}",
        "{\"a\":-1e999}", "{\"a\":NaN}", "{\"a\":Infinity}", "{\"a\":tru}", "{\"a\":nul}", "{\"a\":[1,]}",
        "{\"a\":[1 2]}",
        "{\"a\":\"\xc3\x28\"}",                                         /* not UTF-8 */
        "\xef\xbb\xbf{}",                                               /* a byte order mark */
    };
    UcDeviceState s, before;

    uc_device_state_init (&s);
    g_assert_true (uc_device_state_merge_json (&s, full_json, NULL, NULL));
    before = s;
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        const char *error = NULL;
        GString *ignored = g_string_new (NULL);

        g_test_message ("%s", cases[i]);
        g_assert_false (uc_device_state_merge_json (&s, cases[i], ignored, &error));
        g_assert_nonnull (error);
        g_assert_cmpstr (ignored->str, ==, "");
        g_assert_true (uc_device_state_equal (&s, &before));
        g_string_free (ignored, TRUE);
    }
    {
        const char *error = NULL;
        g_assert_false (uc_device_state_merge_json (&s, NULL, NULL, &error));
        g_assert_cmpstr (error, ==, "no JSON");
    }
}

static void
test_limits (void)
{
    UcDeviceState s;
    GString *json = g_string_new ("{\"screen\":\"off\",\"pad\":\"");
    const char *error = NULL;
    char *deep;

    uc_device_state_init (&s);
    /* exactly UC_DEVICE_JSON_MAX bytes are taken, one more is refused */
    while (json->len < UC_DEVICE_JSON_MAX - 2)
        g_string_append_c (json, 'x');
    g_string_append (json, "\"}");
    g_assert_cmpuint (json->len, ==, UC_DEVICE_JSON_MAX);
    g_assert_true (uc_device_state_merge_json (&s, json->str, NULL, &error));
    g_assert_cmpint (s.screen, ==, UC_DEVICE_SCREEN_OFF);
    g_string_insert_c (json, 1, ' ');
    g_assert_false (uc_device_state_merge_json (&s, json->str, NULL, &error));
    g_assert_cmpstr (error, ==, "longer than 4096 bytes");
    g_string_free (json, TRUE);

    /* 16 levels are taken, 17 refused */
    deep = g_strdup_printf ("{\"a\":%s1%s}", "[[[[[[[[[[[[[[[", "]]]]]]]]]]]]]]]");
    g_assert_true (uc_device_state_merge_json (&s, deep, NULL, &error));
    g_free (deep);
    deep = g_strdup_printf ("{\"a\":%s1%s}", "[[[[[[[[[[[[[[[[", "]]]]]]]]]]]]]]]]");
    g_assert_false (uc_device_state_merge_json (&s, deep, NULL, &error));
    g_assert_cmpstr (error, ==, "nested too deeply");
    g_free (deep);
}

static void
assert_ascii (const char *text)
{
    for (const char *p = text; *p != '\0'; p++)
        g_assert_true (*p == '\n' || (*p >= 0x20 && *p <= 0x7e));
}

static void
test_start_script (void)
{
    UcDeviceState s;
    char *script;

    uc_device_state_init (&s);
    script = uc_device_start_script (&s);
    g_assert_nonnull (strstr (script, "if (typeof " UC_EXT_DEVICE_START " === 'function') " UC_EXT_DEVICE_START
                                      " ({\"screen\":\"on\",\"powerSource\":\"ac\",\"powerProfile\":\"balanced\","
                                      "\"level\":100});\n"));
    g_assert_true (g_str_has_prefix (script, "/*"));
    assert_ascii (script);
    g_free (script);

    g_assert_true (uc_device_state_merge_json (&s, full_json, NULL, NULL));
    script = uc_device_start_script (&s);
    g_assert_nonnull (strstr (script, UC_EXT_DEVICE_START " ({\"screen\":\"dim\",\"powerSource\":\"battery\","
                                      "\"powerProfile\":\"power-saver\",\"level\":42});"));
    /* one call, nothing of the host's input as text, nothing for the page's world */
    g_assert_null (strstr (strstr (script, UC_EXT_DEVICE_START " (") + 1, UC_EXT_DEVICE_START " ("));
    g_assert_null (strstr (script, "window."));
    g_assert_null (strstr (script, "__uc"));
    assert_ascii (script);
    g_free (script);
}

/* the compiled-in shim is the file, one function expression, without the entry point of the first draft */
static void
test_shim_bytes (void)
{
    char *file = NULL;
    gsize length = 0;
    const char *shim = (const char *) uc_device_shim_source;

    g_assert_cmpuint (strlen (shim), ==, sizeof uc_device_shim_source - 1);
    assert_ascii (shim);
    g_assert_true (g_str_has_prefix (strstr (shim, "\n(function (init) {"), "\n(function (init) {"));
    g_assert_nonnull (strstr (shim, "    return update;\n})\n"));
    g_assert_null (strstr (shim, "__ucDeviceHostUpdate"));
    g_assert_null (strstr (shim, "@state@"));
    if (!g_file_get_contents ("device-host-shim.js", &file, &length, NULL)) {
        g_test_skip ("device-host-shim.js is not in the current directory (run from lib/)");
        return;
    }
    g_assert_cmpuint (length, ==, strlen (shim));
    g_assert_cmpstr (file, ==, shim);
    g_free (file);
}

static void
assert_state (GVariant *change, const char *screen, const char *source, const char *profile, gint32 level)
{
    const char *s = NULL;
    gint32 l = -1;

    g_assert_true (g_variant_lookup (change, "screen", "&s", &s));
    g_assert_cmpstr (s, ==, screen);
    g_assert_true (g_variant_lookup (change, "powerSource", "&s", &s));
    g_assert_cmpstr (s, ==, source);
    g_assert_true (g_variant_lookup (change, "powerProfile", "&s", &s));
    g_assert_cmpstr (s, ==, profile);
    g_assert_true (g_variant_lookup (change, "level", "i", &l));
    g_assert_cmpint (l, ==, level);
}

static void
test_changes (void)
{
    UcDeviceState s;
    GVariant *change;
    const char *event = NULL;
    double slept = -1;

    uc_device_state_init (&s);
    g_assert_true (uc_device_state_merge_json (&s, full_json, NULL, NULL));
    change = g_variant_ref_sink (uc_device_state_change (&s));
    g_assert_true (g_variant_is_of_type (change, G_VARIANT_TYPE (UC_EXT_DEVICE_VARIANT_TYPE)));
    g_assert_cmpuint (g_variant_n_children (change), ==, 4);
    assert_state (change, "dim", "battery", "power-saver", 42);
    g_assert_false (g_variant_lookup (change, "event", "&s", &event));
    g_variant_unref (change);

    change = g_variant_ref_sink (uc_device_event_change (&s, UC_DEVICE_EVENT_SUSPEND, 99));
    g_assert_cmpuint (g_variant_n_children (change), ==, 5);
    g_assert_true (g_variant_lookup (change, "event", "&s", &event));
    g_assert_cmpstr (event, ==, "suspend");
    g_assert_false (g_variant_lookup (change, "sleptMs", "d", &slept));
    assert_state (change, "dim", "battery", "power-saver", 42);
    g_variant_unref (change);

    change = g_variant_ref_sink (uc_device_event_change (&s, UC_DEVICE_EVENT_RESUME, G_GINT64_CONSTANT (3600000123)));
    g_assert_cmpuint (g_variant_n_children (change), ==, 6);
    g_assert_true (g_variant_lookup (change, "event", "&s", &event));
    g_assert_cmpstr (event, ==, "resume");
    g_assert_true (g_variant_lookup (change, "sleptMs", "d", &slept));
    g_assert_cmpfloat (slept, ==, 3600000123.0);
    g_variant_unref (change);

    change = g_variant_ref_sink (uc_device_event_change (&s, UC_DEVICE_EVENT_RESUME, -7));
    g_assert_true (g_variant_lookup (change, "sleptMs", "d", &slept));
    g_assert_cmpfloat (slept, ==, 0.0);
    g_variant_unref (change);
}

typedef struct {
    const char *variant;    /* GVariant text */
    const char *json;       /* NULL: refused */
} ChangeCase;

static void
test_change_json (void)
{
    static const ChangeCase cases[] = {
        { "@a{sv} {}", "{}" },
        { "{'screen': <'dim'>, 'powerSource': <'battery'>, 'powerProfile': <'power-saver'>, 'level': <int32 42>}",
          "{\"screen\":\"dim\",\"powerSource\":\"battery\",\"powerProfile\":\"power-saver\",\"level\":42}" },
        { "{'event': <'suspend'>, 'screen': <'off'>}", "{\"screen\":\"off\",\"event\":\"suspend\"}" },
        { "{'event': <'resume'>, 'sleptMs': <61234.0>}", "{\"event\":\"resume\",\"sleptMs\":61234}" },
        { "{'event': <'resume'>, 'sleptMs': <3600000123.0>}", "{\"event\":\"resume\",\"sleptMs\":3600000123}" },
        { "{'sleptMs': <1e300>}", "{\"sleptMs\":9007199254740991}" },
        /* only known names, only valid values, only the right types; text that would leave the string is not a name */
        { "{'screen': <'bright'>, 'powerSource': <'solar'>, 'powerProfile': <1>, 'level': <int32 101>}", "{}" },
        { "{'screen': <'\\\"}); alert(1); //'>, 'level': <int32 -1>, 'event': <'standby'>}", "{}" },
        { "{'level': <42.0>, 'sleptMs': <int32 5>, 'thermal': <'critical'>}", "{}" },
        { "{'sleptMs': <-1.0>, 'level': <int32 0>}", "{\"level\":0}" },
        { "{'level': <int32 100>, 'event': <'Resume'>}", "{\"level\":100}" },
        { "('screen', 'dim')", NULL },
        { "@as ['screen']", NULL },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        GError *error = NULL;
        GVariant *change = g_variant_parse (NULL, cases[i].variant, NULL, NULL, &error);
        char *json;

        g_assert_no_error (error);
        g_test_message ("%s", cases[i].variant);
        json = uc_device_change_json (change);
        g_assert_cmpstr (json, ==, cases[i].json);
        g_free (json);
        g_variant_unref (change);
    }
    g_assert_null (uc_device_change_json (NULL));
}

/* the change of a resume in a locale with a decimal comma: still a JSON number */
static void
test_change_json_locale (void)
{
    static const char *const locales[] = { "de_DE.UTF-8", "de_CH.UTF-8", "fr_FR.UTF-8", NULL };
    UcDeviceState s;
    GVariant *change;
    char *json, *old = g_strdup (setlocale (LC_NUMERIC, NULL));
    gboolean comma = FALSE;

    for (int i = 0; locales[i] != NULL && !comma; i++)
        comma = setlocale (LC_NUMERIC, locales[i]) != NULL && strcmp (localeconv ()->decimal_point, ",") == 0;
    if (!comma) {
        setlocale (LC_NUMERIC, old);
        g_free (old);
        g_test_skip ("no locale with a decimal comma on this host");
        return;
    }
    uc_device_state_init (&s);
    change = g_variant_ref_sink (uc_device_event_change (&s, UC_DEVICE_EVENT_RESUME, 1500));
    json = uc_device_change_json (change);
    setlocale (LC_NUMERIC, old);
    g_free (old);
    g_assert_nonnull (strstr (json, "\"sleptMs\":1500}"));
    g_free (json);
    g_variant_unref (change);
}

static void
test_events (void)
{
    UcDeviceEvent event = UC_DEVICE_EVENT_RESUME;

    g_assert_true (uc_device_event_parse ("suspend", &event));
    g_assert_cmpint (event, ==, UC_DEVICE_EVENT_SUSPEND);
    g_assert_true (uc_device_event_parse ("resume", &event));
    g_assert_cmpint (event, ==, UC_DEVICE_EVENT_RESUME);
    g_assert_false (uc_device_event_parse ("Suspend", &event));
    g_assert_false (uc_device_event_parse ("freeze", &event));
    g_assert_false (uc_device_event_parse ("", &event));
    g_assert_false (uc_device_event_parse (NULL, &event));
    g_assert_cmpint (event, ==, UC_DEVICE_EVENT_RESUME);
}

/* ---- the throttling features against WebKitGTK, when the host has it ------------------------------------------ */

typedef struct _WebKitFeature WebKitFeature;
typedef struct _WebKitFeatureList WebKitFeatureList;

static struct {
    GModule *module;
    const char *name;
    WebKitFeatureList *(*get_all_features) (void);
    WebKitFeature *(*list_find) (WebKitFeatureList *, const char *);
    void (*list_unref) (WebKitFeatureList *);
    gboolean (*get_default_value) (WebKitFeature *);
    const char *(*get_identifier) (WebKitFeature *);
} wk;

static gboolean
load_webkit (void)
{
    static const char *const names[] = { "libwebkit2gtk-4.1.so.0", "libwebkitgtk-6.0.so.4", NULL };

    if (wk.module != NULL)
        return TRUE;
    for (int i = 0; names[i] != NULL && wk.module == NULL; i++) {
        wk.module = g_module_open (names[i], G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
        wk.name = names[i];
    }
    if (wk.module == NULL)
        return FALSE;
    /* webkit_feature_list_find() is new in 2.54 */
    if (!g_module_symbol (wk.module, "webkit_settings_get_all_features", (gpointer *) &wk.get_all_features)
        || !g_module_symbol (wk.module, "webkit_feature_list_find", (gpointer *) &wk.list_find)
        || !g_module_symbol (wk.module, "webkit_feature_list_unref", (gpointer *) &wk.list_unref)
        || !g_module_symbol (wk.module, "webkit_feature_get_default_value", (gpointer *) &wk.get_default_value)
        || !g_module_symbol (wk.module, "webkit_feature_get_identifier", (gpointer *) &wk.get_identifier)) {
        g_module_close (wk.module);
        wk.module = NULL;
        return FALSE;
    }
    return TRUE;
}

static void
test_webkit_features (void)
{
    static const char *const ids[] = { UC_DEVICE_FEATURE_TIMER_THROTTLING, UC_DEVICE_FEATURE_ANIMATION_SUSPEND };
    WebKitFeatureList *features;

    if (!load_webkit ()) {
        g_test_skip ("no WebKitGTK 2.54 on this host");
        return;
    }
    g_test_message ("feature list of %s", wk.name);
    features = wk.get_all_features ();
    /* the preference names are not identifiers (WebKitFeature.cpp:126-135) */
    g_assert_null (wk.list_find (features, "HiddenPageDOMTimerThrottlingEnabled"));
    g_assert_null (wk.list_find (features, "HiddenPageCSSAnimationSuspensionEnabled"));
    for (gsize i = 0; i < G_N_ELEMENTS (ids); i++) {
        WebKitFeature *feature = wk.list_find (features, ids[i]);
        g_assert_nonnull (feature);
        g_assert_cmpstr (wk.get_identifier (feature), ==, ids[i]);
        /* on by default for GTK (WPE: off, so the library switches them on) */
        g_assert_true (wk.get_default_value (feature));
    }
    wk.list_unref (features);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/device/defaults", test_defaults);
    g_test_add_func ("/device/full-state", test_full_state);
    g_test_add_func ("/device/partial-and-unknown", test_partial_and_unknown);
    g_test_add_func ("/device/invalid-members", test_invalid_members);
    g_test_add_func ("/device/refused", test_refused);
    g_test_add_func ("/device/limits", test_limits);
    g_test_add_func ("/device/start-script", test_start_script);
    g_test_add_func ("/device/shim-bytes", test_shim_bytes);
    g_test_add_func ("/device/changes", test_changes);
    g_test_add_func ("/device/change-json", test_change_json);
    g_test_add_func ("/device/change-json-locale", test_change_json_locale);
    g_test_add_func ("/device/events", test_events);
    g_test_add_func ("/device/webkit-features", test_webkit_features);
    return g_test_run ();
}
