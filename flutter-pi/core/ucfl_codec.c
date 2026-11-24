/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * StandardMessageCodec / StandardMethodCodec at byte level, see ucfl_codec.h.
 */
#include "ucfl_codec.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- reader -------------------------------------------------------------- */

struct reader {
    const uint8_t *msg;
    size_t size;
    size_t pos;
    size_t budget; /* list and map items still allowed: nested lists cannot make up more items than the message has
                      bytes, whatever counts they claim before their elements are read */
};

static bool take(struct reader *r, size_t n, const uint8_t **out)
{
    if (n > r->size - r->pos)
        return false;
    *out = r->msg + r->pos;
    r->pos += n;
    return true;
}

static uint64_t le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* aligns the position to a multiple of alignment from the start of the message, as Dart's ReadBuffer._alignTo() */
static bool align(struct reader *r, size_t alignment)
{
    const uint8_t *skip;
    size_t mod = r->pos % alignment;
    return mod == 0 || take(r, alignment - mod, &skip);
}

static bool read_size(struct reader *r, size_t *out)
{
    const uint8_t *p;
    if (!take(r, 1, &p))
        return false;
    if (p[0] < 254) {
        *out = p[0];
        return true;
    }
    int n = p[0] == 254 ? 2 : 4;
    if (!take(r, (size_t) n, &p))
        return false;
    *out = (size_t) le(p, n);
    return true;
}

static int decode(struct reader *r, struct ucfl_value *out, int depth)
{
    const uint8_t *p;
    size_t count, elem;

    memset(out, 0, sizeof(*out));
    out->type = UCFL_NULL;
    if (depth > UCFL_MAX_DEPTH || !take(r, 1, &p))
        return EBADMSG;

    switch (p[0]) {
    case UCFL_NULL:
    case UCFL_TRUE:
    case UCFL_FALSE:
        out->type = (enum ucfl_type) p[0];
        return 0;
    case UCFL_INT32:
        if (!take(r, 4, &p))
            return EBADMSG;
        out->type = UCFL_INT32;
        out->i = (int32_t) (uint32_t) le(p, 4);
        return 0;
    case UCFL_INT64:
        if (!take(r, 8, &p))
            return EBADMSG;
        out->type = UCFL_INT64;
        out->i = (int64_t) le(p, 8);
        return 0;
    case UCFL_FLOAT64: {
        uint64_t bits;
        if (!align(r, 8) || !take(r, 8, &p))
            return EBADMSG;
        bits = le(p, 8);
        memcpy(&out->d, &bits, sizeof(out->d));
        out->type = UCFL_FLOAT64;
        return 0;
    }
    case UCFL_STRING:
        if (!read_size(r, &count) || !take(r, count, &p))
            return EBADMSG;
        out->type = UCFL_STRING;
        out->str.data = (const char *) p;
        out->str.len = count;
        return 0;
    case UCFL_UINT8_LIST:
    case UCFL_INT32_LIST:
    case UCFL_INT64_LIST:
    case UCFL_FLOAT64_LIST:
    case UCFL_FLOAT32_LIST:
        elem = p[0] == UCFL_UINT8_LIST                                   ? 1
               : (p[0] == UCFL_INT32_LIST || p[0] == UCFL_FLOAT32_LIST) ? 4
                                                                        : 8;
        out->type = (enum ucfl_type) p[0];
        if (!read_size(r, &count) || !align(r, elem) || count > (r->size - r->pos) / elem || !take(r, count * elem, &p)) {
            out->type = UCFL_NULL;
            return EBADMSG;
        }
        out->array.data = p;
        out->array.count = count;
        return 0;
    case UCFL_LIST:
    case UCFL_MAP: {
        bool is_map = p[0] == UCFL_MAP;
        size_t n;
        struct ucfl_value *items;

        if (!read_size(r, &count))
            return EBADMSG;
        /* every value takes at least one byte: refuse counts the rest of the message cannot hold before allocating */
        n = is_map ? count * 2 : count;
        if (count > r->size - r->pos || (is_map && n > r->size - r->pos) || n > r->budget)
            return EBADMSG;
        r->budget -= n;
        items = n ? calloc(n, sizeof(*items)) : NULL;
        if (n && !items)
            return ENOMEM;
        out->type = (enum ucfl_type) p[0];
        if (is_map) {
            out->map.entries = items;
            out->map.count = count;
        } else {
            out->list.items = items;
            out->list.count = count;
        }
        for (size_t i = 0; i < n; i++) {
            int err = decode(r, &items[i], depth + 1);
            if (err) {
                ucfl_value_clear(out);
                return err;
            }
        }
        return 0;
    }
    default:
        return EBADMSG; /* UCFL_LARGE_INT and unknown types: custom types of a codec subclass are not supported */
    }
}

int ucfl_decode_value(const uint8_t *msg, size_t size, size_t *pos, struct ucfl_value *out)
{
    struct reader r = { msg, size, *pos, size };
    int err;

    if (!msg || *pos > size) {
        memset(out, 0, sizeof(*out));
        return EBADMSG;
    }
    err = decode(&r, out, 0);
    if (!err)
        *pos = r.pos;
    return err;
}

void ucfl_value_clear(struct ucfl_value *value)
{
    if (!value)
        return;
    if (value->type == UCFL_LIST || value->type == UCFL_MAP) {
        size_t n = value->type == UCFL_MAP ? value->map.count * 2 : value->list.count;
        struct ucfl_value *items = value->type == UCFL_MAP ? value->map.entries : value->list.items;
        for (size_t i = 0; items && i < n; i++)
            ucfl_value_clear(&items[i]);
        free(items);
    }
    memset(value, 0, sizeof(*value));
    value->type = UCFL_NULL;
}

int ucfl_decode_method_call(const uint8_t *msg, size_t size, struct ucfl_value *method, struct ucfl_value *args)
{
    size_t pos = 0;
    int err;

    memset(args, 0, sizeof(*args));
    err = ucfl_decode_value(msg, size, &pos, method);
    if (!err && method->type != UCFL_STRING)
        err = EBADMSG;
    if (!err)
        err = ucfl_decode_value(msg, size, &pos, args);
    if (!err && pos != size)
        err = EBADMSG;
    return err;
}

int ucfl_decode_envelope(const uint8_t *msg, size_t size, bool *is_error, struct ucfl_value *value)
{
    struct ucfl_value rest[3] = { 0 };
    size_t pos = 1;
    int err;

    memset(value, 0, sizeof(*value));
    if (!msg || size < 1 || msg[0] > 1)
        return EBADMSG;
    *is_error = msg[0] == 1;
    err = ucfl_decode_value(msg, size, &pos, value);
    if (!err && *is_error) {
        /* code (a string), message (a string or null), details, and the stack trace of newer Flutter versions
         * (StandardMethodCodec.decodeEnvelope()) */
        err = value->type == UCFL_STRING ? ucfl_decode_value(msg, size, &pos, &rest[0]) : EBADMSG;
        if (!err && rest[0].type != UCFL_STRING && rest[0].type != UCFL_NULL)
            err = EBADMSG;
        if (!err)
            err = ucfl_decode_value(msg, size, &pos, &rest[1]);
        if (!err && pos < size)
            err = ucfl_decode_value(msg, size, &pos, &rest[2]);
        for (size_t i = 0; i < 3; i++)
            ucfl_value_clear(&rest[i]);
    }
    if (!err && pos != size)
        err = EBADMSG;
    if (err)
        ucfl_value_clear(value);
    return err;
}

bool ucfl_value_is_string(const struct ucfl_value *value, const char *s)
{
    size_t len = strlen(s);
    return value && value->type == UCFL_STRING && value->str.len == len && memcmp(value->str.data, s, len) == 0;
}

const struct ucfl_value *ucfl_map_get(const struct ucfl_value *map, const char *key)
{
    if (!map || map->type != UCFL_MAP)
        return NULL;
    for (size_t i = 0; i < map->map.count; i++) {
        if (ucfl_value_is_string(&map->map.entries[2 * i], key))
            return &map->map.entries[2 * i + 1];
    }
    return NULL;
}

bool ucfl_value_get_int(const struct ucfl_value *value, int64_t *out)
{
    if (!value || (value->type != UCFL_INT32 && value->type != UCFL_INT64))
        return false;
    *out = value->i;
    return true;
}

bool ucfl_value_get_number(const struct ucfl_value *value, double *out)
{
    if (value && value->type == UCFL_FLOAT64) {
        *out = value->d;
        return true;
    }
    if (value && (value->type == UCFL_INT32 || value->type == UCFL_INT64)) {
        *out = (double) value->i;
        return true;
    }
    return false;
}

bool ucfl_value_get_bool(const struct ucfl_value *value, bool *out)
{
    if (!value || (value->type != UCFL_TRUE && value->type != UCFL_FALSE))
        return false;
    *out = value->type == UCFL_TRUE;
    return true;
}

char *ucfl_value_dup_string(const struct ucfl_value *value)
{
    char *s;

    if (!value || value->type != UCFL_STRING || memchr(value->str.data, '\0', value->str.len))
        return NULL;
    s = malloc(value->str.len + 1);
    if (!s)
        return NULL;
    memcpy(s, value->str.data, value->str.len);
    s[value->str.len] = '\0';
    return s;
}

/* ---- writer -------------------------------------------------------------- */

void ucfl_writer_init(struct ucfl_writer *w)
{
    memset(w, 0, sizeof(*w));
}

void ucfl_writer_free(struct ucfl_writer *w)
{
    free(w->data);
    memset(w, 0, sizeof(*w));
}

static void put_bytes(struct ucfl_writer *w, const void *bytes, size_t n)
{
    if (w->failed || n == 0)
        return;
    if (n > w->capacity - w->size) {
        size_t cap = w->capacity ? w->capacity : 64;
        while (cap - w->size < n) {
            if (cap > SIZE_MAX / 2) {
                w->failed = true;
                return;
            }
            cap *= 2;
        }
        uint8_t *data = realloc(w->data, cap);
        if (!data) {
            w->failed = true;
            return;
        }
        w->data = data;
        w->capacity = cap;
    }
    memcpy(w->data + w->size, bytes, n);
    w->size += n;
}

static void put_u8(struct ucfl_writer *w, uint8_t v)
{
    put_bytes(w, &v, 1);
}

static void put_le(struct ucfl_writer *w, uint64_t v, int n)
{
    uint8_t b[8];
    for (int i = 0; i < n; i++)
        b[i] = (uint8_t) (v >> (8 * i));
    put_bytes(w, b, (size_t) n);
}

static void put_size(struct ucfl_writer *w, size_t n)
{
    if (n < 254) {
        put_u8(w, (uint8_t) n);
    } else if (n <= 0xffff) {
        put_u8(w, 254);
        put_le(w, n, 2);
    } else if (n <= 0xffffffffu) {
        put_u8(w, 255);
        put_le(w, n, 4);
    } else {
        w->failed = true;
    }
}

void ucfl_put_null(struct ucfl_writer *w)
{
    put_u8(w, UCFL_NULL);
}

void ucfl_put_bool(struct ucfl_writer *w, bool value)
{
    put_u8(w, value ? UCFL_TRUE : UCFL_FALSE);
}

void ucfl_put_int(struct ucfl_writer *w, int64_t value)
{
    if (value >= INT32_MIN && value <= INT32_MAX) {
        put_u8(w, UCFL_INT32);
        put_le(w, (uint64_t) value, 4);
    } else {
        put_u8(w, UCFL_INT64);
        put_le(w, (uint64_t) value, 8);
    }
}

void ucfl_put_double(struct ucfl_writer *w, double value)
{
    static const uint8_t zeros[8];
    uint64_t bits;

    put_u8(w, UCFL_FLOAT64);
    if (w->size % 8)
        put_bytes(w, zeros, 8 - w->size % 8);
    memcpy(&bits, &value, sizeof(bits));
    put_le(w, bits, 8);
}

void ucfl_put_string_len(struct ucfl_writer *w, const char *s, size_t len)
{
    put_u8(w, UCFL_STRING);
    put_size(w, len);
    put_bytes(w, s, len);
}

void ucfl_put_string(struct ucfl_writer *w, const char *s)
{
    if (s)
        ucfl_put_string_len(w, s, strlen(s));
    else
        ucfl_put_null(w);
}

void ucfl_put_list(struct ucfl_writer *w, size_t count)
{
    put_u8(w, UCFL_LIST);
    put_size(w, count);
}

void ucfl_put_map(struct ucfl_writer *w, size_t count)
{
    put_u8(w, UCFL_MAP);
    put_size(w, count);
}

void ucfl_put_method_call(struct ucfl_writer *w, const char *method)
{
    ucfl_put_string(w, method);
}

void ucfl_put_success_envelope(struct ucfl_writer *w)
{
    put_u8(w, 0);
}

void ucfl_put_error_envelope(struct ucfl_writer *w, const char *code, const char *message)
{
    put_u8(w, 1);
    ucfl_put_string(w, code);
    ucfl_put_string(w, message);
    ucfl_put_null(w);
}
