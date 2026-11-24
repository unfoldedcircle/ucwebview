/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Flutter's StandardMessageCodec and StandardMethodCodec at byte level, for the uc.webview channels.
 *
 * The format is the one of packages/flutter/lib/src/services/message_codecs.dart (StandardMessageCodec,
 * StandardMethodCodec): one type byte per value, sizes as 1, 3 or 5 bytes (< 254; 254 + uint16; 255 + uint32),
 * numbers little endian, float64 values and the elements of the typed lists aligned to their size relative to the
 * start of the message. A method call is the method name (a string value) followed by the arguments (one value);
 * a reply is a success envelope (byte 0, then the result) or an error envelope (byte 1, then code, message,
 * details); an event of an EventChannel is a success envelope.
 *
 * No host types: the flutter-pi adapter and a later GTK adapter pass the raw bytes of their platform messages.
 * Thread: any, no global state.
 */
#ifndef UCFL_CODEC_H
#define UCFL_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* type bytes of StandardMessageCodec */
enum ucfl_type {
    UCFL_NULL = 0,
    UCFL_TRUE = 1,
    UCFL_FALSE = 2,
    UCFL_INT32 = 3,
    UCFL_INT64 = 4,
    UCFL_LARGE_INT = 5, /* hex string of old Dart versions; refused by the decoder */
    UCFL_FLOAT64 = 6,
    UCFL_STRING = 7,
    UCFL_UINT8_LIST = 8,
    UCFL_INT32_LIST = 9,
    UCFL_INT64_LIST = 10,
    UCFL_FLOAT64_LIST = 11,
    UCFL_LIST = 12,
    UCFL_MAP = 13,
    UCFL_FLOAT32_LIST = 14,
};

/* Deepest nesting of lists and maps the decoder accepts. */
#define UCFL_MAX_DEPTH 32

/*
 * A decoded value. Strings and typed lists point into the message: the value is valid while the message bytes are.
 * Lists and maps own their arrays (ucfl_value_clear()).
 */
struct ucfl_value {
    enum ucfl_type type;
    union {
        int64_t i; /* UCFL_INT32, UCFL_INT64 */
        double d;  /* UCFL_FLOAT64 */
        struct {
            const char *data; /* UTF-8 as sent, not NUL terminated */
            size_t len;
        } str;
        struct {
            const uint8_t *data; /* the elements, little endian, aligned within the message */
            size_t count;        /* number of elements */
        } array;                 /* the typed lists */
        struct {
            struct ucfl_value *items;
            size_t count;
        } list;
        struct {
            struct ucfl_value *entries; /* entries[2 * i] is a key, entries[2 * i + 1] its value */
            size_t count;               /* number of key/value pairs */
        } map;
    };
};

/* Decodes the value at *pos of the message msg[0..size) and moves *pos behind it. Returns 0, EBADMSG for a malformed
 * or truncated message or one nested deeper than UCFL_MAX_DEPTH, ENOMEM; out is UCFL_NULL after an error. */
int ucfl_decode_value(const uint8_t *msg, size_t size, size_t *pos, struct ucfl_value *out);

/* Frees the arrays of a decoded value and sets it to UCFL_NULL. */
void ucfl_value_clear(struct ucfl_value *value);

/* Decodes a whole method call: method must be a string and nothing may follow the arguments. Returns 0, EBADMSG or
 * ENOMEM; clear both values afterwards (also after an error). */
int ucfl_decode_method_call(const uint8_t *msg, size_t size, struct ucfl_value *method, struct ucfl_value *args);

/* Decodes a whole envelope (a reply or an event). *is_error tells which; value is the result, or for an error the
 * error code (message, details and stack trace are checked, not returned). Returns 0, EBADMSG or ENOMEM. For the
 * tests and a later Dart contract check. */
int ucfl_decode_envelope(const uint8_t *msg, size_t size, bool *is_error, struct ucfl_value *value);

/* accessors; NULL values are allowed and give false/NULL */
bool ucfl_value_is_string(const struct ucfl_value *value, const char *s);
const struct ucfl_value *ucfl_map_get(const struct ucfl_value *map, const char *key);
bool ucfl_value_get_int(const struct ucfl_value *value, int64_t *out);   /* UCFL_INT32/UCFL_INT64 */
bool ucfl_value_get_number(const struct ucfl_value *value, double *out); /* an int or a float64 */
bool ucfl_value_get_bool(const struct ucfl_value *value, bool *out);
/* A malloc()ed NUL terminated copy of a string value; NULL for another type, a string with a NUL byte, or no memory. */
char *ucfl_value_dup_string(const struct ucfl_value *value);

/* ---- writer -------------------------------------------------------------- */

/* A growing message buffer. After a failed allocation every further put is ignored and failed stays true: check it
 * once before sending. */
struct ucfl_writer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
};

void ucfl_writer_init(struct ucfl_writer *w);
void ucfl_writer_free(struct ucfl_writer *w);

void ucfl_put_null(struct ucfl_writer *w);
void ucfl_put_bool(struct ucfl_writer *w, bool value);
/* As Dart does: int32 when it fits, else int64. */
void ucfl_put_int(struct ucfl_writer *w, int64_t value);
void ucfl_put_double(struct ucfl_writer *w, double value);
/* NULL writes null. The bytes must be UTF-8 (Dart decodes them with utf8.decoder). */
void ucfl_put_string(struct ucfl_writer *w, const char *s);
void ucfl_put_string_len(struct ucfl_writer *w, const char *s, size_t len);
/* The headers of a list of count values and of a map of count key/value pairs; the caller puts the elements. */
void ucfl_put_list(struct ucfl_writer *w, size_t count);
void ucfl_put_map(struct ucfl_writer *w, size_t count);

/* StandardMethodCodec envelopes */
void ucfl_put_method_call(struct ucfl_writer *w, const char *method); /* the caller puts the arguments next */
void ucfl_put_success_envelope(struct ucfl_writer *w);                /* the caller puts the result next */
/* A complete error envelope: code, message (NULL = null) and null details. */
void ucfl_put_error_envelope(struct ucfl_writer *w, const char *code, const char *message);

#endif /* UCFL_CODEC_H */
