/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * flutter-pi plugin "uc_webview": web views of libucwebview shown as flutter-pi textures and driven over the
 * uc.webview channels (PROTOCOL.md). Built into flutter-pi from src/plugins/uc_webview with the CMake option
 * BUILD_UC_WEBVIEW_PLUGIN (README.md); the library itself is dlopen()ed on the first call that needs it.
 *
 * The protocol, the library, the views and the page's requests are the host-neutral core's (core/ucfl_host.h); this
 * file is the flutter-pi adapter: the channels (raw platform messages, replies through their response handles, also
 * later for the replies that wait for the library), GLib's main context on the platform thread (ucfp_glib_loop.c),
 * and one "surface" per view: a flutter-pi texture and the frames in flight.
 *
 * Threads
 *  - Platform thread (flutter-pi's sd_event loop): the channel handlers, every libucwebview call, the library's
 *    callbacks (GLib's default context iterated by ucfp_glib_loop.c), and the frame completions below.
 *  - Raster thread: the engine fetches a texture frame (texture_registry.c, texture_gl_external_texture_frame_callback())
 *    and calls resolve_frame() with the engine's GL context current. The EGLImage is bound to a new GL texture there,
 *    in that context: no context switch and no cross-context synchronisation. The engine drops the frame on the
 *    raster thread when it fetched the next one (FlutterOpenGLTexture.destruction_callback); flutter-pi drops a frame
 *    the engine never fetched on the platform thread (texture_push_*() replaced it, texture_destroy()).
 *  - Both report to the platform thread through a mutex-protected list and an eventfd in the sd_event loop
 *    ("done" list), never through flutterpi_post_platform_task(): that takes flutter-pi's event loop lock, which the
 *    platform thread holds while it dispatches GLib, so the raster thread would wait for the dispatch.
 *
 * Frame lifecycle (uc_webview.h, "Frame lifecycle"), per frame_ready():
 *   1. pushed as an unresolved texture frame (texture_push_unresolved_frame()); the engine is told a frame is there.
 *      With the default pacing ("fetch", core/ucfl_pacing.h) every frame is pushed at once and reported when
 *      fetched; with UC_WEBVIEW_FRAME_PACING=ahead the push is reported to the library as rendered at once, and a
 *      frame that arrives while the previous push is not fetched yet waits in the view until it is;
 *   2. resolve_frame() when the engine fetches it: GL texture from the EGLImage (or glTexImage2D of a shared memory
 *      frame), and a "fetched" note to the platform thread: this one is reported (uc_webview_frame_rendered(),
 *      "fetch"), or the waiting frame is pushed ("ahead");
 *   3. dropped (by the engine after the next fetch, or unfetched by flutter-pi) -> "released" note ->
 *      uc_webview_frame_release() and glDeleteTextures() on the platform thread in a context of the plugin's own
 *      (shared with flutter-pi's root context), made current only for that.
 * Why not report a frame when it is on screen: core/ucfl_pacing.h.
 *
 * Frame dumps (README.md, "Frame dumps"; UC_WEBVIEW_DUMP_DIR): on the platform thread, never the raster thread. The
 * newest frame of a view (view->latest) stays valid until its "released" note is handled there, which is only after
 * the engine fetched a newer one, so the trigger file works for a page that renders nothing new as well. A DMA-BUF
 * frame is read back in the plugin's own context: its EGLImage bound to a new texture as resolve_frame() binds it,
 * attached to a temporary framebuffer, glReadPixels(); the engine's context and its GL state are not touched.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>

#include "flutter-pi.h"
#include "gl_renderer.h"
#include "gles.h"
#include "pluginregistry.h"
#include "texture_registry.h"

#include "ucfl_codec.h"
#include "ucfl_dump.h"
#include "ucfl_host.h"
#include "ucfl_loader.h"
#include "ucfl_pacing.h"
#include "ucfl_protocol.h"
#include "ucfl_views.h"
#include "ucfp_glib_loop.h"

#define LOG(fmt, ...) fprintf(stderr, "[uc_webview] " fmt "\n", ##__VA_ARGS__)

struct frame;

/* a note for the platform thread: the engine fetched the frame, or the frame was dropped */
struct done {
    struct done *next;
    struct frame *frame;
    bool released;
};

/* the surface of a view (struct ucfl_host_ops) */
struct view {
    struct plugin *plugin;
    int64_t id;
    int refs;        /* platform thread: the surface table's and one per frame not yet released */
    uc_webview *wv;  /* NULL once the view is disposed */
    struct texture *texture;
    int frame_width, frame_height; /* of the last frame event */
    bool frame_zero_copy, had_frame;
    struct frame *latest; /* the newest frame pushed, NULL once released: what a frame dump writes */
    struct ucfl_pacer pacer;
    struct ucfl_frame_clock clock;
    int64_t reported_ns; /* when a frame was last reported rendered, 0 once the next one arrived */
};

struct frame {
    struct view *view;
    uc_webview_frame f; /* the library's frame, copied */
    GLenum target;      /* set by resolve_frame() */
    GLuint texture;
    bool resolved;
    bool rendered;
    int64_t pushed_ns; /* platform thread: handed to the engine */
    int64_t fetch_ns;  /* raster thread, read on the platform thread after the "fetched" note */
    struct done fetched, released;
};

struct plugin {
    struct flutterpi *flutterpi;

    struct ucfl_host *host;
    const struct ucfl_lib *lib; /* set with the first surface: the library is loaded by then */
    struct ucfp_glib_loop *glib;
    struct ucfl_views views; /* the surfaces by view id */

    struct gl_renderer *renderer; /* NULL: flutter-pi renders without OpenGL ES */
    EGLDisplay egl_display;
    EGLContext egl_context; /* the plugin's own, for deleting textures on the platform thread; created on demand */
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
    bool has_bgra;

    pthread_mutex_t done_lock;
    struct done *done; /* newest first */
    int done_fd;       /* eventfd, -1 after deinit */
    sd_event_source *done_source;

    uint64_t frames_ready, frames_fetched, frames_released, frames_held;
    enum ucfl_pacing_mode pacing; /* UC_WEBVIEW_FRAME_PACING */
    struct ucfl_delay turnaround, to_fetch; /* of all views since the last stats reply */

    /* frame dumps (UC_WEBVIEW_DUMP_DIR), platform thread */
    struct ucfl_dump dump;
    int dump_fd; /* timerfd of the trigger's polls, -1 without */
    sd_event_source *dump_source;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_renderbuffer;
};

/* One instance: frames can outlive the plugin's deinit (the engine drops its last texture frame after it). */
static struct plugin plugin_instance;

/* ---- helpers ------------------------------------------------------------- */

static void log_line(void *ctx, const char *line)
{
    (void) ctx;
    LOG("%s", line);
}

/* ucfl_host_ops.respond: token is the message's response handle; the engine takes a late answer as well */
static void respond(void *ctx, void *token, const uint8_t *data, size_t size)
{
    (void) ctx;
    if (token)
        flutterpi_respond_to_platform_message(token, data, size);
}

/* ucfl_host_ops.send_event */
static void send_event(void *ctx, const uint8_t *data, size_t size)
{
    struct plugin *p = ctx;
    flutterpi_send_platform_message(p->flutterpi, UCFL_CHANNEL_EVENTS, data, size, NULL);
}

static void view_unref(struct view *view)
{
    if (--view->refs == 0)
        free(view);
}

/* any thread */
static int64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* any thread */
static void post_done(struct plugin *p, struct done *note)
{
    static const uint64_t one = 1;

    pthread_mutex_lock(&p->done_lock);
    note->next = p->done;
    p->done = note;
    if (p->done_fd >= 0 && write(p->done_fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
        LOG("waking the platform thread failed: %s", strerror(errno));
    pthread_mutex_unlock(&p->done_lock);
}

/* ---- raster thread: the engine fetches a frame ---------------------------- */

static void upload_pixels(struct plugin *p, const uc_webview_frame *f)
{
    GLenum format = p->has_bgra ? GL_BGRA_EXT : GL_RGBA;
    const uint8_t *pixels = f->pixels;
    uint8_t *copy = NULL;
    size_t row = (size_t) f->width * 4;

    /* GLES 2 has no row length: repack a padded frame; without BGRA8888 swap red and blue as well */
    if (!p->has_bgra || (size_t) f->stride != row) {
        copy = malloc(row * (size_t) f->height);
        if (!copy)
            return;
        for (int y = 0; y < f->height; y++) {
            uint8_t *dst = copy + (size_t) y * row;
            memcpy(dst, pixels + (size_t) y * (size_t) f->stride, row);
            for (size_t x = 0; !p->has_bgra && x < row; x += 4) {
                uint8_t b = dst[x];
                dst[x] = dst[x + 2];
                dst[x + 2] = b;
            }
        }
        pixels = copy;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint) format, f->width, f->height, 0, format, GL_UNSIGNED_BYTE, pixels);
    free(copy);
}

static GLuint new_texture(GLenum target)
{
    GLuint texture = 0;

    glGenTextures(1, &texture);
    glBindTexture(target, texture);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

static void on_resolved_frame_destroy(const struct texture_frame *texture_frame, void *userdata)
{
    struct frame *frame = userdata;

    (void) texture_frame;
    post_done(frame->view->plugin, &frame->released);
}

/*
 * Raster thread, engine GL context current, flutter-pi's texture lock held. The engine resets Skia's view of the GL
 * state before it asks for the texture (embedder_external_texture_gl.cc, ResolveTextureSkia()), so binding a texture
 * here is allowed. Must not fail: flutter-pi 8b86c52 dereferences the frame it just released when a resolve fails
 * (texture_registry.c, texture_gl_external_texture_frame_callback()). A frame that cannot be bound gets texture 0,
 * which the engine refuses and drops again.
 */
static int resolve_frame(size_t width, size_t height, void *userdata, struct texture_frame *out)
{
    struct frame *frame = userdata;
    struct plugin *p = frame->view->plugin;
    GLenum target = GL_TEXTURE_2D;
    GLuint texture;

    (void) width;
    (void) height;

    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {
        /* errors of others are not ours to report */
    }
    texture = new_texture(target);
    if (frame->f.egl_image) {
        /* GL_TEXTURE_2D as remote-ui's WebViewTexture; a driver that samples the image only as an external texture
         * gets GL_TEXTURE_EXTERNAL_OES */
        p->image_target_texture(target, (GLeglImageOES) frame->f.egl_image);
        if (glGetError() != GL_NO_ERROR) {
            glBindTexture(target, 0);
            glDeleteTextures(1, &texture);
            target = GL_TEXTURE_EXTERNAL_OES;
            texture = new_texture(target);
            p->image_target_texture(target, (GLeglImageOES) frame->f.egl_image);
            if (glGetError() != GL_NO_ERROR) {
                LOG("binding a frame's EGLImage to a texture failed");
                glBindTexture(target, 0);
                glDeleteTextures(1, &texture);
                texture = 0;
            }
        }
    } else if (frame->f.pixels) {
        upload_pixels(p, &frame->f);
    }
    glBindTexture(target, 0);

    frame->target = target;
    frame->texture = texture;
    frame->resolved = true;
    memset(out, 0, sizeof(*out));
    out->gl.target = target;
    out->gl.name = texture;
    out->gl.format = GL_RGBA8_OES;
    out->gl.width = (size_t) frame->f.width; /* the engine scales a frame of another size to the Texture widget */
    out->gl.height = (size_t) frame->f.height;
    out->destroy = on_resolved_frame_destroy;
    out->userdata = frame;
    frame->fetch_ns = now_ns();
    post_done(p, &frame->fetched);
    return 0;
}

/* Any thread: after resolve_frame() (nothing to do, the resolved frame's destroy follows), or for a frame the engine
 * never fetched. */
static void on_unresolved_frame_destroy(void *userdata)
{
    struct frame *frame = userdata;

    if (!frame->resolved)
        post_done(frame->view->plugin, &frame->released);
}

/* ---- platform thread: frames done ---------------------------------------- */

/* the EGL context current on the calling thread, to go back to */
struct egl_current {
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw, read;
};

static struct egl_current get_current(void)
{
    return (struct egl_current) {
        .display = eglGetCurrentDisplay(),
        .context = eglGetCurrentContext(),
        .draw = eglGetCurrentSurface(EGL_DRAW),
        .read = eglGetCurrentSurface(EGL_READ),
    };
}

static void restore_current(struct plugin *p, const struct egl_current *previous)
{
    if (previous->context != EGL_NO_CONTEXT)
        eglMakeCurrent(previous->display, previous->draw, previous->read, previous->context);
    else
        eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static bool make_own_context_current(struct plugin *p)
{
    if (p->egl_context == EGL_NO_CONTEXT) {
        p->egl_context = gl_renderer_create_context(p->renderer);
        if (p->egl_context == EGL_NO_CONTEXT) {
            LOG("no EGL context for deleting textures: they leak");
            return false;
        }
    }
    return eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, p->egl_context) == EGL_TRUE;
}

static void apply_pace(struct view *view, struct ucfl_pace pace);

static int on_done(sd_event_source *source, int fd, uint32_t revents, void *userdata)
{
    struct plugin *p = userdata;
    struct done *list, *fifo = NULL;
    uint64_t count;
    struct egl_current previous = get_current();
    bool switched = false, current = false;

    (void) source;
    (void) revents;
    if (read(fd, &count, sizeof(count)) < 0 && errno != EAGAIN)
        LOG("reading the frame notes failed: %s", strerror(errno));

    pthread_mutex_lock(&p->done_lock);
    list = p->done;
    p->done = NULL;
    pthread_mutex_unlock(&p->done_lock);
    while (list) { /* oldest first: a frame's "fetched" comes before its "released" */
        struct done *next = list->next;
        list->next = fifo;
        fifo = list;
        list = next;
    }

    for (struct done *note = fifo, *next; note; note = next) {
        struct frame *frame = note->frame;
        struct view *view = frame->view;

        next = note->next;
        if (!note->released) {
            p->frames_fetched++;
            ucfl_frame_clock_fetched(&view->clock, frame->fetch_ns);
            ucfl_delay_add(&view->clock.to_fetch, frame->pushed_ns, frame->fetch_ns);
            if (view->wv) /* not after dispose_view(): no texture, no library */
                apply_pace(view, ucfl_pacer_fetched(&view->pacer, frame));
            continue;
        }
        ucfl_pacer_gone(&view->pacer, frame);
        if (view->latest == frame)
            view->latest = NULL;
        if (view->wv)
            p->lib->frame_release(view->wv, frame->f.handle);
        if (frame->texture) {
            if (!switched) {
                switched = true;
                current = make_own_context_current(p);
            }
            if (current)
                glDeleteTextures(1, &frame->texture);
        }
        p->frames_released++;
        view_unref(view);
        free(frame);
    }

    if (switched)
        restore_current(p, &previous);
    ucfp_glib_loop_rearm(p->glib);
    return 0;
}

/* ---- platform thread: frame dumps ------------------------------------------ */

static void clear_gl_errors(void)
{
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {
        /* bounded: a context that is lost reports errors forever */
    }
}

/*
 * The pixels of a DMA-BUF frame as R, G, B, A bytes, width * 4 per row, the buffer's first row first; NULL after a
 * log line. In the plugin's own context: the EGLImage bound to a new GL_TEXTURE_2D texture as resolve_frame() binds
 * it, attached to a temporary framebuffer. A driver that has the image only as an external texture, which GLES 2
 * cannot attach to a framebuffer, gets it as a renderbuffer (GL_OES_EGL_image). The framebuffer, texture and
 * renderbuffer bindings and the pack alignment are restored, so this would work in any context.
 *
 * Row order: glReadPixels() returns the rows from framebuffer y = 0 up, and y of a texture attachment is the texel
 * row t, so the first row read is texel row 0, the buffer's first row in memory. That is the top row as the frame is
 * shown: the engine wraps external textures with kTopLeft_GrSurfaceOrigin (flutter engine,
 * shell/platform/embedder/embedder_external_texture_gl.cc, ResolveTextureSkia()), and WebKit's DMA-BUF frames have
 * their first row at the top, as a Wayland buffer. So the rows are not flipped: a frame that is upside down in the
 * file is upside down in the buffer.
 */
static uint8_t *read_frame_pixels(struct plugin *p, const uc_webview_frame *f, const char **how)
{
    struct egl_current previous = get_current();
    GLint previous_framebuffer = 0, previous_texture = 0, previous_renderbuffer = 0, previous_pack = 4;
    GLuint framebuffer = 0, texture = 0, renderbuffer = 0;
    GLenum status, error;
    uint8_t *pixels = NULL;

    if (!make_own_context_current(p)) {
        LOG("frame dump: no GL context of the plugin's own");
        restore_current(p, &previous);
        return NULL;
    }
    clear_gl_errors();
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_renderbuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack);

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    texture = new_texture(GL_TEXTURE_2D);
    p->image_target_texture(GL_TEXTURE_2D, (GLeglImageOES) f->egl_image);
    *how = "a GL texture";
    status = GL_FRAMEBUFFER_UNSUPPORTED;
    if (glGetError() == GL_NO_ERROR) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }
    if (status != GL_FRAMEBUFFER_COMPLETE && p->image_target_renderbuffer) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        clear_gl_errors();
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        p->image_target_renderbuffer(GL_RENDERBUFFER, (GLeglImageOES) f->egl_image);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        status = glGetError() == GL_NO_ERROR ? glCheckFramebufferStatus(GL_FRAMEBUFFER) : GL_FRAMEBUFFER_UNSUPPORTED;
        *how = "a GL renderbuffer";
    }

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG("frame dump: the frame's EGLImage cannot be read through a framebuffer (status 0x%x)", (unsigned) status);
    } else if (!(pixels = malloc((size_t) f->width * 4 * (size_t) f->height))) {
        LOG("frame dump: out of memory");
    } else {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, f->width, f->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if ((error = glGetError()) != GL_NO_ERROR) {
            LOG("frame dump: glReadPixels failed (0x%x)", (unsigned) error);
            free(pixels);
            pixels = NULL;
        }
    }

    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) previous_framebuffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint) previous_texture);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint) previous_renderbuffer);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    if (renderbuffer)
        glDeleteRenderbuffers(1, &renderbuffer);
    restore_current(p, &previous);
    return pixels;
}

/* Platform thread: writes frame as the next frame-<n>.ppm of the dump directory. */
static void dump_frame(struct plugin *p, const struct view *view, const struct frame *frame)
{
    const uc_webview_frame *f = &frame->f;
    char path[UCFL_DUMP_PATH_MAX], source[96];
    char fourcc[5] = { (char) (f->fourcc & 0xff), (char) ((f->fourcc >> 8) & 0xff), (char) ((f->fourcc >> 16) & 0xff),
                       (char) (f->fourcc >> 24), '\0' };
    uint8_t *pixels = NULL;
    const char *how = "";
    int err;

    if (f->egl_image) {
        pixels = read_frame_pixels(p, f, &how);
        if (!pixels)
            return;
        ucfl_dump_next_path(&p->dump, path, sizeof(path));
        err = ucfl_write_ppm(path, pixels, f->width, f->height, (size_t) f->width * 4, UCFL_PIXELS_RGBA, false);
        snprintf(source, sizeof(source), "DMA-BUF %s modifier 0x%llx, read as %s", fourcc,
                 (unsigned long long) f->modifier, how);
    } else if (f->pixels) {
        /* ARGB8888 little endian, top row first (uc_webview.h, uc_webview_frame) */
        ucfl_dump_next_path(&p->dump, path, sizeof(path));
        err = ucfl_write_ppm(path, f->pixels, f->width, f->height, (size_t) f->stride, UCFL_PIXELS_BGRA, false);
        snprintf(source, sizeof(source), "shared memory %s", fourcc);
    } else {
        return;
    }
    free(pixels);
    if (err)
        LOG("frame dump to %s failed: %s", path, strerror(err));
    else
        LOG("frame dumped: %s %dx%d (view %lld, %s)", path, f->width, f->height, (long long) view->id, source);
}

static int on_dump_timer(sd_event_source *source, int fd, uint32_t revents, void *userdata)
{
    struct plugin *p = userdata;
    uint64_t expirations;
    int error, dumped = 0;

    (void) revents;
    if (read(fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
        LOG("frame dumps: reading the timer failed: %s", strerror(errno));
    if (!ucfl_dump_triggered(&p->dump, &error)) {
        if (error) {
            LOG("frame dumps: %s/" UCFL_DUMP_TRIGGER " cannot be deleted (%s): the trigger is off", p->dump.dir,
                strerror(error));
            sd_event_source_set_enabled(source, SD_EVENT_OFF);
        }
        return 0;
    }
    for (size_t i = 0; i < p->views.count; i++) {
        struct view *view = p->views.slots[i].data;

        if (view->wv && view->latest) {
            dump_frame(p, view, view->latest);
            dumped++;
        }
    }
    if (!dumped)
        LOG("frame dump: no view has a frame");
    return 0;
}

/* Platform thread, plugin_init(): dumps on when UC_WEBVIEW_DUMP_DIR is set. */
static void start_dumps(struct plugin *p)
{
    const char *dir = getenv("UC_WEBVIEW_DUMP_DIR");
    const long interval_ns = UCFL_DUMP_POLL_MS * 1000000L;
    const struct itimerspec every = { .it_interval = { 0, interval_ns }, .it_value = { 0, interval_ns } };
    int err;

    p->dump_fd = -1;
    err = ucfl_dump_init(&p->dump, dir);
    if (!ucfl_dump_on(&p->dump)) {
        if (err)
            LOG("frame dumps off: UC_WEBVIEW_DUMP_DIR: %s", strerror(err));
        return;
    }
    if (err)
        LOG("frame dumps: creating %s failed: %s", p->dump.dir, strerror(err));
    if (p->renderer)
        p->image_target_renderbuffer = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC) gl_renderer_get_proc_address(
            p->renderer, "glEGLImageTargetRenderbufferStorageOES");

    p->dump_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (p->dump_fd < 0 || timerfd_settime(p->dump_fd, 0, &every, NULL) != 0)
        err = errno;
    else
        err = flutterpi_sd_event_add_io(&p->dump_source, p->dump_fd, EPOLLIN, on_dump_timer, p);
    if (err) {
        LOG("frame dumps: no timer for the trigger (%s): only the first frame is dumped", strerror(err));
        if (p->dump_fd >= 0)
            close(p->dump_fd);
        p->dump_fd = -1;
        p->dump_source = NULL;
        return;
    }
    LOG("frame dumps to %s: the first frame, and the newest frame of each view after `touch %s/" UCFL_DUMP_TRIGGER "`",
        p->dump.dir, p->dump.dir);
}

static void stop_dumps(struct plugin *p)
{
    if (p->dump_source) {
        sd_event_source_set_enabled(p->dump_source, SD_EVENT_OFF);
        sd_event_source_unref(p->dump_source);
        p->dump_source = NULL;
    }
    if (p->dump_fd >= 0)
        close(p->dump_fd);
    p->dump_fd = -1;
}

/* ---- platform thread: frame pacing (core/ucfl_pacing.h) ------------------------ */

/* a frame the engine never got: back to the library at once (which implies rendered) */
static void release_unshown(struct view *view, struct frame *frame)
{
    struct plugin *p = view->plugin;

    if (view->wv)
        p->lib->frame_release(view->wv, frame->f.handle);
    p->frames_released++;
    view_unref(view);
    free(frame);
}

static void report_rendered(struct view *view, struct frame *frame)
{
    if (frame->rendered)
        return;
    frame->rendered = true;
    if (view->wv) {
        view->plugin->lib->frame_rendered(view->wv, frame->f.handle);
        view->reported_ns = now_ns();
    }
}

/* hands frame to the engine; false if the texture refused it */
static bool push_frame(struct view *view, struct frame *frame)
{
    struct plugin *p = view->plugin;
    const uc_webview_frame *f = &frame->f;
    bool zero_copy = f->egl_image != NULL;

    if (!view->had_frame || f->width != view->frame_width || f->height != view->frame_height ||
        zero_copy != view->frame_zero_copy) {
        struct ucfl_writer w;
        ucfl_writer_init(&w);
        ucfl_put_event_frame(&w, view->id, f->width, f->height, zero_copy);
        ucfl_host_send_event(p->host, &w);
        view->had_frame = true;
        view->frame_width = f->width;
        view->frame_height = f->height;
        view->frame_zero_copy = zero_copy;
    }

    if (texture_push_unresolved_frame(view->texture, &(struct unresolved_texture_frame) {
                                                         .resolve = resolve_frame,
                                                         .destroy = on_unresolved_frame_destroy,
                                                         .userdata = frame,
                                                     }) != 0)
        return false;
    frame->pushed_ns = now_ns();
    view->latest = frame;
    if (ucfl_dump_first(&p->dump))
        dump_frame(p, view, frame);
    return true;
}

/* Platform thread, view not disposed: carries out what the pacer decided, in the order of struct ucfl_pace. */
static void apply_pace(struct view *view, struct ucfl_pace pace)
{
    if (pace.release)
        release_unshown(view, pace.release);
    if (pace.push && !push_frame(view, pace.push)) {
        ucfl_pacer_gone(&view->pacer, pace.push);
        release_unshown(view, pace.push);
        return;
    }
    if (pace.rendered)
        report_rendered(view, pace.rendered);
}

/* ---- platform thread: the surfaces (struct ucfl_host_ops) ------------------------- */

/* surface_frame: frame_ready() of the view's library callback, from the GLib dispatch */
static void surface_frame(void *ctx, void *surface, const uc_webview_frame *f)
{
    struct plugin *p = ctx;
    struct view *view = surface;
    struct frame *frame = calloc(1, sizeof(*frame));

    if (!frame) {
        p->lib->frame_release(view->wv, f->handle);
        return;
    }
    frame->view = view;
    frame->f = *f;
    frame->fetched.frame = frame;
    frame->released.frame = frame;
    frame->released.released = true;
    view->refs++;
    p->frames_ready++;

    ucfl_delay_add(&view->clock.turnaround, view->reported_ns, now_ns());
    view->reported_ns = 0;
    /* a report from in here completes after this callback (uc_webview_frame_rendered()) */
    apply_pace(view, ucfl_pacer_ready(&view->pacer, frame));
    if (view->pacer.held == frame)
        p->frames_held++;
}

/* surface_hold: setFrameHold */
static void surface_hold(void *ctx, void *surface, bool hold)
{
    struct view *view = surface;

    (void) ctx;
    if (hold != view->pacer.hold)
        apply_pace(view, ucfl_pacer_set_hold(&view->pacer, hold));
}

/* surface_new: a flutter-pi texture for the view */
static int surface_new(void *ctx, int64_t view_id, uc_webview *wv, double refresh_rate, int64_t *texture_id,
                       void **surface)
{
    struct plugin *p = ctx;
    struct view *view = calloc(1, sizeof(*view));

    if (!view)
        return -1;
    view->plugin = p;
    view->id = view_id;
    view->wv = wv;
    view->refs = 1;
    ucfl_pacer_init(&view->pacer, p->pacing);
    ucfl_frame_clock_init(&view->clock, refresh_rate);
    view->texture = flutterpi_create_texture(p->flutterpi);
    if (!view->texture) {
        free(view);
        return -1;
    }
    /* the surfaces keep the core's ids, for the stats and the frame dumps */
    if (ucfl_views_insert(&p->views, view_id, view) != 0) {
        texture_destroy(view->texture);
        free(view);
        return -1;
    }
    p->lib = ucfl_host_lib(p->host);
    *texture_id = texture_get_id(view->texture);
    *surface = view;
    return 0;
}

/* surface_free: right before uc_webview_free(), which releases the frames the plugin still holds */
static void surface_free(void *ctx, void *surface)
{
    struct plugin *p = ctx;
    struct view *view = surface;
    struct frame *held;

    /* the texture first: afterwards the engine fetches none of its frames, so none is bound after the library freed
     * the EGLImages; a frame the engine still shows keeps its texture (an EGLImage's siblings outlive the image) */
    texture_destroy(view->texture);
    view->texture = NULL;
    while ((held = ucfl_pacer_take_held(&view->pacer)))
        release_unshown(view, held);
    ucfl_views_remove(&p->views, view->id);
    view->wv = NULL;
    view_unref(view);
}

/* ---- platform thread: the GLib loop and the stats (struct ucfl_host_ops) ----------- */

static int prepare_init(void *ctx, char *reason, size_t reason_size)
{
    struct plugin *p = ctx;

    if (!p->glib)
        p->glib = ucfp_glib_loop_new(flutterpi_sd_event_add_io, flutterpi_post_platform_task, reason, reason_size);
    return p->glib ? 0 : -1;
}

static void after_call(void *ctx)
{
    struct plugin *p = ctx;

    /* the library may have attached sources from this thread, which does not wake the context */
    ucfp_glib_loop_rearm(p->glib);
}

static size_t adapter_stats(void *ctx, struct ucfl_stat *out, size_t max)
{
    struct plugin *p = ctx;
    struct ucfp_glib_stats glib;
    int64_t now = now_ns(), fps = 0;
    uint64_t gaps = 0;
    size_t n;

    ucfp_glib_loop_get_stats(p->glib, &glib);
    for (size_t i = 0; i < p->views.count; i++) {
        struct view *view = p->views.slots[i].data;
        int64_t view_fps = ucfl_frame_clock_milli_fps(&view->clock, now);

        if (view_fps > fps)
            fps = view_fps; /* the view that draws */
        gaps += view->clock.gaps;
        ucfl_delay_take(&p->turnaround, &view->clock.turnaround);
        ucfl_delay_take(&p->to_fetch, &view->clock.to_fetch);
    }
    const struct ucfl_stat stats[] = {
        { "framesReady", (int64_t) p->frames_ready },
        { "framesFetched", (int64_t) p->frames_fetched },
        { "framesReleased", (int64_t) p->frames_released },
        { "framesHeld", (int64_t) p->frames_held },
        { "pacingAhead", p->pacing == UCFL_PACING_AHEAD },
        { "monotonicMs", now / 1000000 },
        { "fetchMilliFps", fps },
        { "fetchGaps", (int64_t) gaps },
        { "webTurnaroundAvgUs", ucfl_delay_avg_us(&p->turnaround) },
        { "webTurnaroundMaxUs", p->turnaround.max_ns / 1000 },
        { "pushToFetchAvgUs", ucfl_delay_avg_us(&p->to_fetch) },
        { "pushToFetchMaxUs", p->to_fetch.max_ns / 1000 },
        { "glibDispatches", (int64_t) glib.dispatches },
        { "glibMaxDispatchUs", (int64_t) glib.max_dispatch_us },
        { "glibTotalDispatchUs", (int64_t) glib.total_dispatch_us },
        { "glibSlowDispatches", (int64_t) glib.slow_dispatches },
        { "glibEpollRebuilds", (int64_t) glib.epoll_rebuilds },
    };
    n = sizeof(stats) / sizeof(stats[0]) < max ? sizeof(stats) / sizeof(stats[0]) : max;
    memcpy(out, stats, n * sizeof(stats[0]));
    memset(&p->turnaround, 0, sizeof(p->turnaround));
    memset(&p->to_fetch, 0, sizeof(p->to_fetch));
    return n;
}

static const struct ucfl_host_ops host_ops = {
    .send_event = send_event,
    .respond = respond,
    .log = log_line,
    .prepare_init = prepare_init,
    .surface_new = surface_new,
    .surface_frame = surface_frame,
    .surface_hold = surface_hold,
    .surface_free = surface_free,
    .after_call = after_call,
    .stats = adapter_stats,
};

/* ---- platform thread: the channels ---------------------------------------- */

static void on_control_message(void *userdata, const FlutterPlatformMessage *message)
{
    struct plugin *p = userdata;

    ucfl_host_handle_control(p->host, message->message, message->message_size, (void *) message->response_handle);
}

static void on_events_message(void *userdata, const FlutterPlatformMessage *message)
{
    struct plugin *p = userdata;

    ucfl_host_handle_events(p->host, message->message, message->message_size, (void *) message->response_handle);
}

/* ---- registration ---------------------------------------------------------- */

static enum plugin_init_result plugin_init(struct flutterpi *flutterpi, void **userdata_out)
{
    struct plugin *p = &plugin_instance;
    struct plugin_registry *registry = flutterpi_get_plugin_registry(flutterpi);
    const char *library = getenv("UC_WEBVIEW_LIBRARY"); /* a test build of the library; default the installed one */
    const char *data_dir = getenv("UC_WEBVIEW_DATA_DIR");
    struct ucfl_host_config config = { 0 };

    memset(p, 0, sizeof(*p));
    p->flutterpi = flutterpi;
    p->egl_display = EGL_NO_DISPLAY;
    p->egl_context = EGL_NO_CONTEXT;
    p->dump_fd = -1;
    ucfl_views_init(&p->views);
    if (ucfl_pacing_parse(getenv("UC_WEBVIEW_FRAME_PACING"), &p->pacing) != 0)
        LOG("UC_WEBVIEW_FRAME_PACING: unknown value (fetch, ahead): %s pacing", ucfl_pacing_name(p->pacing));
    pthread_mutex_init(&p->done_lock, NULL);
    p->done_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (p->done_fd < 0 || flutterpi_sd_event_add_io(&p->done_source, p->done_fd, EPOLLIN, on_done, p) != 0) {
        LOG("no eventfd for the frames: %s", strerror(errno));
        if (p->done_fd >= 0)
            close(p->done_fd);
        return PLUGIN_INIT_RESULT_ERROR;
    }

    if (flutterpi_has_gl_renderer(flutterpi)) {
        p->renderer = flutterpi_get_gl_renderer(flutterpi);
        p->egl_display = gl_renderer_get_egl_display(p->renderer);
        p->image_target_texture =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) gl_renderer_get_proc_address(p->renderer, "glEGLImageTargetTexture2DOES");
        p->has_bgra = gl_renderer_supports_gl_extension(p->renderer, "GL_EXT_texture_format_BGRA8888");
    }
    config.library_path = library && *library ? library : UC_WEBVIEW_SONAME;
    config.egl_display = p->egl_display;
    config.data_dir = data_dir && *data_dir ? data_dir : NULL;
    if (!p->renderer)
        config.unavailable = "flutter-pi renders without OpenGL ES: web views need GL textures";
    else if (!p->image_target_texture)
        config.unavailable = "glEGLImageTargetTexture2DOES is missing";
    p->host = ucfl_host_new(&host_ops, p, &config);
    if (!p->host) {
        LOG("out of memory");
        sd_event_source_unref(p->done_source);
        close(p->done_fd);
        return PLUGIN_INIT_RESULT_ERROR;
    }
    LOG("%s frame pacing", ucfl_pacing_name(p->pacing));
    start_dumps(p);

    if (plugin_registry_set_receiver_v2_locked(registry, UCFL_CHANNEL_CONTROL, on_control_message, p) != 0 ||
        plugin_registry_set_receiver_v2_locked(registry, UCFL_CHANNEL_EVENTS, on_events_message, p) != 0) {
        plugin_registry_remove_receiver_v2_locked(registry, UCFL_CHANNEL_CONTROL);
        ucfl_host_free(p->host);
        p->host = NULL;
        stop_dumps(p);
        sd_event_source_unref(p->done_source);
        close(p->done_fd);
        return PLUGIN_INIT_RESULT_ERROR;
    }
    *userdata_out = p;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

static void plugin_deinit(struct flutterpi *flutterpi, void *userdata)
{
    struct plugin *p = userdata;
    struct plugin_registry *registry = flutterpi_get_plugin_registry(flutterpi);
    struct ucfp_glib_stats glib;

    plugin_registry_remove_receiver_v2_locked(registry, UCFL_CHANNEL_CONTROL);
    plugin_registry_remove_receiver_v2_locked(registry, UCFL_CHANNEL_EVENTS);
    /* every view (surface_free, uc_webview_free()); replies that still wait are dropped with the engine */
    ucfl_host_free(p->host);
    p->host = NULL;
    ucfl_views_free(&p->views);

    ucfp_glib_loop_get_stats(p->glib, &glib);
    if (glib.dispatches)
        LOG("GLib dispatches: %llu, longest %llu us, %llu longer than %u us, %llu epoll sets renewed, frames %llu ready, "
            "%llu fetched, %llu released, %llu waited",
            (unsigned long long) glib.dispatches, (unsigned long long) glib.max_dispatch_us,
            (unsigned long long) glib.slow_dispatches, UCFP_GLIB_SLOW_US, (unsigned long long) glib.epoll_rebuilds,
            (unsigned long long) p->frames_ready,
            (unsigned long long) p->frames_fetched, (unsigned long long) p->frames_released,
            (unsigned long long) p->frames_held);
    ucfp_glib_loop_destroy(p->glib);
    p->glib = NULL;
    stop_dumps(p);

    /* frames the engine still holds post to a closed list from now on (the instance stays, see plugin_instance) */
    sd_event_source_set_enabled(p->done_source, SD_EVENT_OFF);
    sd_event_source_unref(p->done_source);
    pthread_mutex_lock(&p->done_lock);
    close(p->done_fd);
    p->done_fd = -1;
    pthread_mutex_unlock(&p->done_lock);
    if (p->egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(p->egl_display, p->egl_context);
}

FLUTTERPI_PLUGIN("uc_webview", uc_webview, plugin_init, plugin_deinit)
