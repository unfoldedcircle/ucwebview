/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Frame dumps as PPM files, see ucfl_dump.h.
 */
#include "ucfl_dump.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

int ucfl_dump_init(struct ucfl_dump *dump, const char *dir)
{
    size_t len;

    memset(dump, 0, sizeof(*dump));
    dump->next = 1;
    if (!dir || !*dir)
        return 0;
    len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/')
        len--;
    if (len >= sizeof(dump->dir))
        return ENAMETOOLONG;
    memcpy(dump->dir, dir, len);
    dump->dir[len] = '\0';
    if (mkdir(dump->dir, 0700) != 0 && errno != EEXIST)
        return errno;
    return 0;
}

bool ucfl_dump_on(const struct ucfl_dump *dump)
{
    return dump->dir[0] != '\0';
}

bool ucfl_dump_triggered(struct ucfl_dump *dump, int *error)
{
    char path[UCFL_DUMP_PATH_MAX];

    *error = 0;
    if (!ucfl_dump_on(dump) || dump->trigger_off)
        return false;
    snprintf(path, sizeof(path), "%s/" UCFL_DUMP_TRIGGER, dump->dir);
    if (access(path, F_OK) != 0)
        return false;
    if (unlink(path) != 0 && errno != ENOENT) {
        *error = errno;
        dump->trigger_off = true;
        return false;
    }
    return true;
}

bool ucfl_dump_first(struct ucfl_dump *dump)
{
    if (!ucfl_dump_on(dump) || dump->first_done)
        return false;
    dump->first_done = true;
    return true;
}

void ucfl_dump_next_path(struct ucfl_dump *dump, char *path, size_t size)
{
    snprintf(path, size, "%s/frame-%u.ppm", dump->dir, dump->next++);
}

int ucfl_write_ppm(const char *path, const void *pixels, int width, int height, size_t stride, enum ucfl_pixels layout,
                   bool bottom_up)
{
    const uint8_t *src = pixels;
    size_t red = layout == UCFL_PIXELS_BGRA ? 2 : 0, blue = 2 - red;
    char tmp[UCFL_DUMP_PATH_MAX + 8];
    uint8_t *row;
    FILE *file;
    int fd, err = 0;

    if (!path || !pixels || width <= 0 || height <= 0 || stride < (size_t) width * 4)
        return EINVAL;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp))
        return ENAMETOOLONG;
    row = malloc((size_t) width * 3);
    if (!row)
        return ENOMEM;
    /* a new file of our own, mode 0600 (frames show page content): a stale .tmp of a crashed run goes first, and a
     * symbolic link planted in its place is refused rather than followed */
    if (unlink(tmp) != 0 && errno != ENOENT) {
        err = errno;
        free(row);
        return err;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!file) {
        err = errno;
        if (fd >= 0)
            close(fd);
        free(row);
        return err;
    }

    errno = 0;
    if (fprintf(file, "P6\n%d %d\n255\n", width, height) < 0)
        err = errno ? errno : EIO;
    for (int y = 0; !err && y < height; y++) {
        const uint8_t *in = src + (size_t) (bottom_up ? height - 1 - y : y) * stride;
        uint8_t *out = row;

        for (int x = 0; x < width; x++, in += 4, out += 3) {
            out[0] = in[red];
            out[1] = in[1];
            out[2] = in[blue];
        }
        if (fwrite(row, 3, (size_t) width, file) != (size_t) width)
            err = errno ? errno : EIO;
    }
    if (fclose(file) != 0 && !err)
        err = errno ? errno : EIO;
    free(row);
    if (!err && rename(tmp, path) != 0)
        err = errno;
    if (err)
        unlink(tmp);
    return err;
}
