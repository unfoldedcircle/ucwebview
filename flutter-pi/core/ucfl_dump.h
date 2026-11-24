/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * Frame dumps for checks on a device without anyone looking at its screen (README.md, "Frame dumps"): with a dump
 * directory set, a host adapter writes a view's frame as a binary PPM when the trigger file <dir>/dump appears, and
 * once for the first frame. The adapter gets the pixels (from GL or a shared memory frame); these functions do the
 * files. PPM because it needs no library: ImageMagick's convert and Python's PIL read it.
 *
 * Thread: one thread per struct ucfl_dump (the adapter's platform thread); ucfl_write_ppm() on any.
 */
#ifndef UCFL_DUMP_H
#define UCFL_DUMP_H

#include <stdbool.h>
#include <stddef.h>

/* the trigger file in the dump directory: `touch <dir>/dump` */
#define UCFL_DUMP_TRIGGER "dump"
/* how often the adapter looks for the trigger */
#define UCFL_DUMP_POLL_MS 500
#define UCFL_DUMP_DIR_MAX 512
/* a file name of the dump directory fits in this */
#define UCFL_DUMP_PATH_MAX (UCFL_DUMP_DIR_MAX + 32)

/* the pixel layouts of ucfl_write_ppm(): 4 bytes per pixel, the 4th (alpha or padding) is dropped */
enum ucfl_pixels {
    UCFL_PIXELS_RGBA, /* R, G, B, A bytes: glReadPixels() with GL_RGBA and GL_UNSIGNED_BYTE */
    UCFL_PIXELS_BGRA, /* B, G, R, A bytes: DRM ARGB8888 little endian, the library's shared memory frames */
};

struct ucfl_dump {
    char dir[UCFL_DUMP_DIR_MAX]; /* empty: dumps are off */
    unsigned next;               /* the number of the next file, from 1 */
    bool first_done;             /* the first frame was dumped */
    bool trigger_off;            /* the trigger could not be deleted: not looked at any more */
};

/*
 * dir NULL or empty: dumps off, 0. A dir of UCFL_DUMP_DIR_MAX bytes or more: off, ENAMETOOLONG. Otherwise on (trailing
 * slashes dropped), and the directory is created (one level, mode 0700: frames show page content) when it is missing;
 * a failure of that is returned as an errno value, the dumps stay on (the directory may appear later).
 */
int ucfl_dump_init(struct ucfl_dump *dump, const char *dir);
bool ucfl_dump_on(const struct ucfl_dump *dump);
/*
 * Looks for the trigger file (one access()) and deletes it: true once per touch of the file. The caller looks every
 * UCFL_DUMP_POLL_MS. A trigger that cannot be deleted would fire on every look: false with its errno value in *error,
 * and the trigger is not looked at again. *error is 0 otherwise.
 */
bool ucfl_dump_triggered(struct ucfl_dump *dump, int *error);
/* True the first time only (dumps on): the automatic dump of the first frame. */
bool ucfl_dump_first(struct ucfl_dump *dump);
/* The name of the next file, <dir>/frame-<n>.ppm with n from 1, into path (UCFL_DUMP_PATH_MAX bytes suffice). */
void ucfl_dump_next_path(struct ucfl_dump *dump, char *path, size_t size);

/*
 * Writes width x height pixels to path as a binary PPM (P6, maxval 255), through path.tmp and rename(), so that a
 * reader never sees half a file. Rows of pixels are stride bytes apart. The file's first row is the first row of
 * pixels, or the last one with bottom_up. Returns 0 or an errno value (EINVAL for a size of 0, a stride below
 * width * 4 or no pixels).
 */
int ucfl_write_ppm(const char *path, const void *pixels, int width, int height, size_t stride, enum ucfl_pixels layout,
                   bool bottom_up);

#endif /* UCFL_DUMP_H */
