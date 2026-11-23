#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
"""
Generate the public wpe-webkit-2.0 / wpe-platform-2.0 headers of a WPE WebKit
source tree on a build host without WebKit, so that the libucwebview sources can
be checked with `cc -fsyntax-only` (make syntax-check WPE_HOST_INCLUDE=<out>).
The launcher (../launcher) uses the same output. --expect-version fails unless
the tree is that WPE release, so that the check is known to run against it; the
output directory gets a VERSION stamp file.

This replays what the WebKit build does for the installed headers:
  * Source/WebKit/Scripts/glib/generate-api-header.py: substitute
    @API_INCLUDE_PREFIX@ / @*_SINGLE_HEADER_CHECK@ and expand PLATFORM(X),
    ENABLE(X), USE(X) into preprocessor tests (the unifdef pass is skipped, the
    compiler evaluates the tests with the -D flags from PlatformWPE.cmake:323-331);
  * glib-mkenums with WebKitEnumTypes.h.in / WPEEnumTypes.h.in and the sed
    fix-ups from PlatformWPE.cmake:357 and WPEPlatform/CMakeLists.txt:87;
  * WebKitVersion.h / WPEVersion.h / WPEConfig.h from their .in templates.

libsoup-3.0 and the JavaScriptCore GLib API are replaced by stubs with opaque types
and the functions the library and its web process extension call; everything else
is the real header text.

usage: mk-host-headers.py [--expect-version X.Y.Z] <wpewebkit-source-dir> <output-dir>
"""
import os
import re
import shutil
import subprocess
import sys

API_SINGLE_HEADER_CHECK = '''#if !defined(__WEBKIT_H_INSIDE__) && !defined(BUILDING_WEBKIT)
#error "Only <wpe/webkit.h> can be included directly."
#endif'''
SHARED_API_SINGLE_HEADER_CHECK = '''#if !defined(__WEBKIT_H_INSIDE__) && !defined(__WEBKIT_WEB_PROCESS_EXTENSION_H_INSIDE__) && !defined(BUILDING_WEBKIT)
#error "Only <wpe/webkit.h> can be included directly."
#endif'''

SOUP_STUB = '''/* stub for the host syntax check: opaque libsoup-3.0 types referenced by the WebKit headers */
#ifndef STUB_LIBSOUP_SOUP_H
#define STUB_LIBSOUP_SOUP_H
#include <glib-object.h>
typedef struct _SoupMessageHeaders SoupMessageHeaders;
typedef struct _SoupCookie SoupCookie;
typedef struct _SoupSession SoupSession;
typedef struct _SoupMessage SoupMessage;
/* the function libucwebview calls (0.7.0, the Content-Disposition of a response), as declared in libsoup-3.0's
 * soup-message-headers.h */
const char *soup_message_headers_get_one (SoupMessageHeaders *hdrs, const char *name);
/* 0.8.0: the origin headers of the web process extension (soup-message-headers.h), cookies (soup-cookie.h) and the
 * public suffix check (soup-tld.h), as declared in libsoup-3.0 */
void soup_message_headers_replace (SoupMessageHeaders *hdrs, const char *name, const char *value);
void soup_message_headers_remove (SoupMessageHeaders *hdrs, const char *name);
SoupCookie *soup_cookie_new (const char *name, const char *value, const char *domain, const char *path, int max_age);
void soup_cookie_set_expires (SoupCookie *cookie, GDateTime *expires);
void soup_cookie_set_secure (SoupCookie *cookie, gboolean secure);
void soup_cookie_set_http_only (SoupCookie *cookie, gboolean http_only);
void soup_cookie_free (SoupCookie *cookie);
gboolean soup_tld_domain_is_public_suffix (const char *domain);
#endif
'''
JSC_STUB = '''/* stub for the host syntax check: opaque JavaScriptCore GLib types referenced by the WebKit headers */
#ifndef STUB_JSC_JSC_H
#define STUB_JSC_JSC_H
#include <glib-object.h>
typedef struct _JSCValue JSCValue;
typedef struct _JSCException JSCException;
typedef struct _JSCContext JSCContext;
/* the functions libucwebview calls, as declared in JSCValue.h (Source/JavaScriptCore/API/glib, installed as
 * wpe-webkit-2.0/jsc/JSCValue.h:78-82) */
gboolean jsc_value_is_number (JSCValue *value);
double jsc_value_to_double (JSCValue *value);
/* 0.9.0: what the web process extension calls (uc_ext_device.c), as declared in the installed 2.54.2
 * wpe-webkit-2.0/jsc/JSCValue.h, JSCContext.h and JSCException.h; the cross build compiles against the real ones */
typedef enum {
    JSC_VALUE_PROPERTY_CONFIGURABLE = 1 << 0,
    JSC_VALUE_PROPERTY_ENUMERABLE   = 1 << 1,
    JSC_VALUE_PROPERTY_WRITABLE     = 1 << 2
} JSCValuePropertyFlags;
GType jsc_value_get_type (void);
#define JSC_TYPE_VALUE (jsc_value_get_type ())
gboolean jsc_value_is_object (JSCValue *value);
gboolean jsc_value_is_function (JSCValue *value);
gboolean jsc_value_object_has_property (JSCValue *value, const char *name);
JSCValue *jsc_value_object_get_property (JSCValue *value, const char *name);
void jsc_value_object_define_property_data (JSCValue *value, const char *property_name, JSCValuePropertyFlags flags,
                                            JSCValue *property_value);
JSCValue *jsc_value_new_function (JSCContext *context, const char *name, GCallback callback, gpointer user_data,
                                  GDestroyNotify destroy_notify, GType return_type, guint n_params, ...);
JSCValue *jsc_value_function_call (JSCValue *value, GType first_parameter_type, ...);
JSCValue *jsc_value_new_from_json (JSCContext *context, const char *json);
char *jsc_value_to_json (JSCValue *value, guint indent);
JSCValue *jsc_context_get_global_object (JSCContext *context);
JSCValue *jsc_context_evaluate_with_source_uri (JSCContext *context, const char *code, gssize length, const char *uri,
                                                guint line_number);
JSCException *jsc_context_get_exception (JSCContext *context);
void jsc_context_clear_exception (JSCContext *context);
guint jsc_exception_get_line_number (JSCException *exception);
#endif
'''
JSBASE_STUB = '''#ifndef STUB_JSBASE_H
#define STUB_JSBASE_H
typedef struct OpaqueJSContext *JSGlobalContextRef;
typedef const struct OpaqueJSValue *JSValueRef;
#endif
'''


def expand_ifdefs(line):
    s = re.sub(r"(ENABLE|USE)\(([A-Z0-9_]+)\)", r"(defined(\1_\2) && \1_\2)", line)
    return re.sub(r"(PLATFORM)\(([A-Z0-9_]+)\)", r"(defined(WTF_\1_\2) && WTF_\1_\2)", s)


def generate_api_header(template, output):
    data = ""
    with open(template, encoding="utf-8") as fd:
        for line in fd:
            if line.startswith("@"):
                if line.strip() == "@API_SINGLE_HEADER_CHECK@":
                    data += API_SINGLE_HEADER_CHECK + "\n"
                elif line.strip() == "@SHARED_API_SINGLE_HEADER_CHECK@":
                    data += SHARED_API_SINGLE_HEADER_CHECK + "\n"
                elif line.strip() == "@INJECTED_BUNDLE_API_SINGLE_HEADER_CHECK@":
                    data += SHARED_API_SINGLE_HEADER_CHECK + "\n"
            elif line.startswith("#include <@"):
                data += line.replace("@API_INCLUDE_PREFIX@", "wpe")
            else:
                data += expand_ifdefs(line)
    with open(output, "w", encoding="utf-8") as fd:
        fd.write(data)


def version_from_cmake(src):
    """SET_PROJECT_VERSION(major minor micro) in Source/cmake/OptionsWPE.cmake (VersioningUtils.cmake:1-4)."""
    text = open(os.path.join(src, "Source", "cmake", "OptionsWPE.cmake"), encoding="utf-8").read()
    m = re.search(r"SET_PROJECT_VERSION\(\s*(\d+)\s+(\d+)\s+(\d+)\s*\)", text)
    if not m:
        sys.exit("cannot find SET_PROJECT_VERSION in OptionsWPE.cmake")
    return m.group(1), m.group(2), m.group(3)


def configure_file(template, output, substitutions, cmakedefines=()):
    text = open(template, encoding="utf-8").read()
    for key, value in substitutions.items():
        text = text.replace("@%s@" % key, value)
    for name in cmakedefines:
        text = text.replace("#cmakedefine %s" % name, "#define %s 1" % name)
    text = re.sub(r"^#cmakedefine (\w+).*$", r"/* #undef \1 */", text, flags=re.M)
    with open(output, "w", encoding="utf-8") as fd:
        fd.write(text)


def mkenums(template, headers, output, seds):
    data = subprocess.run(["glib-mkenums", "--template", template] + headers,
                          check=True, stdout=subprocess.PIPE, encoding="utf-8").stdout
    for pattern, repl in seds:
        data = data.replace(pattern, repl)
    with open(output, "w", encoding="utf-8") as fd:
        fd.write(data)


def main(src, out, expect_version=None):
    webkit_dir = os.path.join(src, "Source", "WebKit")
    glib_api = os.path.join(webkit_dir, "UIProcess", "API", "glib")
    wpe_api = os.path.join(webkit_dir, "UIProcess", "API", "wpe")
    platform = os.path.join(webkit_dir, "WPEPlatform", "wpe")
    major, minor, micro = version_from_cmake(src)
    version = "%s.%s.%s" % (major, minor, micro)
    if expect_version is not None and version != expect_version:
        sys.exit("source tree is WPE %s, expected %s" % (version, expect_version))

    wk_out = os.path.join(out, "wpe-webkit-2.0", "wpe")
    plat_out = os.path.join(out, "wpe-webkit-2.0", "wpe-platform", "wpe")
    stubs = os.path.join(out, "stubs")
    for d in (wk_out, plat_out, os.path.join(stubs, "libsoup"), os.path.join(stubs, "jsc"),
              os.path.join(stubs, "JavaScriptCore")):
        os.makedirs(d, exist_ok=True)

    # 1. wpe-webkit-2.0 API headers from the .h.in templates (PlatformWPE.cmake:163-246)
    cmake = open(os.path.join(webkit_dir, "PlatformWPE.cmake"), encoding="utf-8").read()
    templates = sorted(set(re.findall(r"UIProcess/API/glib/(\w+\.h\.in)", cmake)))
    generated = []
    for name in templates:
        output = os.path.join(wk_out, name[:-3])
        generate_api_header(os.path.join(glib_api, name), output)
        generated.append(output)
    for name in ("WebKitColor.h", "WebKitRectangle.h"):
        shutil.copy(os.path.join(wpe_api, name), wk_out)
    configure_file(os.path.join(wpe_api, "WebKitVersion.h.in"), os.path.join(wk_out, "WebKitVersion.h"),
                   {"PROJECT_VERSION_MAJOR": major, "PROJECT_VERSION_MINOR": minor, "PROJECT_VERSION_MICRO": micro})
    enum_inputs = [h for h in generated if not h.endswith(("webkit.h", "WebKitAutocleanups.h", "WebKitEnumTypes.h"))]
    mkenums(os.path.join(wpe_api, "WebKitEnumTypes.h.in"), enum_inputs, os.path.join(wk_out, "WebKitEnumTypes.h"),
            [("web_kit", "webkit"), ("WEBKIT_TYPE_KIT", "WEBKIT_TYPE")])

    # 1b. the web process extension API (PlatformWPE.cmake:266-296 with ENABLE_2022_GLIB_API, 363-380), for the
    #     library's web process extension (0.8.0); WebKitWebExtension*.h.in are the old API's
    bundle_api = os.path.join(webkit_dir, "WebProcess", "InjectedBundle", "API", "glib")
    bundle_templates = sorted(set(re.findall(r"WebProcess/InjectedBundle/API/glib/([\w-]+\.h\.in)", cmake)))
    bundle_generated = []
    for name in bundle_templates:
        if name.startswith("WebKitWebExtension"):
            continue
        output = os.path.join(wk_out, name[:-3])
        generate_api_header(os.path.join(bundle_api, name), output)
        bundle_generated.append(output)
    bundle_enum_inputs = [h for h in bundle_generated if not h.endswith("webkit-web-process-extension.h")]
    mkenums(os.path.join(webkit_dir, "WebProcess", "InjectedBundle", "API", "wpe", "WebKitWebProcessEnumTypesWPE2.h.in"),
            bundle_enum_inputs, os.path.join(wk_out, "WebKitWebProcessEnumTypes.h"),
            [("web_kit", "webkit"), ("WEBKIT_TYPE_KIT", "WEBKIT_TYPE")])
    generated += bundle_generated

    # 2. wpe-platform-2.0 headers (WPEPlatform/CMakeLists.txt:48-80)
    plat_cmake = open(os.path.join(webkit_dir, "WPEPlatform", "CMakeLists.txt"), encoding="utf-8").read()
    installed = re.search(r"set\(WPEPlatform_INSTALLED_HEADERS(.*?)\)", plat_cmake, re.S).group(1)
    plat_headers = []
    for name in re.findall(r"WPEPlatform/wpe/(\w+\.h)", installed):
        shutil.copy(os.path.join(platform, name), plat_out)
        plat_headers.append(os.path.join(plat_out, name))
    shutil.copy(os.path.join(platform, "wpe-platform.h"), plat_out)
    configure_file(os.path.join(platform, "WPEVersion.h.in"), os.path.join(plat_out, "WPEVersion.h"),
                   {"PROJECT_VERSION_MAJOR": major, "PROJECT_VERSION_MINOR": minor, "PROJECT_VERSION_MICRO": micro})
    configure_file(os.path.join(platform, "WPEConfig.h.in"), os.path.join(plat_out, "WPEConfig.h"), {},
                   cmakedefines=("WPE_PLATFORM_DRM", "WPE_PLATFORM_HEADLESS"))
    mkenums(os.path.join(platform, "WPEEnumTypes.h.in"), plat_headers, os.path.join(plat_out, "WPEEnumTypes.h"),
            [("w_pe", "wpe"), ("WPE_TYPE_PE", "WPE_TYPE"), ("WPE_TYPEEGL", "WPE_TYPE_EGL"), ("wpeegl", "wpe_egl")])

    # 3. stubs for libraries the host does not have
    open(os.path.join(stubs, "libsoup", "soup.h"), "w").write(SOUP_STUB)
    open(os.path.join(stubs, "jsc", "jsc.h"), "w").write(JSC_STUB)
    open(os.path.join(stubs, "JavaScriptCore", "JSBase.h"), "w").write(JSBASE_STUB)

    for name in ("WPEKeymap.h", "WPEInputMethodContext.h", "WPEBufferDMABuf.h", "WPEBufferSHM.h", "WPEEvent.h"):
        if not os.path.exists(os.path.join(plat_out, name)):
            sys.exit("expected installed header %s missing from WPEPlatform_INSTALLED_HEADERS" % name)
    with open(os.path.join(out, "VERSION"), "w", encoding="utf-8") as fd:
        fd.write(version + "\n")

    print("generated %d wpe-webkit headers and %d wpe-platform headers for WPE %s in %s"
          % (len(generated), len(plat_headers), version, out))
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    expected = None
    if len(args) >= 2 and args[0] == "--expect-version":
        expected = args[1]
        args = args[2:]
    if len(args) != 2:
        sys.exit(__doc__)
    sys.exit(main(args[0], args[1], expected))
