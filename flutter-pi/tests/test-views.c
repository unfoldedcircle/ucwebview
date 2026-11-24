/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test of core/ucfl_views.c: ids start at 1, are never reused, and lookups of removed ids fail.
 */
#include "ucfl_views.h"

#include <glib.h>

static void test_ids(void)
{
    struct ucfl_views views;
    int a, b, c;

    ucfl_views_init(&views);
    g_assert_cmpint(ucfl_views_add(&views, NULL), ==, -1);
    g_assert_cmpint(ucfl_views_add(&views, &a), ==, 1);
    g_assert_cmpint(ucfl_views_add(&views, &b), ==, 2);
    g_assert_true(ucfl_views_get(&views, 1) == &a);
    g_assert_true(ucfl_views_remove(&views, 1) == &a);
    g_assert_null(ucfl_views_get(&views, 1));
    g_assert_null(ucfl_views_remove(&views, 1));
    g_assert_cmpint(ucfl_views_add(&views, &c), ==, 3); /* not 1 again */
    g_assert_true(ucfl_views_get(&views, 2) == &b);
    g_assert_true(ucfl_views_get(&views, 3) == &c);
    for (int i = 0; i < 20; i++)
        g_assert_cmpint(ucfl_views_add(&views, &a), ==, 4 + i); /* the table grows */
    g_assert_cmpuint(views.count, ==, 22);
    g_assert_null(ucfl_views_get(&views, 0));
    ucfl_views_free(&views);

    /* a mirror of another table's ids */
    ucfl_views_init(&views);
    g_assert_cmpint(ucfl_views_insert(&views, 5, &a), ==, 0);
    g_assert_cmpint(ucfl_views_insert(&views, 5, &b), ==, -1); /* present */
    g_assert_cmpint(ucfl_views_insert(&views, 0, &b), ==, -1);
    g_assert_cmpint(ucfl_views_insert(&views, 2, NULL), ==, -1);
    g_assert_cmpint(ucfl_views_insert(&views, 2, &b), ==, 0);
    g_assert_true(ucfl_views_get(&views, 5) == &a);
    g_assert_cmpint(ucfl_views_add(&views, &c), ==, 6);
    ucfl_views_free(&views);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/views/ids", test_ids);
    return g_test_run();
}
