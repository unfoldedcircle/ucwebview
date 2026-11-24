/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Host unit test of core/ucfl_dump.c: the PPM writer (both pixel layouts, a padded stride, the row order, the
 * temporary file), and the dump directory, its trigger file and the file names, in a temporary directory.
 */
#include "ucfl_dump.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <glib.h>
#include <glib/gstdio.h>

/* 3 x 2 pixels, 16 bytes per row (4 bytes of padding): top row red, green, blue; bottom row white, black, grey */
static const uint8_t rgba[] = {
    0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff, 0xee, 0xee, 0xee, 0xee,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0x80, 0x40, 0x20, 0x00, 0xee, 0xee, 0xee, 0xee,
};
/* the same image as B, G, R, A bytes */
static const uint8_t bgra[] = {
    0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xee, 0xee, 0xee, 0xee,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff, 0x20, 0x40, 0x80, 0x00, 0xee, 0xee, 0xee, 0xee,
};
static const char header[] = "P6\n3 2\n255\n";
static const uint8_t top_row[] = { 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff };
static const uint8_t bottom_row[] = { 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x80, 0x40, 0x20 };

static char *make_dir(void)
{
    GError *error = NULL;
    char *dir = g_dir_make_tmp("ucfl-dump-XXXXXX", &error);

    g_assert_no_error(error);
    return dir;
}

static void remove_dir(char *dir)
{
    const char *name;
    GDir *d = g_dir_open(dir, 0, NULL);

    while (d && (name = g_dir_read_name(d))) {
        char *path = g_build_filename(dir, name, NULL);
        g_assert_cmpint(g_remove(path), ==, 0);
        g_free(path);
    }
    if (d)
        g_dir_close(d);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_free(dir);
}

static void touch(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    GError *error = NULL;

    g_file_set_contents(path, "", 0, &error);
    g_assert_no_error(error);
    g_free(path);
}

static bool exists(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    bool found = g_file_test(path, G_FILE_TEST_EXISTS);

    g_free(path);
    return found;
}

/* the PPM of the test image, top row first or bottom row first */
static void check_ppm(const char *path, bool flipped)
{
    char *data = NULL;
    size_t size = 0;
    GError *error = NULL;
    size_t h = sizeof(header) - 1;

    g_file_get_contents(path, &data, &size, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(size, ==, h + 2 * sizeof(top_row));
    g_assert_cmpmem(data, h, header, h);
    g_assert_cmpmem(data + h, sizeof(top_row), flipped ? bottom_row : top_row, sizeof(top_row));
    g_assert_cmpmem(data + h + sizeof(top_row), sizeof(bottom_row), flipped ? top_row : bottom_row,
                    sizeof(bottom_row));
    g_free(data);
}

static void test_ppm(void)
{
    char *dir = make_dir();
    char *path = g_build_filename(dir, "frame.ppm", NULL);

    g_assert_cmpint(ucfl_write_ppm(path, rgba, 3, 2, 16, UCFL_PIXELS_RGBA, false), ==, 0);
    check_ppm(path, false);
    g_assert_cmpint(ucfl_write_ppm(path, bgra, 3, 2, 16, UCFL_PIXELS_BGRA, false), ==, 0); /* replaces the file */
    check_ppm(path, false);
    g_assert_cmpint(ucfl_write_ppm(path, rgba, 3, 2, 16, UCFL_PIXELS_RGBA, true), ==, 0);
    check_ppm(path, true);
    g_assert_false(exists(dir, "frame.ppm.tmp"));
    g_free(path);
    remove_dir(dir);
}


/* a stale or planted .tmp is replaced, never followed; the file is the owner's only */
static void test_ppm_tmp(void)
{
    char *dir = make_dir();
    char *path = g_build_filename(dir, "frame.ppm", NULL);
    char *tmp = g_build_filename(dir, "frame.ppm.tmp", NULL);
    char *target = g_build_filename(dir, "target", NULL);
    char *contents = NULL;
    struct stat st;

    g_assert_true(g_file_set_contents(target, "keep", -1, NULL));
    g_assert_cmpint(symlink(target, tmp), ==, 0);
    g_assert_cmpint(ucfl_write_ppm(path, rgba, 3, 2, 16, UCFL_PIXELS_RGBA, false), ==, 0);
    check_ppm(path, false);
    g_assert_true(g_file_get_contents(target, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "keep");
    g_assert_cmpint(stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    g_assert_false(exists(dir, "frame.ppm.tmp"));
    g_free(contents);
    g_free(target);
    g_free(tmp);
    g_free(path);
    remove_dir(dir);
}

static void test_ppm_errors(void)
{
    char *dir = make_dir();
    char *path = g_build_filename(dir, "frame.ppm", NULL);
    char *missing = g_build_filename(dir, "no-such-dir", "frame.ppm", NULL);

    g_assert_cmpint(ucfl_write_ppm(path, rgba, 3, 2, 11, UCFL_PIXELS_RGBA, false), ==, EINVAL);
    g_assert_cmpint(ucfl_write_ppm(path, rgba, 0, 2, 16, UCFL_PIXELS_RGBA, false), ==, EINVAL);
    g_assert_cmpint(ucfl_write_ppm(path, rgba, 3, 0, 16, UCFL_PIXELS_RGBA, false), ==, EINVAL);
    g_assert_cmpint(ucfl_write_ppm(path, NULL, 3, 2, 16, UCFL_PIXELS_RGBA, false), ==, EINVAL);
    g_assert_cmpint(ucfl_write_ppm(missing, rgba, 3, 2, 16, UCFL_PIXELS_RGBA, false), ==, ENOENT);
    g_assert_false(exists(dir, "frame.ppm"));
    g_free(missing);
    g_free(path);
    remove_dir(dir);
}

static void test_off(void)
{
    struct ucfl_dump dump;
    char long_dir[UCFL_DUMP_DIR_MAX + 1];
    int error = -1;

    g_assert_cmpint(ucfl_dump_init(&dump, NULL), ==, 0);
    g_assert_false(ucfl_dump_on(&dump));
    g_assert_false(ucfl_dump_first(&dump));
    g_assert_false(ucfl_dump_triggered(&dump, &error));
    g_assert_cmpint(error, ==, 0);
    g_assert_cmpint(ucfl_dump_init(&dump, ""), ==, 0);
    g_assert_false(ucfl_dump_on(&dump));

    memset(long_dir, 'a', sizeof(long_dir) - 1);
    long_dir[0] = '/';
    long_dir[sizeof(long_dir) - 1] = '\0';
    g_assert_cmpint(ucfl_dump_init(&dump, long_dir), ==, ENAMETOOLONG);
    g_assert_false(ucfl_dump_on(&dump));
}

static void test_trigger(void)
{
    char *dir = make_dir();
    char *with_slash = g_strconcat(dir, "//", NULL);
    struct ucfl_dump dump;
    char path[UCFL_DUMP_PATH_MAX];
    char *expected;
    int error = -1;

    g_assert_cmpint(ucfl_dump_init(&dump, with_slash), ==, 0);
    g_assert_true(ucfl_dump_on(&dump));
    g_assert_cmpstr(dump.dir, ==, dir);

    g_assert_true(ucfl_dump_first(&dump));
    g_assert_false(ucfl_dump_first(&dump));

    g_assert_false(ucfl_dump_triggered(&dump, &error));
    g_assert_cmpint(error, ==, 0);
    touch(dir, UCFL_DUMP_TRIGGER);
    g_assert_true(ucfl_dump_triggered(&dump, &error));
    g_assert_cmpint(error, ==, 0);
    g_assert_false(exists(dir, UCFL_DUMP_TRIGGER)); /* one touch, one dump */
    g_assert_false(ucfl_dump_triggered(&dump, &error));
    touch(dir, UCFL_DUMP_TRIGGER);
    g_assert_true(ucfl_dump_triggered(&dump, &error));

    ucfl_dump_next_path(&dump, path, sizeof(path));
    expected = g_build_filename(dir, "frame-1.ppm", NULL);
    g_assert_cmpstr(path, ==, expected);
    g_free(expected);
    ucfl_dump_next_path(&dump, path, sizeof(path));
    expected = g_build_filename(dir, "frame-2.ppm", NULL);
    g_assert_cmpstr(path, ==, expected);
    g_free(expected);

    g_free(with_slash);
    remove_dir(dir);
}

/* the directory is created when missing; a trigger that cannot be deleted turns the trigger off */
static void test_create_and_stuck_trigger(void)
{
    char *parent = make_dir();
    char *dir = g_build_filename(parent, "dumps", NULL);
    struct ucfl_dump dump;
    struct stat st;
    int error = 0;

    g_assert_cmpint(ucfl_dump_init(&dump, dir), ==, 0);
    g_assert_cmpint(stat(dir, &st), ==, 0);
    g_assert_true(S_ISDIR(st.st_mode));
    g_assert_cmpint(st.st_mode & 0777, ==, 0700);
    g_assert_cmpint(ucfl_dump_init(&dump, dir), ==, 0); /* exists: fine */

    if (geteuid() != 0) {
        /* a read-only directory: the trigger stays */
        touch(dir, UCFL_DUMP_TRIGGER);
        g_assert_cmpint(chmod(dir, 0500), ==, 0);
        g_assert_false(ucfl_dump_triggered(&dump, &error));
        g_assert_cmpint(error, ==, EACCES);
        g_assert_true(dump.trigger_off);
        g_assert_false(ucfl_dump_triggered(&dump, &error));
        g_assert_cmpint(error, ==, 0);
        g_assert_cmpint(chmod(dir, 0700), ==, 0);
    }

    remove_dir(dir);
    g_assert_cmpint(g_rmdir(parent), ==, 0);
    g_free(parent);

    /* a parent that does not exist: the error, the dumps stay on */
    g_assert_cmpint(ucfl_dump_init(&dump, "/nonexistent-ucfl-parent/dumps"), ==, ENOENT);
    g_assert_true(ucfl_dump_on(&dump));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dump/ppm", test_ppm);
    g_test_add_func("/dump/ppm-errors", test_ppm_errors);
    g_test_add_func("/dump/ppm-tmp", test_ppm_tmp);
    g_test_add_func("/dump/off", test_off);
    g_test_add_func("/dump/trigger", test_trigger);
    g_test_add_func("/dump/create-and-stuck-trigger", test_create_and_stuck_trigger);
    return g_test_run();
}
