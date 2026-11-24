/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * The views of a host adapter by id, see ucfl_views.h. A remote shows one or two views at a time, so a linear table
 * is enough.
 */
#include "ucfl_views.h"

#include <stdlib.h>
#include <string.h>

void ucfl_views_init(struct ucfl_views *views)
{
    memset(views, 0, sizeof(*views));
}

void ucfl_views_free(struct ucfl_views *views)
{
    free(views->slots);
    memset(views, 0, sizeof(*views));
}

static int grow(struct ucfl_views *views)
{
    if (views->count == views->capacity) {
        size_t capacity = views->capacity ? views->capacity * 2 : 4;
        struct ucfl_view_slot *slots = realloc(views->slots, capacity * sizeof(*slots));
        if (!slots)
            return -1;
        views->slots = slots;
        views->capacity = capacity;
    }
    return 0;
}

int64_t ucfl_views_add(struct ucfl_views *views, void *data)
{
    if (!data || grow(views) != 0)
        return -1;
    views->slots[views->count].id = ++views->last_id;
    views->slots[views->count].data = data;
    views->count++;
    return views->last_id;
}

int ucfl_views_insert(struct ucfl_views *views, int64_t id, void *data)
{
    if (!data || id <= 0 || ucfl_views_get(views, id) || grow(views) != 0)
        return -1;
    views->slots[views->count].id = id;
    views->slots[views->count].data = data;
    views->count++;
    if (id > views->last_id)
        views->last_id = id;
    return 0;
}

void *ucfl_views_get(const struct ucfl_views *views, int64_t id)
{
    for (size_t i = 0; i < views->count; i++) {
        if (views->slots[i].id == id)
            return views->slots[i].data;
    }
    return NULL;
}

void *ucfl_views_remove(struct ucfl_views *views, int64_t id)
{
    for (size_t i = 0; i < views->count; i++) {
        if (views->slots[i].id == id) {
            void *data = views->slots[i].data;
            memmove(&views->slots[i], &views->slots[i + 1], (views->count - i - 1) * sizeof(views->slots[0]));
            views->count--;
            return data;
        }
    }
    return NULL;
}
