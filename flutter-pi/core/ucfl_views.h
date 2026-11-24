/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The views of a host adapter by the id the channel protocol uses. Ids start at 1 and are never reused in a process,
 * so a late call for a disposed view gets "noView" instead of reaching a newer one.
 *
 * Thread: the adapter's platform thread only (no locking).
 */
#ifndef UCFL_VIEWS_H
#define UCFL_VIEWS_H

#include <stddef.h>
#include <stdint.h>

struct ucfl_views {
    struct ucfl_view_slot {
        int64_t id;
        void *data;
    } *slots;
    size_t count;
    size_t capacity;
    int64_t last_id;
};

void ucfl_views_init(struct ucfl_views *views);
/* Frees the table, not the data of the views. */
void ucfl_views_free(struct ucfl_views *views);
/* Adds data (not NULL) under a new id; returns the id, or -1 without memory. */
int64_t ucfl_views_add(struct ucfl_views *views, void *data);
/* Adds data (not NULL) under id (> 0, not in the table): a second table that mirrors another one's ids. 0, or -1 for
 * an invalid or present id or without memory. Later ucfl_views_add() ids stay above id. */
int ucfl_views_insert(struct ucfl_views *views, int64_t id, void *data);
/* The data of id, NULL for an unknown id. */
void *ucfl_views_get(const struct ucfl_views *views, int64_t id);
/* Removes id and returns its data, NULL for an unknown id. */
void *ucfl_views_remove(struct ucfl_views *views, int64_t id);

#endif /* UCFL_VIEWS_H */
