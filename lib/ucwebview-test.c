/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Unfolded Circle ApS
 *
 * ucwebview-test: command line host for libucwebview and the test oracle of
 * its validation on the device. It is a plain consumer of uc_webview.h (no
 * WPE headers) that
 *   - creates an EGLDisplay on an EGL device (EGL_EXT_platform_device, the
 *     GPU's render node on the device, Mesa's software device on a build host)
 *     or the surfaceless platform, like Qt's eglfs would own one,
 *   - runs uc_webview_global_init(), loads a URL or an HTML file, receives
 *     frames, reads them back through a GLES2 FBO (DMA-BUF frames) or from the
 *     shared memory pixels, prints size/fourcc/modifier/sha256 per frame and
 *     writes the last frame as PNG,
 *   - optionally resizes, taps, soaks (--loop) and exercises the IME path,
 *   - reports a display refresh rate like a host (--refresh-rate) and checks
 *     the frame pacing with a requestAnimationFrame counter page (--raf-test),
 *   - zooms a page step by step and prints how long each step took to render
 *     (--zoom-test, uc_webview_when_rendered()),
 *   - magnifies a page at a focal point (--magnify) and pinches it with two
 *     synthesized fingers through uc_webview_touch() like a host (--pinch),
 *     reporting the pinch callbacks, the render time, the final magnification,
 *     whether the focal point stayed and whether a tap or scroll reached the
 *     page during the gesture (library 0.6.0),
 *   - answers the page's requests and checks them against pages of its own
 *     HTTP server (--dialog-test, --auth-test, --window-test,
 *     --download-test; library 0.7.0, ucwebview-test-requests.c);
 *   - taps links, a checkbox and window.open()/window.close() buttons of a
 *     page of its own HTTP server like remote-ui and checks what each tap
 *     did (--link-test, ucwebview-test-links.c);
 *   - checks the IME focus reports across a navigation to another site, a
 *     web process swap (--ime-swap-test, library 0.7.5, ucwebview-test-ime.c);
 *   - checks the site settings against pages of servers of its own: user
 *     agent, cookies and their persistence, origin headers, certificate
 *     exceptions, color scheme, clearing website data (--site-test, library
 *     0.8.0, ucwebview-test-site.c);
 *   - checks the device state API of a page: the state it starts with,
 *     changes, suspend and resume, invalid input, reload, the hidden page
 *     throttling, the API off and on (--device-test, library 0.9.0,
 *     ucwebview-test-device.c);
 *   - checks the reason of failed loads, one load_failed_detail() right
 *     before each FAILED and none for loads without FAILED
 *     (--load-fail-test, library 0.10.0, ucwebview-test-loadfail.c); every
 *     mode prints the reason of a failed load ("load failed: ...");
 *   - checks the web processes WebKit keeps after the last view is freed
 *     with the library's cache model, and the back/forward cache of a view
 *     across two sites (--process-test, library 0.10.4,
 *     ucwebview-test-process.c).
 * Exit codes: 0 ok, 1 load/web process failure (also a URL the library
 * refuses to show: a download, a PDF), IME mismatch, a zoom step that did not
 * render, a failed magnify/pinch check, a failed request, link or IME swap test,
 * 2 timeout or EGL setup failure, 3 bad command line.
 *
 * Its output follows the library's logging rules (package README, "What the
 * logs carry"): URIs without credentials, query and fragment unless
 * UC_WEBVIEW_LOG_FULL_URLS=1 (uc_uri_policy.c is compiled in), and never the
 * text, length or caret offsets of an editable element.
 */
#include "uc_uri_policy.h"
#include "uc_webview.h"
#include "ucwebview-test-requests.h"
#include "ucwebview-test-links.h"
#include "ucwebview-test-ime.h"
#include "ucwebview-test-process.h"
#include "ucwebview-test-site.h"
#include "ucwebview-test-device.h"
#include "ucwebview-test-loadfail.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <glib.h>
#include <glib-unix.h>
#include <math.h>   /* NAN, a constant: no libm */
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- options ----------------------------------------------------------------- */

static gboolean opt_selftest;
static char *opt_url;
static char *opt_html;
static char *opt_out;
static int opt_frames = 3;
static int opt_timeout = -1;   /* -1: 30 s, or --loop + 30 s when soaking */
static char *opt_size;
static double opt_scale = 1.0;
static char *opt_resize;
static int opt_loop;
static char *opt_egl = NULL; /* device | surfaceless | none */
static char *opt_tap;
static gboolean opt_ime_test;
static char *opt_data_dir;
static gboolean opt_verbose;
static gboolean opt_version;
static gboolean opt_allow_file_urls;
static double opt_refresh_rate;   /* 0: not given */
static int opt_raf_test;          /* seconds to measure, 0: off */
static gboolean opt_plain_new;    /* uc_webview_new() instead of uc_webview_new_sized() */
static char *opt_zoom_test;       /* zoom steps in percent, NULL: off */
static char *opt_magnify;         /* "S[@X,Y]", NULL: off */
static char *opt_pinch;           /* "FROM:TO[@X,Y]" finger distances in px, NULL: off */
static gboolean opt_prevent_touch; /* the page's touch listeners call preventDefault() (a map) */

static const GOptionEntry entries[] = {
    { "selftest", 0, 0, G_OPTION_ARG_NONE, &opt_selftest, "Check the C API without WPE (version, NULL handling) and exit", NULL },
    { "headless", 0, 0, G_OPTION_ARG_STRING, &opt_url, "Load URL, wait for frames, dump the last one", "URL" },
    { "html", 0, 0, G_OPTION_ARG_FILENAME, &opt_html, "Load this HTML file instead of a URL", "FILE" },
    { "out", 0, 0, G_OPTION_ARG_FILENAME, &opt_out, "Write the last frame as RGBA PNG", "frame.png" },
    { "frames", 0, 0, G_OPTION_ARG_INT, &opt_frames, "Number of frames to receive before exiting (default 3)", "N" },
    { "timeout", 0, 0, G_OPTION_ARG_INT, &opt_timeout, "Give up after S seconds without reaching the goal (default 30, or --loop + 30)", "S" },
    { "size", 0, 0, G_OPTION_ARG_STRING, &opt_size, "View size in pixels (default 480x800)", "WxH" },
    { "scale", 0, 0, G_OPTION_ARG_DOUBLE, &opt_scale, "Device scale factor (default 1.0)", "F" },
    { "resize", 0, 0, G_OPTION_ARG_STRING, &opt_resize, "Resize to each size after a frame arrived", "WxH,WxH" },
    { "loop", 0, 0, G_OPTION_ARG_INT, &opt_loop, "Soak: keep rendering for S seconds, print frame count and VmRSS", "S" },
    { "egl", 0, 0, G_OPTION_ARG_STRING, &opt_egl, "EGLDisplay: device (default), surfaceless, none (shared memory frames)", "MODE" },
    { "tap", 0, 0, G_OPTION_ARG_STRING, &opt_tap, "Send a touch tap at X,Y after the first frame", "X,Y" },
    { "ime-test", 0, 0, G_OPTION_ARG_NONE, &opt_ime_test, "Load a page with an <input>, tap it, type through the IME API, check the result", NULL },
    { "data-dir", 0, 0, G_OPTION_ARG_FILENAME, &opt_data_dir, "Website data directory (default: ephemeral)", "DIR" },
    { "allow-file-urls", 0, 0, G_OPTION_ARG_NONE, &opt_allow_file_urls,
      "Allow file: URLs like UC_WEBVIEW_ALLOW_FILE_URLS=1 (the library blocks them by default)", NULL },
    { "refresh-rate", 0, 0, G_OPTION_ARG_DOUBLE, &opt_refresh_rate,
      "Display refresh rate in Hz (20..240) passed like a host does; UC_WEBVIEW_REFRESH_RATE wins (default 60)", "HZ" },
    { "raf-test", 0, 0, G_OPTION_ARG_INT, &opt_raf_test,
      "Load a requestAnimationFrame counter page, measure for S seconds after 1 s warm-up, pass when the page's "
      "rate matches the display refresh rate", "S" },
    { "plain-new", 0, 0, G_OPTION_ARG_NONE, &opt_plain_new,
      "Create the view with uc_webview_new() (the API version 1 callbacks, like hosts built against a 0.3 header) "
      "instead of uc_webview_new_sized()", NULL },
    { "zoom-test", 0, 0, G_OPTION_ARG_STRING, &opt_zoom_test,
      "Zoom the page (URL, --html, or a large internal page) through these levels in percent, one step after the "
      "other, and print how long each took to render (uc_webview_when_rendered()); a step a+b+c sets its levels back "
      "to back and waits once", "110,125,150" },
    { "magnify", 0, 0, G_OPTION_ARG_STRING, &opt_magnify,
      "Set the magnification S at the focal point X,Y (default: the view centre) one second after the page (URL, "
      "--html or an internal grid page) loaded; print the render time, the result and whether the focal point stayed",
      "S[@X,Y]" },
    { "pinch", 0, 0, G_OPTION_ARG_STRING, &opt_pinch,
      "Pinch with two synthesized fingers from FROM to TO px apart around X,Y (default: the view centre) in 500 ms, "
      "then magnify like a host; print the pinch callbacks, the render time, the result and whether a tap or scroll "
      "reached the page", "FROM:TO[@X,Y]" },
    { "prevent-touch", 0, 0, G_OPTION_ARG_NONE, &opt_prevent_touch,
      "With --pinch: the page's touch listeners call preventDefault() like a map; the library must not pinch", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Debug output", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print the version and exit", NULL },
    { NULL }
};

/* ---- state ----------------------------------------------------------------------- */

/* what the page's counter script reports (counters_js) */
typedef struct {
    int click, scroll, touch;
    double scale, left, top, max_left, max_top;
} PageState;

typedef struct {
    GMainLoop *loop;
    uc_webview *wv;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
    GLuint texture, fbo;
    int width, height;
    int frames;                 /* frame_ready() count */
    int rendered, released;     /* counters for the leak check */
    uc_webview_frame held;      /* previous frame, released when the next one arrives */
    gboolean has_held;
    guchar *last_rgba;          /* readback of the last frame */
    int last_w, last_h;
    char *last_sha;
    gint64 start_us, last_frame_us;
    GPtrArray *resizes;         /* "WxH" strings still to apply */
    guint timeout_source;
    gboolean tapped;
    int exit_code;
    /* ime test */
    int ime_focus_count;
    gboolean ime_typed;
    gboolean ime_seen_preedit;  /* the page reported the composition text ("abz") */
    char *last_title;
    guint ime_settle_source;
    /* raf test */
    GArray *raf_rates;          /* double: idle-window rates the page reported during the measurement */
    GArray *raf_moving;         /* double: moving-window rates */
    GArray *raf_sds;            /* double: interval standard deviations (ms) of the idle windows */
    int raf_long;               /* long intervals of the idle windows */
    gboolean raf_measuring;
    int raf_frames_start;
    gint64 raf_start_us;
    /* zoom test */
    char **zoom_steps;          /* "125" or "150+175+200" */
    guint zoom_index;           /* the step running or next */
    gboolean zoom_started;
    gint64 zoom_t0;
    int zoom_frames_start;
    GArray *zoom_ms;            /* double: render time of every step */
    int zoom_timeouts, zoom_cancelled;
    /* magnify and pinch tests (library 0.6.0) */
    gboolean gesture_started;
    guint state_seq;            /* the page state request waited for */
    GSourceFunc state_next;     /* runs when the page reported state_seq */
    PageState state[4];         /* 0: before, 1: after the gesture, 2: after the magnification */
    int state_slot;
    double magnify_scale, focal_x, focal_y, magnification0;
    gint64 magnify_t0;
    int magnify_status;
    double magnify_ms;
    double pinch_from, pinch_to;
    int pinch_step;
    int pinch_begins, pinch_updates, pinch_ends, pinch_cancels;
    double pinch_begin_x, pinch_begin_y, pinch_scale, pinch_end_x, pinch_end_y;
    int magnification_changes;
    gboolean done;
} Host;

static const char ime_test_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
    "<style>body{margin:0;background:#fff}input{position:absolute;left:20px;top:20px;width:200px;height:40px;font-size:24px}</style>"
    "</head><body><input id=i type=text autocomplete=off>"
    "<script>var i=document.getElementById('i');"
    "i.addEventListener('input',function(){document.title='v:'+i.value;});"
    "i.addEventListener('focus',function(){document.title='focus';});"
    "</script></body></html>";

/* requestAnimationFrame counter, in windows of one second that alternate between "moving" (a box moves by 4 px per
 * callback, so every callback produces a frame) and "idle" (callbacks change nothing, so WebKit composites no frame
 * and the callbacks run at the bare display link rate, LayerTreeHost.cpp:216-217). Per window the page reports,
 * from the callbacks' own timestamps, the rate, the standard deviation of the intervals and the number of
 * intervals longer than 1.5 times their median, in the title ("raf:50.00 sd:0.31 long:0 mode:idle"), the body and
 * the console. A host that presents frames on a display (remote-ui) bounds the moving rate by its presentation;
 * the idle rate is always the display link's. */
static const char raf_test_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
    "<style>body{margin:0;background:#fff;font:28px sans-serif}"
    "#b{position:absolute;top:160px;left:0;width:60px;height:60px;background:#06c}</style>"
    "</head><body><div id=f>-</div><div id=b></div>"
    "<script>var t0=0,last=0,x=0,d=[],mode='moving',b=document.getElementById('b'),f=document.getElementById('f');"
    "function tick(t){if(last)d.push(t-last);last=t;if(!t0)t0=t;"
    "if(t-t0>=1000&&d.length){var s=0,q=0,i,m,med,lg=0;for(i=0;i<d.length;i++)s+=d[i];m=s/d.length;"
    "for(i=0;i<d.length;i++)q+=(d[i]-m)*(d[i]-m);med=d.slice().sort(function(a,c){return a-c;})[d.length>>1];"
    "for(i=0;i<d.length;i++)if(d[i]>1.5*med)lg++;"
    "var r=(d.length*1000/s).toFixed(2),sd=Math.sqrt(q/d.length).toFixed(2);"
    "document.title='raf:'+r+' sd:'+sd+' long:'+lg+' mode:'+mode;f.textContent=mode+': '+r+' fps, sd '+sd+' ms, long '+lg;"
    "console.log('raf '+mode+' '+r+' fps sd '+sd+' ms long '+lg);d=[];t0=t;mode=mode=='moving'?'idle':'moving';}"
    "if(mode=='moving'){x=(x+4)%400;b.style.transform='translateX('+x+'px)';}requestAnimationFrame(tick);}"
    "requestAnimationFrame(tick);</script></body></html>";

/* A long page with a table, built by its own script, to make a zoom step's relayout take measurable time without
 * a network: 2000 paragraphs with inline styles and a table of 300 x 6 cells. */
static const char zoom_test_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
    "<style>body{margin:8px;font:16px sans-serif;background:#fff}td{border:1px solid #888;padding:2px 4px}</style>"
    "</head><body><h1>zoom test</h1><div id=c></div>"
    "<script>var t='Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
    "ut labore et dolore magna aliqua. ',h=[],i,r,c;"
    "for(i=0;i<2000;i++)h.push('<p>'+i+' <b>'+t+'</b>'+t+'<i>'+t+'</i></p>');"
    "h.push('<table>');for(r=0;r<300;r++){h.push('<tr>');for(c=0;c<6;c++)h.push('<td>'+r+'.'+c+' '+t.substr(0,20)+"
    "'</td>');h.push('</tr>');}h.push('</table>');document.getElementById('c').innerHTML=h.join('');"
    "</script></body></html>";

/* A grid page for the magnify and pinch tests: 1200 x 3000 CSS px with a label every 200 x 100 px, so that a PNG shows
 * where the view is, and room to scroll both ways. No links: a tap must not navigate. */
static const char grid_test_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
    "<style>html,body{margin:0}body{position:relative;width:1200px;height:3000px;font:16px sans-serif;"
    "background:repeating-linear-gradient(45deg,#eee 0 20px,#ddd 20px 40px)}"
    "div{position:absolute;color:#036}</style></head><body><script>var h=[],x,y;"
    "for(y=0;y<3000;y+=100)for(x=0;x<1200;x+=200)h.push('<div style=\"left:'+x+'px;top:'+y+'px\">'+x+','+y+'</div>');"
    "document.body.innerHTML=h.join('');</script></body></html>";

/* Counters in the page's own world for the magnify and pinch tests: clicks, scroll events and touch events (passive
 * listeners: the page leaves the touches to the browser; with --prevent-touch they call preventDefault() like a map
 * and WebKit cancels its gesture controller, PageClientImpl.cpp:248-258). __ucState(n) puts them with the visual
 * viewport into the title: "ucstate:<n> click:0 scroll:0 touch:0 scale:1 left:0 top:0 maxleft:720 maxtop:2200". */
static const char counters_js[] =
    "(function () {"
    " if (window.__ucCounters) return;"
    " var c = window.__ucCounters = { click: 0, scroll: 0, touch: 0 }, prevent = %d;"
    " addEventListener ('click', function () { c.click++; }, true);"
    " addEventListener ('scroll', function () { c.scroll++; }, { passive: true, capture: true });"
    " ['touchstart', 'touchmove', 'touchend', 'touchcancel'].forEach (function (t) {"
    "  addEventListener (t, function (e) { c.touch++; if (prevent) e.preventDefault (); },"
    "   { passive: !prevent, capture: true }); });"
    " window.__ucState = function (n) {"
    "  var v = visualViewport, s = document.scrollingElement || document.documentElement;"
    "  document.title = 'ucstate:' + n + ' click:' + c.click + ' scroll:' + c.scroll + ' touch:' + c.touch"
    "   + ' scale:' + v.scale + ' left:' + v.pageLeft + ' top:' + v.pageTop"
    "   + ' maxleft:' + (s.scrollWidth - v.width) + ' maxtop:' + (s.scrollHeight - v.height); };"
    "}) ();";

static void
quit (Host *host, int code)
{
    if (host->done)
        return;
    host->done = TRUE;
    host->exit_code = code;
    g_main_loop_quit (host->loop);
}

static gboolean
parse_size (const char *text, int *w, int *h)
{
    return text != NULL && sscanf (text, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

static char *
read_vm_rss (void)
{
    char *status = NULL, *line, *result = g_strdup ("?");
    if (!g_file_get_contents ("/proc/self/status", &status, NULL, NULL))
        return result;
    line = strstr (status, "VmRSS:");
    if (line != NULL) {
        char *end = strchr (line, '\n');
        g_free (result);
        result = g_strstrip (g_strndup (line + 6, end != NULL ? (gsize) (end - line - 6) : strlen (line + 6)));
    }
    g_free (status);
    return result;
}

/* ---- EGL / GLES setup ------------------------------------------------------------- */

static gboolean
has_ext (const char *extensions, const char *name)
{
    size_t len = strlen (name);
    const char *p;
    if (extensions == NULL)
        return FALSE;
    for (p = extensions; (p = strstr (p, name)) != NULL; p += len)
        if ((p == extensions || p[-1] == ' ') && (p[len] == '\0' || p[len] == ' '))
            return TRUE;
    return FALSE;
}

static EGLDisplay
create_device_display (void)
{
    const char *client_ext = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);
    PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC) eglGetProcAddress ("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC query_string = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress ("eglQueryDeviceStringEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");
    EGLDeviceEXT devices[16];
    EGLint n = 0, chosen = -1;

    if (!has_ext (client_ext, "EGL_EXT_device_enumeration") || !has_ext (client_ext, "EGL_EXT_platform_device")
        || query_devices == NULL || query_string == NULL || get_display == NULL) {
        g_printerr ("ucwebview-test: EGL_EXT_device_enumeration/EGL_EXT_platform_device missing (client extensions: %s)\n",
                    client_ext != NULL ? client_ext : "(none)");
        return EGL_NO_DISPLAY;
    }
    if (!query_devices (G_N_ELEMENTS (devices), devices, &n) || n == 0) {
        g_printerr ("ucwebview-test: no EGL devices\n");
        return EGL_NO_DISPLAY;
    }
    for (EGLint i = 0; i < n; i++) {
        const char *ext = query_string (devices[i], EGL_EXTENSIONS);
        const char *render = has_ext (ext, "EGL_EXT_device_drm_render_node") ? query_string (devices[i], 0x3377 /* EGL_DRM_RENDER_NODE_FILE_EXT */) : NULL;
        const char *primary = has_ext (ext, "EGL_EXT_device_drm") ? query_string (devices[i], EGL_DRM_DEVICE_FILE_EXT) : NULL;
        g_print ("egl device %d: %s%s%s%s%s\n", i, ext != NULL ? ext : "", primary != NULL ? " primary=" : "", primary != NULL ? primary : "",
                 render != NULL && *render ? " render=" : "", render != NULL && *render ? render : "");
        /* prefer a device with a real render node (the GPU), else the first one (Mesa software device) */
        if (chosen < 0 || (render != NULL && *render != '\0' && g_file_test (render, G_FILE_TEST_EXISTS)
                           && !has_ext (ext, "EGL_MESA_device_software")))
            chosen = i;
    }
    g_print ("using egl device %d\n", chosen);
    return get_display (EGL_PLATFORM_DEVICE_EXT, devices[chosen], NULL);
}

static EGLDisplay
create_surfaceless_display (void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");
    const char *client_ext = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!has_ext (client_ext, "EGL_MESA_platform_surfaceless") || get_display == NULL) {
        g_printerr ("ucwebview-test: EGL_MESA_platform_surfaceless missing\n");
        return EGL_NO_DISPLAY;
    }
    return get_display (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
}

static gboolean
setup_egl (Host *host, const char *mode)
{
    EGLint major, minor;
    const char *ext;
    static const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig config = NULL;

    if (g_strcmp0 (mode, "none") == 0) {
        g_print ("egl: none (shared memory frames)\n");
        return TRUE;
    }
    host->egl_display = g_strcmp0 (mode, "surfaceless") == 0 ? create_surfaceless_display () : create_device_display ();
    if (host->egl_display == EGL_NO_DISPLAY)
        return FALSE;
    if (!eglInitialize (host->egl_display, &major, &minor)) {
        g_printerr ("ucwebview-test: eglInitialize failed: 0x%x\n", eglGetError ());
        return FALSE;
    }
    ext = eglQueryString (host->egl_display, EGL_EXTENSIONS);
    g_print ("egl %d.%d %s, dma-buf import: %s, modifiers: %s\n", major, minor, eglQueryString (host->egl_display, EGL_VENDOR),
             has_ext (ext, "EGL_EXT_image_dma_buf_import") ? "yes" : "NO",
             has_ext (ext, "EGL_EXT_image_dma_buf_import_modifiers") ? "yes" : "NO");

    if (!eglBindAPI (EGL_OPENGL_ES_API))
        return FALSE;
    if (has_ext (ext, "EGL_KHR_no_config_context") && has_ext (ext, "EGL_KHR_surfaceless_context")) {
        host->egl_context = eglCreateContext (host->egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
        host->egl_surface = EGL_NO_SURFACE;
    } else {
        static const EGLint cfg_attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
        static const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        EGLint n_configs = 0;
        if (!eglChooseConfig (host->egl_display, cfg_attribs, &config, 1, &n_configs) || n_configs == 0) {
            g_printerr ("ucwebview-test: no pbuffer EGLConfig\n");
            return FALSE;
        }
        host->egl_context = eglCreateContext (host->egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
        host->egl_surface = eglCreatePbufferSurface (host->egl_display, config, pbuf_attribs);
    }
    if (host->egl_context == EGL_NO_CONTEXT || !eglMakeCurrent (host->egl_display, host->egl_surface, host->egl_surface, host->egl_context)) {
        g_printerr ("ucwebview-test: cannot create/make current a GLES2 context: 0x%x\n", eglGetError ());
        return FALSE;
    }
    host->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress ("glEGLImageTargetTexture2DOES");
    if (host->image_target_texture == NULL) {
        g_printerr ("ucwebview-test: glEGLImageTargetTexture2DOES missing\n");
        return FALSE;
    }
    g_print ("gles: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    glGenTextures (1, &host->texture);
    glGenFramebuffers (1, &host->fbo);
    return TRUE;
}

/* ---- frame readback ---------------------------------------------------------------- */

/* Reads frame into a top-down RGBA buffer; alpha forced to 255 for XRGB buffers. */
static guchar *
read_frame (Host *host, const uc_webview_frame *frame)
{
    gsize size = (gsize) frame->width * frame->height * 4;
    guchar *rgba = g_malloc (size);
    gboolean opaque = frame->fourcc == 0x34325258u; /* XR24 */

    if (frame->egl_image != NULL) {
        GLenum status;
        glBindTexture (GL_TEXTURE_2D, host->texture);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        host->image_target_texture (GL_TEXTURE_2D, (GLeglImageOES) frame->egl_image);
        glBindFramebuffer (GL_FRAMEBUFFER, host->fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, host->texture, 0);
        status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            g_printerr ("ucwebview-test: FBO incomplete (0x%x), GL error 0x%x\n", status, glGetError ());
            memset (rgba, 0, size);
        } else {
            /* texel row 0 of a dma-buf is the top row; glReadPixels on an FBO returns texel order, no flip */
            glReadPixels (0, 0, frame->width, frame->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glBindTexture (GL_TEXTURE_2D, 0);
    } else if (frame->pixels != NULL) {
        /* ARGB8888 little endian = B,G,R,A bytes */
        for (int y = 0; y < frame->height; y++) {
            const guchar *src = (const guchar *) frame->pixels + (gsize) y * frame->stride;
            guchar *dst = rgba + (gsize) y * frame->width * 4;
            for (int x = 0; x < frame->width; x++, src += 4, dst += 4) {
                dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
            }
        }
    } else {
        memset (rgba, 0, size);
    }
    if (opaque)
        for (gsize i = 3; i < size; i += 4)
            rgba[i] = 255;
    return rgba;
}

static gboolean
write_png (const char *path, const guchar *rgba, int width, int height)
{
    FILE *fp = fopen (path, "wb");
    png_structp png;
    png_infop info;

    if (fp == NULL) {
        g_printerr ("ucwebview-test: cannot write %s\n", path);
        return FALSE;
    }
    png = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png_create_info_struct (png);
    if (setjmp (png_jmpbuf (png))) {
        png_destroy_write_struct (&png, &info);
        fclose (fp);
        return FALSE;
    }
    png_init_io (png, fp);
    png_set_IHDR (png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info (png, info);
    for (int y = 0; y < height; y++)
        png_write_row (png, (png_const_bytep) (rgba + (gsize) y * width * 4));
    png_write_end (png, NULL);
    png_destroy_write_struct (&png, &info);
    fclose (fp);
    return TRUE;
}

/* ---- callbacks ----------------------------------------------------------------------- */

static gboolean
do_tap_up (gpointer user_data)
{
    Host *host = user_data;
    int x, y;
    if (sscanf (opt_tap, "%d,%d", &x, &y) == 2)
        uc_webview_touch (host->wv, 0, UC_WEBVIEW_TOUCH_UP, x, y, 0);
    return G_SOURCE_REMOVE;
}

static void
maybe_tap (Host *host)
{
    int x, y;
    if (host->tapped || opt_tap == NULL || sscanf (opt_tap, "%d,%d", &x, &y) != 2)
        return;
    host->tapped = TRUE;
    g_print ("tap at %d,%d\n", x, y);
    uc_webview_touch (host->wv, 0, UC_WEBVIEW_TOUCH_DOWN, x, y, 0);
    g_timeout_add (60, do_tap_up, host);
}

static int
compare_doubles (gconstpointer a, gconstpointer b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static gboolean
raf_end (gpointer user_data)
{
    Host *host = user_data;
    /* idle windows run at the display link's rate: the display refresh rate plus the library's 1 % headroom
     * (uc_refresh_tick_mhz() in uc_refresh.c) */
    double display = uc_webview_global_get_refresh_rate ();
    double expected = display * 1.01;
    double elapsed = (g_get_monotonic_time () - host->raf_start_us) / 1e6;
    double frame_rate = elapsed > 0 ? (host->frames - host->raf_frames_start) / elapsed : 0.0;
    double median, tolerance = MAX (0.5, expected * 0.02);
    gboolean ok;

    if (host->raf_rates->len == 0) {
        g_printerr ("raf-test: the page reported no idle requestAnimationFrame rate in %d s (needs at least 2 s)\n",
                    opt_raf_test);
        quit (host, 1);
        return G_SOURCE_REMOVE;
    }
    g_array_sort (host->raf_rates, compare_doubles);
    g_array_sort (host->raf_moving, compare_doubles);
    g_array_sort (host->raf_sds, compare_doubles);
    median = g_array_index (host->raf_rates, double, host->raf_rates->len / 2);
    /* the idle windows measure WebKit's display link alone; the moving ones also how fast frames are produced */
    ok = median - expected <= tolerance && expected - median <= tolerance;
    g_print ("raf-test: idle requestAnimationFrame %.2f/s (median of %u windows, %.2f..%.2f, interval sd %.2f ms, "
             "%d long interval(s)), moving %.2f/s (median of %u windows), frames %.2f/s, display refresh rate "
             "%.3f Hz, display link %.3f Hz (tolerance %.2f): %s\n", median, host->raf_rates->len,
             g_array_index (host->raf_rates, double, 0),
             g_array_index (host->raf_rates, double, host->raf_rates->len - 1),
             g_array_index (host->raf_sds, double, host->raf_sds->len / 2), host->raf_long,
             host->raf_moving->len > 0 ? g_array_index (host->raf_moving, double, host->raf_moving->len / 2) : 0.0,
             host->raf_moving->len, frame_rate, display, expected, tolerance, ok ? "ok" : "MISMATCH");
    quit (host, ok ? 0 : 1);
    return G_SOURCE_REMOVE;
}

static gboolean
raf_begin (gpointer user_data)
{
    Host *host = user_data;
    /* after a second of warm-up (page load, first composites): count from here */
    host->raf_measuring = TRUE;
    host->raf_frames_start = host->frames;
    host->raf_start_us = g_get_monotonic_time ();
    g_array_set_size (host->raf_rates, 0);
    g_array_set_size (host->raf_moving, 0);
    g_array_set_size (host->raf_sds, 0);
    host->raf_long = 0;
    g_print ("raf-test: measuring for %d s at a display refresh rate of %.3f Hz\n", opt_raf_test,
             uc_webview_global_get_refresh_rate ());
    g_timeout_add ((guint) opt_raf_test * 1000, raf_end, host);
    return G_SOURCE_REMOVE;
}

/* ---- zoom test ------------------------------------------------------------------ */

static const char *const rendered_names[] = { "done", "timeout", "cancelled" };

/* "110,125+150+175,100": every level 1..2000 %; FALSE on anything else */
static gboolean
zoom_steps_valid (char **steps)
{
    if (steps == NULL || steps[0] == NULL)
        return FALSE;
    for (char **step = steps; *step != NULL; step++) {
        char **levels = g_strsplit (*step, "+", -1);
        gboolean ok = levels[0] != NULL;
        for (char **level = levels; ok && *level != NULL; level++) {
            char *end = NULL;
            guint64 percent = g_ascii_strtoull (*level, &end, 10);
            ok = end != *level && *end == '\0' && percent >= 1 && percent <= 2000;
        }
        g_strfreev (levels);
        if (!ok)
            return FALSE;
    }
    return TRUE;
}

static gboolean zoom_next (gpointer user_data);

static void
on_zoom_rendered (uc_webview *wv, int status, void *user)
{
    Host *host = user;
    double ms = (g_get_monotonic_time () - host->zoom_t0) / 1000.0;
    (void) wv;

    g_print ("zoom-test: %s %%: rendered after %.0f ms (%s), %d frame(s) since the zoom\n",
             host->zoom_steps[host->zoom_index], ms,
             status >= 0 && status <= 2 ? rendered_names[status] : "?", host->frames - host->zoom_frames_start);
    g_array_append_val (host->zoom_ms, ms);
    if (status == UC_WEBVIEW_RENDERED_TIMEOUT)
        host->zoom_timeouts++;
    else if (status != UC_WEBVIEW_RENDERED_DONE)
        host->zoom_cancelled++;
    host->zoom_index++;
    /* a moment between the steps, like a user pressing a key again */
    g_timeout_add (500, zoom_next, host);
}

static gboolean
zoom_next (gpointer user_data)
{
    Host *host = user_data;
    char **levels;

    if (host->zoom_steps[host->zoom_index] == NULL) {
        GArray *sorted = g_array_copy (host->zoom_ms);
        gboolean ok = host->zoom_timeouts == 0 && host->zoom_cancelled == 0;
        g_array_sort (sorted, compare_doubles);
        g_print ("zoom-test: %u step(s) rendered after %.0f..%.0f ms, median %.0f ms; %d timeout(s), "
                 "%d cancelled: %s\n",
                 sorted->len, g_array_index (sorted, double, 0), g_array_index (sorted, double, sorted->len - 1),
                 g_array_index (sorted, double, sorted->len / 2), host->zoom_timeouts, host->zoom_cancelled,
                 ok ? "ok" : "FAILED");
        g_array_unref (sorted);
        quit (host, ok ? 0 : 1);
        return G_SOURCE_REMOVE;
    }
    /* every level of the step at once, like key presses that WebKit gets before it rendered the first one */
    levels = g_strsplit (host->zoom_steps[host->zoom_index], "+", -1);
    host->zoom_t0 = g_get_monotonic_time ();
    host->zoom_frames_start = host->frames;
    for (char **level = levels; *level != NULL; level++)
        uc_webview_set_zoom (host->wv, g_ascii_strtoull (*level, NULL, 10) / 100.0);
    g_strfreev (levels);
    if (uc_webview_when_rendered (host->wv, on_zoom_rendered, host) != 0) {
        g_printerr ("zoom-test: uc_webview_when_rendered() refused the request\n");
        quit (host, 1);
    }
    return G_SOURCE_REMOVE;
}

static void
zoom_start (Host *host)
{
    if (host->zoom_started)
        return;
    host->zoom_started = TRUE;
    g_print ("zoom-test: page loaded, %u step(s) after a second\n", g_strv_length (host->zoom_steps));
    /* the page settles (first frames, late layout) before the first step */
    g_timeout_add (1000, zoom_next, host);
}

/* ---- magnify and pinch tests (library 0.6.0) ------------------------------------- */

#define PINCH_STEPS    25     /* moves of each finger */
#define PINCH_STEP_MS  20     /* 25 x 20 ms = 500 ms */

static double
abs_double (double v)
{
    return v < 0 ? -v : v;
}

/* "S[@X,Y]" or "A:B[@X,Y]": the numbers before the @ (one, or two with sep) and the point (default: the view centre) */
static gboolean
parse_gesture (const char *text, char sep, double *a, double *b, double *x, double *y, int width, int height)
{
    char **parts;
    char *end = NULL;
    gboolean ok;

    if (text == NULL)
        return FALSE;
    parts = g_strsplit (text, "@", 2);
    *x = width / 2.0;
    *y = height / 2.0;
    *a = g_ascii_strtod (parts[0], &end);
    ok = end != parts[0];
    if (ok && sep != 0) {
        ok = *end == sep;
        if (ok) {
            const char *start = end + 1;
            *b = g_ascii_strtod (start, &end);
            ok = end != start;
        }
    }
    ok = ok && *end == '\0';
    if (ok && parts[1] != NULL)
        ok = sscanf (parts[1], "%lf,%lf", x, y) == 2;
    g_strfreev (parts);
    return ok;
}

/* the page's state into host->state[slot], then next() from the main loop */
static void
request_state (Host *host, int slot, GSourceFunc next)
{
    char *script = g_strdup_printf ("window.__ucState && __ucState (%u)", ++host->state_seq);
    host->state_slot = slot;
    host->state_next = next;
    uc_webview_run_javascript (host->wv, script);
    g_free (script);
}

static gboolean
parse_state (const char *title, guint seq, PageState *state)
{
    char *prefix = g_strdup_printf ("ucstate:%u ", seq);
    gboolean ok = g_str_has_prefix (title, prefix);
    const char *p;

    g_free (prefix);
    if (!ok)
        return FALSE;
    memset (state, 0, sizeof *state);
#define FIELD(name, target, conv) if ((p = strstr (title, " " name ":")) != NULL) target = conv (p + strlen (name) + 2)
    FIELD ("click", state->click, atoi);
    FIELD ("scroll", state->scroll, atoi);
    FIELD ("touch", state->touch, atoi);
#define STRTOD(s) g_ascii_strtod (s, NULL)
    FIELD ("scale", state->scale, STRTOD);
    FIELD ("left", state->left, STRTOD);
    FIELD ("top", state->top, STRTOD);
    FIELD ("maxleft", state->max_left, STRTOD);
    FIELD ("maxtop", state->max_top, STRTOD);
#undef STRTOD
#undef FIELD
    return TRUE;
}

/* the content under the focal point before (state a) and after (state b) the magnification, CSS pixels: equal within
 * a CSS pixel unless the page could not scroll that far (uc_webview.h, uc_webview_set_magnification()) */
static gboolean
focal_point_kept (Host *host, const PageState *a, const PageState *b)
{
    double px0 = a->left + host->focal_x / a->scale, py0 = a->top + host->focal_y / a->scale;
    double px1 = b->left + host->focal_x / b->scale, py1 = b->top + host->focal_y / b->scale;
    double want_left = px0 - host->focal_x / b->scale, want_top = py0 - host->focal_y / b->scale;
    double dx = abs_double (px1 - px0), dy = abs_double (py1 - py0);
    gboolean edge_x = want_left < -0.5 || want_left > b->max_left + 0.5;
    gboolean edge_y = want_top < -0.5 || want_top > b->max_top + 0.5;
    gboolean ok = (dx <= 1.0 || edge_x) && (dy <= 1.0 || edge_y);

    g_print ("magnify: scroll %.1f,%.1f -> %.1f,%.1f; the content under %.0f,%.0f moved by %.2f,%.2f CSS px%s%s: %s\n",
             a->left, a->top, b->left, b->top, host->focal_x, host->focal_y, px1 - px0, py1 - py0,
             edge_x ? " (x at the page edge)" : "", edge_y ? " (y at the page edge)" : "", ok ? "kept" : "MOVED");
    return ok;
}

static gboolean
magnify_report (gpointer user_data)
{
    Host *host = user_data;
    const PageState *before = &host->state[opt_pinch != NULL ? 1 : 0], *after = &host->state[2];
    double magnification = uc_webview_get_magnification (host->wv);
    gboolean ok = host->magnify_status == UC_WEBVIEW_RENDERED_DONE
                  && abs_double (magnification - host->magnify_scale) < 1e-3
                  && abs_double (after->scale - host->magnify_scale) < 1e-3;

    g_print ("magnify: magnification %.2f (uc_webview_get_magnification), page scale %.2f (visualViewport)\n",
             magnification, after->scale);
    ok = focal_point_kept (host, before, after) && ok;
    if (opt_pinch != NULL)
        g_print ("pinch-test: %s\n", ok ? "ok" : "FAILED");
    else
        g_print ("magnify-test: %s\n", ok ? "ok" : "FAILED");
    quit (host, ok ? 0 : 1);
    return G_SOURCE_REMOVE;
}

static void
on_magnify_rendered (uc_webview *wv, int status, void *user)
{
    Host *host = user;
    (void) wv;
    host->magnify_status = status;
    host->magnify_ms = (g_get_monotonic_time () - host->magnify_t0) / 1000.0;
    g_print ("magnify: %.2f -> %.2f at %.0f,%.0f: rendered after %.0f ms (%s), %d frame(s) since\n",
             host->magnification0, host->magnify_scale, host->focal_x, host->focal_y, host->magnify_ms,
             status >= 0 && status <= 2 ? rendered_names[status] : "?", host->frames - host->zoom_frames_start);
    request_state (host, 2, magnify_report);
}

/* like a host at the end of a pinch: one magnification, then wait for the page to render it */
static gboolean
magnify_now (gpointer user_data)
{
    Host *host = user_data;

    host->magnification0 = uc_webview_get_magnification (host->wv);
    host->magnify_t0 = g_get_monotonic_time ();
    host->zoom_frames_start = host->frames;
    if (uc_webview_set_magnification (host->wv, host->magnify_scale, host->focal_x, host->focal_y) != 0
        || uc_webview_when_rendered (host->wv, on_magnify_rendered, host) != 0) {
        g_printerr ("magnify: uc_webview_set_magnification(%.2f) or uc_webview_when_rendered() refused\n",
                    host->magnify_scale);
        quit (host, 1);
    }
    return G_SOURCE_REMOVE;
}

/* after the gesture: did a click or scroll reach the page while two fingers were down? */
static gboolean
pinch_check (gpointer user_data)
{
    Host *host = user_data;
    const PageState *a = &host->state[0], *b = &host->state[1];
    int clicks = b->click - a->click, scrolls = b->scroll - a->scroll, touches = b->touch - a->touch;
    gboolean leaked = clicks != 0 || scrolls != 0;
    gboolean recognised = host->pinch_begins == 1 && host->pinch_ends == 1 && host->pinch_cancels == 0;

    g_print ("pinch: callbacks: %d begin (at %.0f,%.0f), %d update(s), %d end (scale %.3f at %.0f,%.0f), %d cancel\n",
             host->pinch_begins, host->pinch_begin_x, host->pinch_begin_y, host->pinch_updates, host->pinch_ends,
             host->pinch_scale, host->pinch_end_x, host->pinch_end_y, host->pinch_cancels);
    g_print ("pinch: during the gesture the page counted %d click(s), %d scroll event(s), %d touch event(s): %s\n",
             clicks, scrolls, touches, leaked ? "a tap or scroll LEAKED" : "no tap or scroll leaked");
    if (opt_prevent_touch) {
        /* the page handled the touches: the library must leave them alone */
        gboolean ok = host->pinch_begins == 0 && !leaked
                      && abs_double (uc_webview_get_magnification (host->wv) - host->magnification0) < 1e-6;
        g_print ("pinch: the page handled the touches (preventDefault), %s\n",
                 host->pinch_begins == 0 ? "no pinch reported" : "but the library PINCHED");
        g_print ("pinch-test: %s\n", ok ? "ok" : "FAILED");
        quit (host, ok ? 0 : 1);
        return G_SOURCE_REMOVE;
    }
    if (!recognised || leaked) {
        g_print ("pinch-test: FAILED (%s)\n", !recognised ? "no single pinch from begin to end" : "leak");
        quit (host, 1);
        return G_SOURCE_REMOVE;
    }
    /* the host's step: the magnification it had times the pinch's scale, around the point the pinch began at */
    host->magnify_scale = host->magnification0 * host->pinch_scale;
    host->magnify_scale = CLAMP (host->magnify_scale, UC_WEBVIEW_MAGNIFICATION_MIN, UC_WEBVIEW_MAGNIFICATION_MAX);
    host->focal_x = host->pinch_begin_x;
    host->focal_y = host->pinch_begin_y;
    magnify_now (host);
    return G_SOURCE_REMOVE;
}

static gboolean
pinch_finished (gpointer user_data)
{
    Host *host = user_data;
    /* the gesture's last reports come after WebKit's round trips through the web process */
    request_state (host, 1, pinch_check);
    return G_SOURCE_REMOVE;
}

static gboolean
pinch_tick (gpointer user_data)
{
    Host *host = user_data;
    double cx = host->focal_x, cy = host->focal_y, span;
    int step = host->pinch_step++;

    if (step == 0) {
        g_print ("pinch: fingers %.0f -> %.0f px apart around %.0f,%.0f in %d ms\n", host->pinch_from, host->pinch_to, cx,
                 cy, PINCH_STEPS * PINCH_STEP_MS);
        uc_webview_touch (host->wv, 0, UC_WEBVIEW_TOUCH_DOWN, cx - host->pinch_from / 2, cy, 0);
        return G_SOURCE_CONTINUE;
    }
    if (step == 1) {
        uc_webview_touch (host->wv, 1, UC_WEBVIEW_TOUCH_DOWN, cx + host->pinch_from / 2, cy, 0);
        return G_SOURCE_CONTINUE;
    }
    if (step - 1 <= PINCH_STEPS) {
        /* like a touch driver: every finger moves in every frame, the host forwards them one by one */
        span = host->pinch_from + (host->pinch_to - host->pinch_from) * (step - 1) / PINCH_STEPS;
        uc_webview_touch (host->wv, 0, UC_WEBVIEW_TOUCH_MOVE, cx - span / 2, cy, 0);
        uc_webview_touch (host->wv, 1, UC_WEBVIEW_TOUCH_MOVE, cx + span / 2, cy, 0);
        return G_SOURCE_CONTINUE;
    }
    if (step - 1 == PINCH_STEPS + 1) {
        uc_webview_touch (host->wv, 1, UC_WEBVIEW_TOUCH_UP, cx + host->pinch_to / 2, cy, 0);
        return G_SOURCE_CONTINUE;
    }
    uc_webview_touch (host->wv, 0, UC_WEBVIEW_TOUCH_UP, cx - host->pinch_to / 2, cy, 0);
    g_timeout_add (700, pinch_finished, host);
    return G_SOURCE_REMOVE;
}

/* the baseline is in: start the gesture */
static gboolean
gesture_begin (gpointer user_data)
{
    Host *host = user_data;

    host->magnification0 = uc_webview_get_magnification (host->wv);
    if (opt_pinch != NULL)
        g_timeout_add (PINCH_STEP_MS, pinch_tick, host);
    else
        magnify_now (host);
    return G_SOURCE_REMOVE;
}

static gboolean
gesture_settled (gpointer user_data)
{
    Host *host = user_data;
    request_state (host, 0, gesture_begin);
    return G_SOURCE_REMOVE;
}

static void
gesture_start (Host *host)
{
    char *script;

    if (host->gesture_started)
        return;
    host->gesture_started = TRUE;
    script = g_strdup_printf (counters_js, opt_prevent_touch ? 1 : 0);
    uc_webview_run_javascript (host->wv, script);
    g_free (script);
    /* the page settles (first frames, the counters' touch regions reach WebKit's event dispatcher) */
    g_timeout_add (1000, gesture_settled, host);
}

static void
on_pinch (uc_webview *wv, int phase, double scale, double x, double y, void *user)
{
    Host *host = user;
    (void) wv;
    switch (phase) {
    case UC_WEBVIEW_PINCH_BEGIN:
        host->pinch_begins++;
        host->pinch_begin_x = x;
        host->pinch_begin_y = y;
        g_print ("pinch begin at %.1f,%.1f\n", x, y);
        break;
    case UC_WEBVIEW_PINCH_UPDATE:
        host->pinch_updates++;
        if (opt_verbose)
            g_print ("pinch update %.3f at %.1f,%.1f\n", scale, x, y);
        break;
    case UC_WEBVIEW_PINCH_END:
        host->pinch_ends++;
        g_print ("pinch end %.3f at %.1f,%.1f\n", scale, x, y);
        break;
    case UC_WEBVIEW_PINCH_CANCEL:
        host->pinch_cancels++;
        g_print ("pinch cancel %.3f\n", scale);
        break;
    default:
        g_print ("pinch phase %d?\n", phase);
        return;
    }
    host->pinch_scale = scale;
    host->pinch_end_x = x;
    host->pinch_end_y = y;
}

static void
on_magnification_changed (uc_webview *wv, double magnification, void *user)
{
    Host *host = user;
    (void) wv;
    host->magnification_changes++;
    g_print ("magnification changed to %.2f (navigation or history)\n", magnification);
}

static void
on_frame_ready (uc_webview *wv, const uc_webview_frame *frame, void *user)
{
    Host *host = user;
    gint64 now = g_get_monotonic_time ();
    GChecksum *checksum;
    char fourcc[5];

    host->frames++;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (frame->fourcc >> (8 * i)) & 0xff;
        fourcc[i] = g_ascii_isprint (c) ? (char) c : '?';
    }
    fourcc[4] = '\0';

    if (opt_raf_test == 0
        && ((opt_zoom_test == NULL && opt_magnify == NULL && opt_pinch == NULL && !request_test_mode ()) || opt_out != NULL)) {
        /* the pacing, zoom, magnify and pinch tests measure WebKit, not the read-back: frames are displayed and released
         * at once (those tests read them back only for --out) */
        g_free (host->last_rgba);
        host->last_rgba = read_frame (host, frame);
        host->last_w = frame->width;
        host->last_h = frame->height;
        checksum = g_checksum_new (G_CHECKSUM_SHA256);
        g_checksum_update (checksum, host->last_rgba, (gsize) frame->width * frame->height * 4);
        g_free (host->last_sha);
        host->last_sha = g_strdup (g_checksum_get_string (checksum));
        g_checksum_free (checksum);
    }

    if ((opt_loop == 0 && opt_raf_test == 0 && opt_zoom_test == NULL && opt_magnify == NULL && opt_pinch == NULL
         && !request_test_mode ())
        || opt_verbose)
        g_print ("frame %d: %dx%d fourcc=%s modifier=0x%016" G_GINT64_MODIFIER "x planes=%d %s t=+%.1f ms (%.1f ms since last) sha256=%s\n",
                 host->frames, frame->width, frame->height, fourcc, frame->modifier, frame->n_planes,
                 frame->egl_image != NULL ? "egl-image" : (frame->pixels != NULL ? "shm-pixels" : "no-data"),
                 (now - host->start_us) / 1000.0, host->last_frame_us ? (now - host->last_frame_us) / 1000.0 : 0.0,
                 host->last_sha != NULL ? host->last_sha : "-");
    host->last_frame_us = now;

    /* Display it (synchronously read back above), then tell WebKit; release the previous frame like a
     * double-buffering scene graph would. */
    uc_webview_frame_rendered (wv, frame->handle);
    host->rendered++;
    if (host->has_held) {
        uc_webview_frame_release (wv, host->held.handle);
        host->released++;
    }
    host->held = *frame;
    host->has_held = TRUE;

    if (host->resizes != NULL && host->resizes->len > 0) {
        int w, h;
        char *next = g_ptr_array_steal_index (host->resizes, 0);
        if (parse_size (next, &w, &h)) {
            g_print ("resize -> %dx%d\n", w, h);
            uc_webview_resize (wv, w, h);
        }
        g_free (next);
    }
    maybe_tap (host);
    if (opt_raf_test > 0 && host->frames == 1)
        g_timeout_add (1000, raf_begin, host);

    if (opt_loop == 0 && !opt_ime_test && opt_raf_test == 0 && opt_zoom_test == NULL && opt_magnify == NULL
        && opt_pinch == NULL && !request_test_mode () && host->frames >= opt_frames)
        quit (host, 0);
}

static void
on_load_changed (uc_webview *wv, int event, const char *uri, void *user)
{
    Host *host = user;
    static const char *const names[] = { "started", "redirected", "committed", "finished", "FAILED" };
    char *loggable = uc_uri_policy_loggable (uri, uc_env_value_true (g_getenv ("UC_WEBVIEW_LOG_FULL_URLS")));
    (void) wv;
    g_print ("load %s: %s\n", event >= 0 && event <= 4 ? names[event] : "?", loggable);
    g_free (loggable);
    if (request_test_load_changed (event, uri))
        return;     /* a request mode: an unexpected FAILED fails its step */
    if (event == UC_WEBVIEW_LOAD_FAILED)
        quit (host, 1);
    else if (event == UC_WEBVIEW_LOAD_FINISHED && opt_zoom_test != NULL)
        zoom_start (host);
    else if (event == UC_WEBVIEW_LOAD_FINISHED && (opt_magnify != NULL || opt_pinch != NULL))
        gesture_start (host);
}

/* library 0.10.0: why the load failed, right before its FAILED */
static void
on_load_failed_detail (uc_webview *wv, const char *uri, int reason, const char *error_domain, int error_code,
                       unsigned tls_errors, const char *message, void *user)
{
    static const char *const names[] = { "OTHER", "HOST_NOT_FOUND", "NO_NETWORK", "CONNECTION_REFUSED", "TIMEOUT",
                                         "CERTIFICATE", "SCHEME_REFUSED" };
    gboolean full = uc_env_value_true (g_getenv ("UC_WEBVIEW_LOG_FULL_URLS"));
    char *loggable = uc_uri_policy_loggable (uri, full);
    char *scrubbed = uc_uri_policy_scrub_text (message, full);
    (void) wv; (void) user;
    g_print ("load failed: %s: %s (%s %d, tls 0x%x): %s\n", loggable,
             reason >= 0 && reason < (int) G_N_ELEMENTS (names) ? names[reason] : "?", error_domain, error_code,
             tls_errors, scrubbed);
    g_free (scrubbed);
    g_free (loggable);
}

static gboolean
ime_settled (gpointer user_data)
{
    Host *host = user_data;
    host->ime_settle_source = 0;
    if (g_strcmp0 (host->last_title, "v:ab") == 0) {
        g_print ("ime-test: input value is \"ab\" as expected%s\n",
                 host->ime_seen_preedit ? "" : " (the pre-edit text was not observed through input events)");
        quit (host, 0);
    } else {
        g_printerr ("ime-test: final input value \"%s\", expected \"ab\"\n",
                    host->last_title != NULL && g_str_has_prefix (host->last_title, "v:") ? host->last_title + 2 : "(none)");
        quit (host, 1);
    }
    return G_SOURCE_REMOVE;
}

static void
on_title_changed (uc_webview *wv, const char *title, void *user)
{
    Host *host = user;
    (void) wv;
    g_print ("title: %s\n", title != NULL ? title : "(null)");
    g_free (host->last_title);
    host->last_title = g_strdup (title);
    request_test_title_changed (title);
    if (host->state_next != NULL && title != NULL
        && parse_state (title, host->state_seq, &host->state[host->state_slot])) {
        GSourceFunc next = host->state_next;
        host->state_next = NULL;
        g_idle_add (next, host);
    }
    if (opt_raf_test > 0 && host->raf_measuring && title != NULL && g_str_has_prefix (title, "raf:")) {
        /* "raf:<rate> sd:<ms> long:<n> mode:<moving|idle>" */
        const char *sd = strstr (title, " sd:"), *lg = strstr (title, " long:");
        double rate = g_ascii_strtod (title + 4, NULL);
        double deviation = sd != NULL ? g_ascii_strtod (sd + 4, NULL) : 0.0;
        if (strstr (title, " mode:moving") != NULL) {
            g_array_append_val (host->raf_moving, rate);
        } else {
            g_array_append_val (host->raf_rates, rate);
            g_array_append_val (host->raf_sds, deviation);
            host->raf_long += lg != NULL ? atoi (lg + 6) : 0;
        }
    }
    if (opt_ime_test && title != NULL && g_str_has_prefix (title, "v:") && host->ime_typed) {
        /* commit(abc) -> "abc", delete(-1,1) -> "ab", preedit(z) -> "abz" (composition), preedit("") -> "ab" */
        if (g_str_equal (title, "v:abz"))
            host->ime_seen_preedit = TRUE;
        if (host->ime_settle_source == 0)
            host->ime_settle_source = g_timeout_add (1500, ime_settled, host);
    }
}

static void
on_progress (uc_webview *wv, double progress, void *user)
{
    (void) wv; (void) user;
    if (opt_verbose)
        g_print ("progress %.0f%%\n", progress * 100);
}

static void
on_terminated (uc_webview *wv, int reason, void *user)
{
    Host *host = user;
    (void) wv;
    g_printerr ("web process terminated (reason %d)\n", reason);
    if (request_test_terminated (reason))
        return;     /* the dialog test kills it on purpose */
    quit (host, 1);
}

static gboolean
ime_type (gpointer user_data)
{
    Host *host = user_data;
    /* The IME validation sequence: commit, delete one, pre-edit shown and ended -> "ab". */
    uc_webview_ime_commit (host->wv, "abc");
    uc_webview_ime_delete_surrounding (host->wv, -1, 1);
    uc_webview_ime_preedit (host->wv, "z", 1);
    uc_webview_ime_preedit (host->wv, "", 0);
    host->ime_typed = TRUE;
    g_print ("ime-test: typed commit(abc) delete(-1,1) preedit(z) preedit(\"\")\n");
    return G_SOURCE_REMOVE;
}

static void
on_ime_focus (uc_webview *wv, int focused, unsigned hints, unsigned purpose, const char *surrounding, int cursor, void *user)
{
    Host *host = user;
    (void) wv;
    (void) surrounding; (void) cursor; /* the field's content and caret are not printed */
    g_print ("ime focus=%d hints=0x%x purpose=%u\n", focused, hints, purpose);
    if (focused) {
        host->ime_focus_count++;
        if (opt_ime_test && !host->ime_typed)
            g_timeout_add (100, ime_type, host);
    }
}

static void
on_ime_cursor_area (uc_webview *wv, int x, int y, int w, int h, void *user)
{
    (void) wv; (void) user;
    if (opt_verbose)
        g_print ("ime cursor area %d,%d %dx%d\n", x, y, w, h);
}

static void
on_ime_surrounding (uc_webview *wv, const char *text, int cursor, int anchor, void *user)
{
    (void) wv; (void) user; (void) text; (void) cursor; (void) anchor;
    if (opt_verbose)
        g_print ("ime surrounding text changed\n");
}

static void
on_cursor (uc_webview *wv, const char *name, void *user)
{
    (void) wv; (void) user;
    if (opt_verbose)
        g_print ("cursor: %s\n", name);
}

static gboolean
on_timeout (gpointer user_data)
{
    Host *host = user_data;
    host->timeout_source = 0;
    g_printerr ("ucwebview-test: timeout after %d s (%d frame(s) received)\n", opt_timeout, host->frames);
    quit (host, 2);
    return G_SOURCE_REMOVE;
}

static gboolean
on_loop_tick (gpointer user_data)
{
    Host *host = user_data;
    char *rss = read_vm_rss ();
    g_print ("soak +%.0f s: %d frames, rendered %d, released %d, VmRSS %s\n", (g_get_monotonic_time () - host->start_us) / 1e6,
             host->frames, host->rendered, host->released, rss);
    g_free (rss);
    if ((g_get_monotonic_time () - host->start_us) / 1000000 >= opt_loop) {
        quit (host, 0);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
on_signal (gpointer user_data)
{
    quit (user_data, 0);
    return G_SOURCE_REMOVE;
}

/* ---- selftest ---------------------------------------------------------------------------- */

static int
selftest (void)
{
    int failures = 0;
    uc_webview_callbacks cb = { 0 };
#define CHECK(cond) do { if (!(cond)) { g_printerr ("selftest FAILED: %s\n", #cond); failures++; } } while (0)

    CHECK (uc_webview_api_version () == UC_WEBVIEW_API_VERSION);
    /* before global_init: every call is a no-op or returns NULL/0, nothing crashes */
    CHECK (uc_webview_new (100, 100, &cb, NULL) == NULL);
    CHECK (uc_webview_new_sized (100, 100, &cb, sizeof cb, NULL) == NULL);
    uc_webview_global_shutdown ();
    uc_webview_free (NULL);
    uc_webview_resize (NULL, 1, 1);
    uc_webview_set_mapped (NULL, 1);
    uc_webview_set_focused (NULL, 1);
    uc_webview_set_scale (NULL, 1.0);
    uc_webview_load_uri (NULL, "about:blank");
    uc_webview_load_html (NULL, "", NULL);
    uc_webview_reload (NULL);
    uc_webview_stop (NULL);
    uc_webview_go_back (NULL);
    uc_webview_go_forward (NULL);
    CHECK (uc_webview_can_go_back (NULL) == 0);
    CHECK (uc_webview_can_go_forward (NULL) == 0);
    CHECK (uc_webview_get_uri (NULL) == NULL);
    CHECK (uc_webview_get_title (NULL) == NULL);
    uc_webview_run_javascript (NULL, "1");
    uc_webview_set_zoom (NULL, 1.0);
    uc_webview_set_background_rgba (NULL, 0, 0, 0, 1);
    uc_webview_frame_rendered (NULL, NULL);
    uc_webview_frame_release (NULL, NULL);
    uc_webview_touch (NULL, 0, UC_WEBVIEW_TOUCH_DOWN, 0, 0, 0);
    uc_webview_pointer_button (NULL, UC_WEBVIEW_BUTTON_PRIMARY, 1, 0, 0, 0, 0);
    uc_webview_pointer_move (NULL, 0, 0, 0, 0);
    uc_webview_scroll (NULL, 0, 1, 0, 0, 0, 0);
    uc_webview_key (NULL, 0xff0d, 0, 1, 0, 0);
    uc_webview_ime_commit (NULL, "a");
    uc_webview_ime_preedit (NULL, "a", 1);
    uc_webview_ime_delete_surrounding (NULL, -1, 1);
    /* rendered requests (library 0.5.0): refused without a view, the callback never runs */
    CHECK (uc_webview_when_rendered (NULL, NULL, NULL) == -1);
    CHECK (UC_WEBVIEW_RENDERED_DONE == 0 && UC_WEBVIEW_RENDERED_TIMEOUT == 1 && UC_WEBVIEW_RENDERED_CANCELLED == 2);
    /* magnification (library 0.6.0): refused without a view */
    CHECK (uc_webview_set_magnification (NULL, 2.0, 0, 0) == -1);
    CHECK (uc_webview_get_magnification (NULL) == 0.0);
    CHECK (UC_WEBVIEW_PINCH_BEGIN == 0 && UC_WEBVIEW_PINCH_UPDATE == 1 && UC_WEBVIEW_PINCH_END == 2
           && UC_WEBVIEW_PINCH_CANCEL == 3);
    /* display refresh rate: kept before global_init (the environment is read only by global_init), range checked */
    CHECK (uc_webview_global_set_refresh_rate (50.0) == 0);
    CHECK (uc_webview_global_get_refresh_rate () == 50.0);
    CHECK (uc_webview_global_set_refresh_rate (19.0) == -1);
    CHECK (uc_webview_global_set_refresh_rate (241.0) == -1);
    CHECK (uc_webview_global_set_refresh_rate (NAN) == -1);
    CHECK (uc_webview_global_get_refresh_rate () == 50.0);
    CHECK (uc_webview_global_set_refresh_rate (59.6) == 0);
    CHECK (uc_webview_global_get_refresh_rate () == 59.6);
    /* touch drag threshold (library 0.3.0): 16 px unless UC_WEBVIEW_DRAG_THRESHOLD names 1..200 */
    {
        const char *env = g_getenv ("UC_WEBVIEW_DRAG_THRESHOLD");
        int threshold = uc_webview_global_get_drag_threshold ();
        if (env == NULL || *env == '\0')
            CHECK (threshold == 16);
        else
            CHECK (threshold >= 1 && threshold <= 200);
    }
    /* struct layout the hosts rely on */
    CHECK (sizeof (uc_webview_frame) >= 7 * sizeof (void *));
    CHECK (sizeof (uc_webview_callbacks) >= 9 * sizeof (void (*) (void)));   /* the API version 1 callbacks */
    CHECK (sizeof (uc_webview_callbacks) == 18 * sizeof (void (*) (void)));  /* + pinch, magnification_changed (0.6.0),
                                                                               + six request callbacks (0.7.0),
                                                                               + load_failed_detail (0.10.0) */
    CHECK (UC_WEBVIEW_FAIL_OTHER == 0 && UC_WEBVIEW_FAIL_HOST_NOT_FOUND == 1 && UC_WEBVIEW_FAIL_NO_NETWORK == 2
           && UC_WEBVIEW_FAIL_CONNECTION_REFUSED == 3 && UC_WEBVIEW_FAIL_TIMEOUT == 4 && UC_WEBVIEW_FAIL_CERTIFICATE == 5
           && UC_WEBVIEW_FAIL_SCHEME_REFUSED == 6);
    /* requests (library 0.7.0): refused without a view */
    CHECK (uc_webview_request_reply (NULL, 1, 1, NULL) == -1);
    CHECK (uc_webview_request_auth (NULL, 1, "user", "password") == -1);
    CHECK (UC_WEBVIEW_DIALOG_ALERT == 0 && UC_WEBVIEW_DIALOG_CONFIRM == 1 && UC_WEBVIEW_DIALOG_PROMPT == 2
           && UC_WEBVIEW_DIALOG_BEFORE_UNLOAD == 3);
    CHECK (UC_WEBVIEW_AUTH_SCHEME_HTTP_BASIC == 2 && UC_WEBVIEW_AUTH_SCHEME_HTTP_DIGEST == 3 && UC_WEBVIEW_AUTH_RETRY == 1
           && UC_WEBVIEW_AUTH_PROXY == 2);
    CHECK (UC_WEBVIEW_NEW_WINDOW_LOADED == 0 && UC_WEBVIEW_NEW_WINDOW_REFUSED == 1 && UC_WEBVIEW_RESPONSE_DOWNLOAD == 0
           && UC_WEBVIEW_RESPONSE_UNSUPPORTED == 1);
    CHECK (UC_WEBVIEW_LOAD_FINISHED == 3 && UC_WEBVIEW_TERMINATED_BY_API == 2 && UC_WEBVIEW_MOD_BUTTON1 == 256);
    /* site settings (library 0.8.0): the view functions refuse NULL, the session functions need
     * uc_webview_global_init(); exceptions, the TLS policy and the color scheme are kept for it */
    {
        const char *names[] = { "Authorization" }, *values[] = { "Bearer x" };
        uc_webview_cookie cookie = { "a", "1", NULL, NULL, 0, 0, -1 };
        CHECK (uc_webview_set_origin_headers (NULL, "https://example.com", names, values, 1) == -1);
        CHECK (uc_webview_set_user_agent (NULL, UC_WEBVIEW_USER_AGENT_MOBILE, NULL) == -1);
        CHECK (uc_webview_global_set_cookies ("https://example.com/", &cookie, 1, NULL, NULL) == -1);
        CHECK (uc_webview_global_clear_data (UC_WEBVIEW_DATA_ALL, NULL, NULL) == -1);
        CHECK (uc_webview_global_allow_certificate ("selftest.invalid", NULL) == 0);
        CHECK (uc_webview_global_allow_certificate ("selftest.invalid:443", NULL) == -1);
        CHECK (uc_webview_global_set_tls_errors_policy (0) == 0);
        CHECK (uc_webview_global_set_color_scheme (UC_WEBVIEW_COLOR_SCHEME_LIGHT) == 0);
        CHECK (uc_webview_global_set_color_scheme (7) == -1);
        CHECK (UC_WEBVIEW_USER_AGENT_DEFAULT == 0 && UC_WEBVIEW_USER_AGENT_CUSTOM == 3 && UC_WEBVIEW_COLOR_SCHEME_DARK == 1
               && UC_WEBVIEW_DATA_CACHE == 1u && UC_WEBVIEW_DATA_ALL == 0xffffffffu && UC_WEBVIEW_DONE_OK == 0);
        CHECK (sizeof (uc_webview_cookie) == 4 * sizeof (char *) + 2 * sizeof (int) + sizeof (int64_t));
    }
    /* device state for web pages (library 0.9.0): refused without a view */
    CHECK (uc_webview_set_device_api (NULL, 1) == -1);
    CHECK (uc_webview_set_device_state (NULL, "{}") == -1);
    CHECK (uc_webview_device_event (NULL, "suspend", 0) == -1);
#undef CHECK
    g_print ("selftest %s (API version %d, library %s)\n", failures == 0 ? "ok" : "FAILED", uc_webview_api_version (), UC_WEBVIEW_VERSION);
    return failures == 0 ? 0 : 1;
}

/* ---- main ------------------------------------------------------------------------------- */

static Host *the_host;

/* the request modes end the loop through this */
static void
request_quit (int code)
{
    quit (the_host, code);
}

/* after uc_webview_global_shutdown() */
static void
egl_teardown (Host *host)
{
    if (host->egl_display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent (host->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (host->egl_surface != EGL_NO_SURFACE)
        eglDestroySurface (host->egl_display, host->egl_surface);
    eglDestroyContext (host->egl_display, host->egl_context);
    eglTerminate (host->egl_display);
}

/* The library's rule for UC_WEBVIEW_ALLOW_FILE_URLS ("1", "true", "yes", "on"; uc_uri_policy.c) */
static gboolean
file_urls_allowed (void)
{
    const char *value = g_getenv ("UC_WEBVIEW_ALLOW_FILE_URLS");
    return value != NULL && (g_str_equal (value, "1") || g_ascii_strcasecmp (value, "true") == 0
                             || g_ascii_strcasecmp (value, "yes") == 0 || g_ascii_strcasecmp (value, "on") == 0);
}

int
main (int argc, char **argv)
{
    GOptionContext *context = g_option_context_new ("[URL]  - libucwebview test host");
    GError *error = NULL;
    Host host = { 0 };
    uc_webview_callbacks cb = {
        .frame_ready = on_frame_ready, .load_changed = on_load_changed, .title_changed = on_title_changed,
        .progress_changed = on_progress, .web_process_terminated = on_terminated, .ime_focus = on_ime_focus,
        .ime_cursor_area = on_ime_cursor_area, .cursor_changed = on_cursor, .ime_surrounding = on_ime_surrounding,
        .pinch = on_pinch, .magnification_changed = on_magnification_changed,
        .load_failed_detail = on_load_failed_detail,
    };
    int width = 480, height = 800;

    g_set_prgname ("ucwebview-test");
    g_option_context_add_main_entries (context, entries, NULL);
    g_option_context_add_main_entries (context, request_test_entries, NULL);
    /* the six request callbacks (0.7.0): the tool answers requests in every mode */
    request_test_set_callbacks (&cb);
    request_test_set_quit (request_quit);
    the_host = &host;
    g_option_context_add_main_entries (context, link_test_entries, NULL);
    g_option_context_add_main_entries (context, ime_swap_test_entries, NULL);
    g_option_context_add_main_entries (context, site_test_entries, NULL);
    g_option_context_add_main_entries (context, device_test_entries, NULL);
    g_option_context_add_main_entries (context, load_fail_test_entries, NULL);
    g_option_context_add_main_entries (context, process_test_entries, NULL);
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("ucwebview-test: %s\n", error->message);
        return 3;
    }
    if (opt_version) {
        g_print ("ucwebview-test %s (API %d)\n", UC_WEBVIEW_VERSION, UC_WEBVIEW_API_VERSION);
        return 0;
    }
    if (opt_selftest)
        return selftest ();
    if (!link_test_check_options () || !ime_swap_test_check_options () || !site_test_check_options ()
        || !load_fail_test_check_options () || !process_test_check_options ())
        return 3;
    if (opt_url == NULL && argc > 1)
        opt_url = g_strdup (argv[1]);
    if (!request_test_check_options ())
        return 3;
    if (opt_url == NULL && opt_html == NULL && !opt_ime_test && opt_raf_test == 0 && opt_zoom_test == NULL
        && opt_magnify == NULL && opt_pinch == NULL && !request_test_mode () && !link_test_mode ()
        && !ime_swap_test_mode () && !site_test_mode () && !device_test_mode () && !load_fail_test_mode ()
        && !process_test_mode ()) {
        g_printerr ("%s", g_option_context_get_help (context, TRUE, NULL));
        return 3;
    }
    if (opt_size != NULL && !parse_size (opt_size, &width, &height)) {
        g_printerr ("ucwebview-test: bad --size\n");
        return 3;
    }
    if (opt_refresh_rate != 0.0 && !(opt_refresh_rate >= 20.0 && opt_refresh_rate <= 240.0)) {
        g_printerr ("ucwebview-test: --refresh-rate must be 20..240 Hz\n");
        return 3;
    }
    if (opt_raf_test < 0) {
        g_printerr ("ucwebview-test: bad --raf-test\n");
        return 3;
    }
    if (opt_zoom_test != NULL) {
        host.zoom_steps = g_strsplit (opt_zoom_test, ",", -1);
        if (!zoom_steps_valid (host.zoom_steps)) {
            g_printerr ("ucwebview-test: bad --zoom-test, expected levels in percent (1..2000) like 110,125+150,100\n");
            return 3;
        }
        host.zoom_ms = g_array_new (FALSE, FALSE, sizeof (double));
    }
    if (opt_magnify != NULL && opt_pinch != NULL) {
        g_printerr ("ucwebview-test: --magnify and --pinch are separate tests\n");
        return 3;
    }
    if (opt_magnify != NULL
        && (!parse_gesture (opt_magnify, 0, &host.magnify_scale, NULL, &host.focal_x, &host.focal_y, width, height)
            || !(host.magnify_scale >= UC_WEBVIEW_MAGNIFICATION_MIN && host.magnify_scale <= UC_WEBVIEW_MAGNIFICATION_MAX))) {
        g_printerr ("ucwebview-test: bad --magnify, expected S[@X,Y] with S in %.2f..%.0f\n", UC_WEBVIEW_MAGNIFICATION_MIN,
                    UC_WEBVIEW_MAGNIFICATION_MAX);
        return 3;
    }
    if (opt_pinch != NULL
        && (!parse_gesture (opt_pinch, ':', &host.pinch_from, &host.pinch_to, &host.focal_x, &host.focal_y, width,
                            height)
            || host.pinch_from < 20 || host.pinch_to < 20)) {
        g_printerr ("ucwebview-test: bad --pinch, expected FROM:TO[@X,Y], finger distances of 20 px or more\n");
        return 3;
    }
    if (opt_prevent_touch && opt_pinch == NULL) {
        g_printerr ("ucwebview-test: --prevent-touch goes with --pinch\n");
        return 3;
    }
    if (uc_webview_api_version () != UC_WEBVIEW_API_VERSION) {
        g_printerr ("ucwebview-test: library API version %d, compiled against %d\n", uc_webview_api_version (), UC_WEBVIEW_API_VERSION);
        return 1;
    }
    if (opt_verbose)
        g_setenv ("G_MESSAGES_DEBUG", "all", FALSE);
    /* the library reads the switch once in uc_webview_global_init() */
    if (opt_allow_file_urls)
        g_setenv ("UC_WEBVIEW_ALLOW_FILE_URLS", "1", TRUE);

    if (!setup_egl (&host, opt_egl != NULL ? opt_egl : "device"))
        return 2;

    if (uc_webview_global_init (host.egl_display != EGL_NO_DISPLAY ? host.egl_display : NULL, opt_data_dir, NULL) != 0) {
        g_printerr ("ucwebview-test: uc_webview_global_init failed\n");
        return 2;
    }
    /* like remote-ui: the screen's rate right after init, before the first view and load */
    if (opt_refresh_rate != 0.0)
        uc_webview_global_set_refresh_rate (opt_refresh_rate);
    g_print ("display refresh rate: %.3f Hz\n", uc_webview_global_get_refresh_rate ());
    g_print ("touch drag threshold: %d px\n", uc_webview_global_get_drag_threshold ());
    if (link_test_mode ()) {
        /* views of its own, one per variant (ucwebview-test-links.c) */
        int code = link_test_run (width, height, opt_scale, opt_timeout > 0 ? opt_timeout : link_test_default_timeout ());
        uc_webview_global_shutdown ();
        egl_teardown (&host);
        g_option_context_free (context);
        return code;
    }
    if (ime_swap_test_mode ()) {
        /* views of its own, one per variant (ucwebview-test-ime.c) */
        int code = ime_swap_test_run (width, height, opt_scale,
                                      opt_timeout > 0 ? opt_timeout : ime_swap_test_default_timeout ());
        uc_webview_global_shutdown ();
        egl_teardown (&host);
        g_option_context_free (context);
        return code;
    }
    if (device_test_mode ()) {
        /* a view of its own (ucwebview-test-device.c) */
        int code = device_test_run (width, height, opt_scale,
                                    opt_timeout > 0 ? opt_timeout : device_test_default_timeout ());
        uc_webview_global_shutdown ();
        egl_teardown (&host);
        g_option_context_free (context);
        return code;
    }
    if (load_fail_test_mode ()) {
        /* views of its own (ucwebview-test-loadfail.c) */
        int code = load_fail_test_run (width, height, opt_scale,
                                       opt_timeout > 0 ? opt_timeout : load_fail_test_default_timeout ());
        uc_webview_global_shutdown ();
        egl_teardown (&host);
        g_option_context_free (context);
        return code;
    }
    if (process_test_mode ()) {
        /* views of its own (ucwebview-test-process.c) */
        int code = process_test_run (width, height, opt_scale,
                                     opt_timeout > 0 ? opt_timeout : process_test_default_timeout ());
        uc_webview_global_shutdown ();
        egl_teardown (&host);
        g_option_context_free (context);
        return code;
    }
    if (site_test_mode ()) {
        /* a view of its own (ucwebview-test-site.c); --data-dir went to uc_webview_global_init() above */
        int code = site_test_run (width, height, opt_scale, opt_data_dir,
                                  opt_timeout > 0 ? opt_timeout : site_test_default_timeout ());
        uc_webview_global_shutdown ();
        egl_teardown (&host);
        g_option_context_free (context);
        return code;
    }

    host.loop = g_main_loop_new (NULL, FALSE);
    host.start_us = g_get_monotonic_time ();
    /* like a host built against this header (remote-ui resolves uc_webview_new_sized() as an optional symbol) */
    if (opt_plain_new)
        host.wv = uc_webview_new (width, height, &cb, &host);
    else
        host.wv = uc_webview_new_sized (width, height, &cb, sizeof cb, &host);
    if (host.wv == NULL) {
        g_printerr ("ucwebview-test: %s failed\n", opt_plain_new ? "uc_webview_new" : "uc_webview_new_sized");
        return 2;
    }
    if (opt_plain_new)
        g_print ("view: uc_webview_new()\n");
    else
        g_print ("view: uc_webview_new_sized(), callbacks %" G_GSIZE_FORMAT " bytes\n", (gsize) sizeof cb);
    uc_webview_set_scale (host.wv, opt_scale);
    /* the library's default since 0.6.2; set anyway, so that frame hashes do not depend on the library's release */
    uc_webview_set_background_rgba (host.wv, 1, 1, 1, 1);
    if (opt_resize != NULL) {
        char **parts = g_strsplit (opt_resize, ",", -1);
        host.resizes = g_ptr_array_new_with_free_func (g_free);
        for (char **p = parts; *p != NULL; p++)
            g_ptr_array_add (host.resizes, g_strdup (*p));
        g_strfreev (parts);
    }
    host.raf_rates = g_array_new (FALSE, FALSE, sizeof (double));
    host.raf_moving = g_array_new (FALSE, FALSE, sizeof (double));
    host.raf_sds = g_array_new (FALSE, FALSE, sizeof (double));
    if (request_test_mode ()) {
        if (!request_test_start (host.wv, opt_plain_new, request_quit))
            return 2;
    } else if (opt_raf_test > 0) {
        g_print ("raf-test: loading the requestAnimationFrame counter page\n");
        uc_webview_load_html (host.wv, raf_test_html, "about:blank");
    } else if (opt_zoom_test != NULL && opt_url == NULL && opt_html == NULL) {
        g_print ("zoom-test: loading the internal long page\n");
        uc_webview_load_html (host.wv, zoom_test_html, "about:blank");
    } else if ((opt_magnify != NULL || opt_pinch != NULL) && opt_url == NULL && opt_html == NULL) {
        g_print ("%s: loading the internal grid page\n", opt_pinch != NULL ? "pinch-test" : "magnify-test");
        uc_webview_load_html (host.wv, grid_test_html, "about:blank");
    } else if (opt_ime_test) {
        if (opt_tap == NULL)
            opt_tap = g_strdup ("60,40"); /* inside the <input> of ime_test_html */
        g_print ("ime-test: loading the test page, tapping the input at %s\n", opt_tap);
        uc_webview_load_html (host.wv, ime_test_html, "about:blank");
    } else if (opt_html != NULL) {
        char *html = NULL;
        if (!g_file_get_contents (opt_html, &html, NULL, &error)) {
            g_printerr ("ucwebview-test: %s\n", error->message);
            return 3;
        }
        /* relative URLs of the file resolve against file:/// only where the library allows file: URLs; else
         * the page is an about:blank document like any other HTML string */
        uc_webview_load_html (host.wv, html, file_urls_allowed () ? "file:///" : NULL);
        g_free (html);
    } else {
        uc_webview_load_uri (host.wv, opt_url);
    }

    if (opt_timeout < 0 && request_test_mode ())
        opt_timeout = request_test_default_timeout ();
    else if (opt_timeout < 0 && opt_zoom_test != NULL)
        opt_timeout = 30 + 62 * (int) g_strv_length (host.zoom_steps);   /* every step may take the library's 60 s */
    else if (opt_timeout < 0 && (opt_magnify != NULL || opt_pinch != NULL))
        opt_timeout = 30 + 62;                                            /* the magnification may take 60 s as well */
    else if (opt_timeout < 0)
        opt_timeout = (opt_loop > 0 ? opt_loop : opt_raf_test) + 30;   /* a soak must not be cut short by the default */
    host.timeout_source = g_timeout_add_seconds (opt_timeout, on_timeout, &host);
    if (opt_loop > 0)
        g_timeout_add_seconds (10, on_loop_tick, &host);
    g_unix_signal_add (SIGINT, on_signal, &host);
    g_unix_signal_add (SIGTERM, on_signal, &host);

    g_main_loop_run (host.loop);

    if (host.timeout_source != 0)
        g_source_remove (host.timeout_source);
    if (host.has_held) {
        uc_webview_frame_release (host.wv, host.held.handle);
        host.released++;
    }
    if (host.last_rgba != NULL && opt_out != NULL) {
        if (write_png (opt_out, host.last_rgba, host.last_w, host.last_h))
            g_print ("wrote %s (%dx%d RGBA, sha256 of the raw pixels %s)\n", opt_out, host.last_w, host.last_h, host.last_sha);
        else
            host.exit_code = host.exit_code == 0 ? 2 : host.exit_code;
    }
    {
        char *rss = read_vm_rss ();
        g_print ("summary: %d frames in %.1f s, rendered %d, released %d, ime focus events %d, VmRSS %s, exit %d\n", host.frames,
                 (g_get_monotonic_time () - host.start_us) / 1e6, host.rendered, host.released, host.ime_focus_count, rss, host.exit_code);
        g_free (rss);
    }
    if (opt_ime_test && host.exit_code == 0 && !host.ime_typed) {
        g_printerr ("ime-test: ime_focus never fired\n");
        host.exit_code = 1;
    }

    /* a request left pending ends inside uc_webview_free() without a callback (uc_webview.h) */
    request_test_freeing (TRUE);
    uc_webview_free (host.wv);
    request_test_freeing (FALSE);
    host.exit_code = request_test_finish (host.exit_code);
    uc_webview_global_shutdown ();
    egl_teardown (&host);
    g_free (host.last_rgba);
    g_free (host.last_sha);
    g_free (host.last_title);
    if (host.resizes != NULL)
        g_ptr_array_unref (host.resizes);
    g_array_unref (host.raf_rates);
    g_array_unref (host.raf_moving);
    g_array_unref (host.raf_sds);
    g_strfreev (host.zoom_steps);
    if (host.zoom_ms != NULL)
        g_array_unref (host.zoom_ms);
    g_main_loop_unref (host.loop);
    g_option_context_free (context);
    return host.exit_code;
}
