/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Device state for web pages (library 0.9.0) without WebKit, see uc_device.h. The JSON parser is a strict RFC 8259
 * reader into a GVariant tree (objects a{sv}, arrays av, strings s, numbers d, true/false b, null an empty mv); the
 * library has no JSON dependency and its input is a few members.
 */
#include "uc_device.h"
#include "uc_ext_protocol.h"

#include <math.h>
#include <string.h>

#define UC_DEVICE_JSON_DEPTH 16

static const char *const screen_names[] = { "on", "dim", "off" };
static const char *const source_names[] = { "ac", "battery" };
static const char *const profile_names[] = { "power-saver", "balanced", "performance" };

/* ---- JSON ------------------------------------------------------------------------------------------------------ */

typedef struct {
    const char *p, *end;
    int depth;
    const char *error;
} Parser;

static GVariant *parse_value (Parser *ps);

static void
skip_space (Parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static gboolean
fail (Parser *ps, const char *error)
{
    if (ps->error == NULL)
        ps->error = error;
    return FALSE;
}

static gboolean
hex4 (Parser *ps, gunichar *out)
{
    gunichar value = 0;

    if (ps->end - ps->p < 4)
        return fail (ps, "a \\u escape without four hex digits");
    for (int i = 0; i < 4; i++) {
        int digit = g_ascii_xdigit_value (ps->p[i]);
        if (digit < 0)
            return fail (ps, "a \\u escape without four hex digits");
        value = value * 16 + (gunichar) digit;
    }
    ps->p += 4;
    *out = value;
    return TRUE;
}

/* at the opening quote; a newly allocated UTF-8 string or NULL */
static char *
parse_string (Parser *ps)
{
    GString *s = g_string_new (NULL);

    ps->p++;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char) *ps->p++;
        gunichar u;

        if (c == '"')
            return g_string_free (s, FALSE);
        if (c < 0x20) {
            fail (ps, "a control character in a string");
            break;
        }
        if (c != '\\') {
            g_string_append_c (s, (char) c);    /* the input is valid UTF-8 as a whole */
            continue;
        }
        if (ps->p >= ps->end)
            break;
        c = (unsigned char) *ps->p++;
        switch (c) {
        case '"': case '\\': case '/': g_string_append_c (s, (char) c); continue;
        case 'b': g_string_append_c (s, '\b'); continue;
        case 'f': g_string_append_c (s, '\f'); continue;
        case 'n': g_string_append_c (s, '\n'); continue;
        case 'r': g_string_append_c (s, '\r'); continue;
        case 't': g_string_append_c (s, '\t'); continue;
        case 'u': break;
        default:
            fail (ps, "an invalid escape in a string");
            g_string_free (s, TRUE);
            return NULL;
        }
        if (!hex4 (ps, &u))
            break;
        if (u >= 0xd800 && u <= 0xdbff) {
            gunichar low;
            if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                fail (ps, "a lone surrogate in a string");
                break;
            }
            ps->p += 2;
            if (!hex4 (ps, &low))
                break;
            if (low < 0xdc00 || low > 0xdfff) {
                fail (ps, "a lone surrogate in a string");
                break;
            }
            u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
        } else if (u >= 0xdc00 && u <= 0xdfff) {
            fail (ps, "a lone surrogate in a string");
            break;
        }
        if (u == 0) {
            fail (ps, "\\u0000 in a string");
            break;
        }
        g_string_append_unichar (s, u);
    }
    fail (ps, "an unterminated string");
    g_string_free (s, TRUE);
    return NULL;
}

static GVariant *
parse_number (Parser *ps)
{
    const char *start = ps->p;
    char *copy;
    double value;

    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;
    if (ps->p < ps->end && *ps->p == '0') {
        ps->p++;
    } else if (ps->p < ps->end && *ps->p >= '1' && *ps->p <= '9') {
        while (ps->p < ps->end && g_ascii_isdigit (*ps->p))
            ps->p++;
    } else {
        fail (ps, "an invalid number");
        return NULL;
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || !g_ascii_isdigit (*ps->p)) {
            fail (ps, "an invalid number");
            return NULL;
        }
        while (ps->p < ps->end && g_ascii_isdigit (*ps->p))
            ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;
        if (ps->p >= ps->end || !g_ascii_isdigit (*ps->p)) {
            fail (ps, "an invalid number");
            return NULL;
        }
        while (ps->p < ps->end && g_ascii_isdigit (*ps->p))
            ps->p++;
    }
    /* the C locale's strtod, whatever the process' locale */
    copy = g_strndup (start, (gsize) (ps->p - start));
    value = g_ascii_strtod (copy, NULL);
    g_free (copy);
    if (!isfinite (value)) {
        fail (ps, "a number out of range");
        return NULL;
    }
    return g_variant_new_double (value);
}

static gboolean
literal (Parser *ps, const char *word)
{
    size_t n = strlen (word);
    if ((size_t) (ps->end - ps->p) < n || memcmp (ps->p, word, n) != 0)
        return FALSE;
    ps->p += n;
    return TRUE;
}

static GVariant *
parse_object (Parser *ps)
{
    GVariantBuilder builder;
    GHashTable *names;

    if (++ps->depth > UC_DEVICE_JSON_DEPTH) {
        fail (ps, "nested too deeply");
        return NULL;
    }
    ps->p++;
    g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
    names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    skip_space (ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        goto done;
    }
    for (;;) {
        char *name;
        GVariant *value;

        skip_space (ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            fail (ps, "an object member without a name");
            goto error;
        }
        name = parse_string (ps);
        if (name == NULL)
            goto error;
        if (g_hash_table_contains (names, name)) {
            g_free (name);
            fail (ps, "a member name twice in an object");
            goto error;
        }
        g_hash_table_add (names, name);
        skip_space (ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            fail (ps, "a member without ':'");
            goto error;
        }
        ps->p++;
        value = parse_value (ps);
        if (value == NULL)
            goto error;
        g_variant_builder_add (&builder, "{sv}", name, value);
        skip_space (ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            break;
        }
        fail (ps, "an object without ',' or '}'");
        goto error;
    }
done:
    g_hash_table_unref (names);
    ps->depth--;
    return g_variant_builder_end (&builder);
error:
    g_hash_table_unref (names);
    g_variant_builder_clear (&builder);
    return NULL;
}

static GVariant *
parse_array (Parser *ps)
{
    GVariantBuilder builder;

    if (++ps->depth > UC_DEVICE_JSON_DEPTH) {
        fail (ps, "nested too deeply");
        return NULL;
    }
    ps->p++;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));
    skip_space (ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        ps->depth--;
        return g_variant_builder_end (&builder);
    }
    for (;;) {
        GVariant *value = parse_value (ps);
        if (value == NULL) {
            g_variant_builder_clear (&builder);
            return NULL;
        }
        g_variant_builder_add (&builder, "v", value);
        skip_space (ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            ps->depth--;
            return g_variant_builder_end (&builder);
        }
        fail (ps, "an array without ',' or ']'");
        g_variant_builder_clear (&builder);
        return NULL;
    }
}

static GVariant *
parse_value (Parser *ps)
{
    skip_space (ps);
    if (ps->p >= ps->end) {
        fail (ps, "a value is missing");
        return NULL;
    }
    switch (*ps->p) {
    case '{':
        return parse_object (ps);
    case '[':
        return parse_array (ps);
    case '"': {
        char *s = parse_string (ps);
        return s != NULL ? g_variant_new_take_string (s) : NULL;
    }
    case 't':
        if (literal (ps, "true"))
            return g_variant_new_boolean (TRUE);
        break;
    case 'f':
        if (literal (ps, "false"))
            return g_variant_new_boolean (FALSE);
        break;
    case 'n':
        if (literal (ps, "null"))
            return g_variant_new_maybe (G_VARIANT_TYPE_VARIANT, NULL);
        break;
    default:
        if (*ps->p == '-' || g_ascii_isdigit (*ps->p))
            return parse_number (ps);
        break;
    }
    fail (ps, "an invalid value");
    return NULL;
}

/* the root object of json, a new reference, or NULL with *error */
static GVariant *
parse_json_object (const char *json, const char **error)
{
    Parser ps = { NULL, NULL, 0, NULL };
    size_t len;
    GVariant *root;

    if (json == NULL) {
        *error = "no JSON";
        return NULL;
    }
    len = strlen (json);
    if (len > UC_DEVICE_JSON_MAX) {
        *error = "longer than 4096 bytes";
        return NULL;
    }
    if (!g_utf8_validate (json, (gssize) len, NULL)) {
        *error = "not UTF-8";
        return NULL;
    }
    ps.p = json;
    ps.end = json + len;
    skip_space (&ps);
    if (ps.p >= ps.end || *ps.p != '{') {
        *error = "not a JSON object";
        return NULL;
    }
    root = parse_value (&ps);
    if (root == NULL) {
        *error = ps.error != NULL ? ps.error : "invalid JSON";
        return NULL;
    }
    g_variant_ref_sink (root);
    skip_space (&ps);
    if (ps.p != ps.end) {
        g_variant_unref (root);
        *error = "text after the JSON object";
        return NULL;
    }
    return root;
}

/* ---- state ------------------------------------------------------------------------------------------------------ */

void
uc_device_state_init (UcDeviceState *state)
{
    state->screen = UC_DEVICE_SCREEN_ON;
    state->source = UC_DEVICE_SOURCE_AC;
    state->profile = UC_DEVICE_PROFILE_BALANCED;
    state->level = 100;
}

gboolean
uc_device_state_equal (const UcDeviceState *a, const UcDeviceState *b)
{
    return a->screen == b->screen && a->source == b->source && a->profile == b->profile && a->level == b->level;
}

static void
add_ignored (GString *ignored, const char *name)
{
    if (ignored != NULL)
        g_string_append_printf (ignored, "%s%s", ignored->len > 0 ? ", " : "", name);
}

/* member name of obj as one of names[n]: its index, -1 when absent, -2 when present and invalid */
static int
lookup_name (GVariant *obj, const char *member, const char *const *names, int n)
{
    GVariant *value = g_variant_lookup_value (obj, member, NULL);
    int index = -2;

    if (value == NULL)
        return -1;
    if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string (value, NULL);
        for (int i = 0; i < n; i++) {
            if (strcmp (s, names[i]) == 0)
                index = i;
        }
    }
    g_variant_unref (value);
    return index;
}

gboolean
uc_device_state_merge_json (UcDeviceState *state, const char *json, GString *ignored, const char **error)
{
    const char *reason = NULL;
    GVariant *root = parse_json_object (json, &reason);
    GVariant *level;
    int index;

    if (root == NULL) {
        if (error != NULL)
            *error = reason;
        return FALSE;
    }
    index = lookup_name (root, "screen", screen_names, G_N_ELEMENTS (screen_names));
    if (index >= 0)
        state->screen = (UcDeviceScreen) index;
    else if (index == -2)
        add_ignored (ignored, "screen");
    index = lookup_name (root, "powerSource", source_names, G_N_ELEMENTS (source_names));
    if (index >= 0)
        state->source = (UcDeviceSource) index;
    else if (index == -2)
        add_ignored (ignored, "powerSource");
    index = lookup_name (root, "powerProfile", profile_names, G_N_ELEMENTS (profile_names));
    if (index >= 0)
        state->profile = (UcDeviceProfile) index;
    else if (index == -2)
        add_ignored (ignored, "powerProfile");

    /* an integer in 0..100, in any JSON form of it (42, 42.0, 4.2e1) */
    level = g_variant_lookup_value (root, "level", NULL);
    if (level != NULL && g_variant_is_of_type (level, G_VARIANT_TYPE_DOUBLE) && g_variant_get_double (level) >= 0.0
        && g_variant_get_double (level) <= 100.0
        && (double) (int) g_variant_get_double (level) == g_variant_get_double (level))
        state->level = (int) g_variant_get_double (level);
    else if (level != NULL)
        add_ignored (ignored, "level");
    g_clear_pointer (&level, g_variant_unref);
    g_variant_unref (root);
    return TRUE;
}

char *
uc_device_state_describe (const UcDeviceState *state)
{
    return g_strdup_printf ("screen %s, %s, %s, %d %%", screen_names[state->screen], source_names[state->source],
                            profile_names[state->profile], state->level);
}

/* ---- the start script and the changes -------------------------------------------------------------------------- */

char *
uc_device_start_script (const UcDeviceState *state)
{
    /* only names of the tables and an integer: nothing of the host's input as text */
    return g_strdup_printf ("/* libucwebview: the device API of this document (uc_ext_protocol.h) */\n"
                            "if (typeof " UC_EXT_DEVICE_START " === 'function') " UC_EXT_DEVICE_START
                            " ({\"screen\":\"%s\",\"powerSource\":\"%s\",\"powerProfile\":\"%s\",\"level\":%d});\n",
                            screen_names[state->screen], source_names[state->source], profile_names[state->profile],
                            CLAMP (state->level, 0, 100));
}

/* the state's members of a change, into builder (an a{sv}) */
static void
add_state (GVariantBuilder *builder, const UcDeviceState *state)
{
    g_variant_builder_add (builder, "{sv}", "screen", g_variant_new_string (screen_names[state->screen]));
    g_variant_builder_add (builder, "{sv}", "powerSource", g_variant_new_string (source_names[state->source]));
    g_variant_builder_add (builder, "{sv}", "powerProfile", g_variant_new_string (profile_names[state->profile]));
    g_variant_builder_add (builder, "{sv}", "level", g_variant_new_int32 (CLAMP (state->level, 0, 100)));
}

GVariant *
uc_device_state_change (const UcDeviceState *state)
{
    GVariantBuilder change;

    g_variant_builder_init (&change, G_VARIANT_TYPE_VARDICT);
    add_state (&change, state);
    return g_variant_builder_end (&change);
}

GVariant *
uc_device_event_change (const UcDeviceState *state, UcDeviceEvent event, gint64 slept_ms)
{
    GVariantBuilder change;

    g_variant_builder_init (&change, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add (&change, "{sv}", "event",
                           g_variant_new_string (event == UC_DEVICE_EVENT_SUSPEND ? "suspend" : "resume"));
    if (event == UC_DEVICE_EVENT_RESUME)
        g_variant_builder_add (&change, "{sv}", "sleptMs", g_variant_new_double ((double) MAX (slept_ms, 0)));
    add_state (&change, state);
    return g_variant_builder_end (&change);
}

static const char *const event_names[] = { "suspend", "resume" };

/* member of change as one of names[n], NULL when absent, not a string or not one of them */
static const char *
change_name (GVariant *change, const char *member, const char *const *names, int n)
{
    const char *s = NULL;

    if (!g_variant_lookup (change, member, "&s", &s))
        return NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp (s, names[i]) == 0)
            return names[i];
    }
    return NULL;
}

static void
json_member (GString *json, const char *name, const char *value)
{
    if (value != NULL)
        g_string_append_printf (json, "%s\"%s\":\"%s\"", json->len > 1 ? "," : "", name, value);
}

char *
uc_device_change_json (GVariant *change)
{
    GString *json;
    gint32 level;
    double slept;

    if (change == NULL || !g_variant_is_of_type (change, G_VARIANT_TYPE_VARDICT))
        return NULL;
    /* the values are names of the tables (no escaping needed) and numbers */
    json = g_string_new ("{");
    json_member (json, "screen", change_name (change, "screen", screen_names, G_N_ELEMENTS (screen_names)));
    json_member (json, "powerSource", change_name (change, "powerSource", source_names, G_N_ELEMENTS (source_names)));
    json_member (json, "powerProfile",
                 change_name (change, "powerProfile", profile_names, G_N_ELEMENTS (profile_names)));
    if (g_variant_lookup (change, "level", "i", &level) && level >= 0 && level <= 100)
        g_string_append_printf (json, "%s\"level\":%d", json->len > 1 ? "," : "", (int) level);
    json_member (json, "event", change_name (change, "event", event_names, G_N_ELEMENTS (event_names)));
    if (g_variant_lookup (change, "sleptMs", "d", &slept) && isfinite (slept) && slept >= 0.0) {
        char number[G_ASCII_DTOSTR_BUF_SIZE];
        /* whole milliseconds, at most 2^53 - 1 (an int64 from the host fits, JavaScript keeps it exact);
         * g_ascii_formatd: '.' whatever LC_NUMERIC says */
        g_ascii_formatd (number, sizeof number, "%.0f", MIN (slept, 9007199254740991.0));
        g_string_append_printf (json, "%s\"sleptMs\":%s", json->len > 1 ? "," : "", number);
    }
    g_string_append_c (json, '}');
    return g_string_free (json, FALSE);
}

gboolean
uc_device_event_parse (const char *type, UcDeviceEvent *event)
{
    if (g_strcmp0 (type, "suspend") == 0)
        *event = UC_DEVICE_EVENT_SUSPEND;
    else if (g_strcmp0 (type, "resume") == 0)
        *event = UC_DEVICE_EVENT_RESUME;
    else
        return FALSE;
    return TRUE;
}
