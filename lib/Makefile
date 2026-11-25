# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# libucwebview + ucwebview-test: a plain Makefile. Standalone builds and firmware or distribution builds (for example
# a Buildroot generic package) call it the same way; a cross build passes CC/CFLAGS/LDFLAGS/PKG_CONFIG on the make
# command line, which overrides assignments in this file, so the flags of this file are collected in ALL_CFLAGS
# instead of being appended to CFLAGS.
#
# Targets:
#   make lib              libucwebview.so.1 (needs wpe-webkit-2.0, wpe-platform-2.0, egl) and its web process
#                         extension libucwebview-extension.so (wpe-web-process-extension-2.0, library 0.8.0; since
#                         0.9.0 also the library's script world and device-host-shim.js, uc_ext_protocol.h)
#   make ucwebview-test   the test host (needs the library, egl, glesv2, libpng, libsoup-3.0)
#   make all              lib, plus ucwebview-test when WITH_TEST=1
#   make ucwebview.pc     the pkg-config file for PREFIX/libdir/includedir/VERSION
#   make check            host unit tests, GLib only (no WPE needed), and check-js when Node is installed
#   make check-js         device-host-shim.js (library 0.9.0) in Node's vm with a minimal DOM (Node 19 or newer:
#                         CustomEvent is a global since Node 19)
#   make check-asan       the ABI copy test (tests/test-abi.c), the rendered request queue
#                         (tests/test-rendered.c), the pinch tracker (tests/test-pinch.c), the request
#                         table (tests/test-requests.c), the IME state machine (tests/test-ime.c), the header,
#                         site, device and load error tests with AddressSanitizer and UBSan
#   make syntax-check     -fsyntax-only of the WPE sources against generated
#                         host headers, see tests/mk-host-headers.py
#   make syntax-check-negative  must FAIL: proves the syntax check bites
#   make check-wpe-fake   runs the screen/vsync observer glue against a fake
#                         WPEScreen/WPEScreenSyncObserver (same headers)
#   make ucwebview-test-gtk  the test host against WebKitGTK 4.1 (developer harness, see README.md "Host checks")
#   make install          DESTDIR/PREFIX install (lib, extension, header, .pc, test host)
#   make clean
#
# Variables (command line or environment):
#   CC, CFLAGS, LDFLAGS   compiler and flags; this file adds its warnings and defines in ALL_CFLAGS
#   PKG_CONFIG            pkg-config to use (default pkg-config); a cross build passes its sysroot wrapper
#   VERSION               the library version compiled in (UC_WEBVIEW_VERSION, which uc_webview_library_version()
#                         returns since 0.11.0) and written to ucwebview.pc
#                         (default 0.0.0-dev; release builds pass the tag without its "v", e.g. VERSION=0.11.0)
#   WITH_TEST             1 (default): `all` and `install` include ucwebview-test; 0: the library only
#   PREFIX                installation prefix (default /usr)
#   libdir, includedir, bindir  default $(PREFIX)/lib, $(PREFIX)/include, $(PREFIX)/bin
#   EXT_DIR               directory of the web process extension, compiled into the library
#                         (default $(libdir)/ucwebview/web-process-extensions)
#   DESTDIR               staging root for `make install` (default empty)
#   SOUP_CFLAGS, SOUP_LIBS  libsoup-3.0 flags of ucwebview-test (default from PKG_CONFIG); a host without the
#                         libsoup development files passes SOUP_CFLAGS=-I<sysroot>/usr/include/libsoup-3.0
#   WPE_HOST_INCLUDE      output directory of tests/mk-host-headers.py, for syntax-check, syntax-check-negative,
#                         check-wpe-fake and ucwebview-test-gtk
#   NODE                  Node.js for check-js (default node)
#   GTK_HOST_LIBDIR       directory of the WebKitGTK 4.1 runtime libraries for ucwebview-test-gtk
#                         (default /usr/lib/x86_64-linux-gnu)

PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr
libdir     ?= $(PREFIX)/lib
includedir ?= $(PREFIX)/include
bindir     ?= $(PREFIX)/bin
VERSION    ?= 0.0.0-dev
DESTDIR    ?=
WITH_TEST  ?= 1

SONAME     := libucwebview.so.1
LIB_PKGS   := wpe-webkit-2.0 wpe-platform-2.0 gio-2.0 egl
# The web process extension (origin headers, library 0.8.0; the library's script world and the device state, 0.9.0):
# WebKit's web processes load every .so of EXT_DIR, which the library sets on the default WebKitWebContext; the
# library is built with the same path.
EXTENSION  := libucwebview-extension.so
EXT_DIR    ?= $(libdir)/ucwebview/web-process-extensions
EXT_PKGS   := wpe-web-process-extension-2.0 gmodule-2.0
TEST_PKGS  := gio-2.0 egl
HOST_PKGS  := gio-2.0 egl glesv2 libpng
# libsoup-3.0: the HTTP server of the request modes (0.7.0). A build host without its development files passes
# SOUP_CFLAGS=-I<sysroot>/usr/include/libsoup-3.0 (the headers do not depend on the architecture).
SOUP_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libsoup-3.0)
SOUP_LIBS   ?= $(shell $(PKG_CONFIG) --libs libsoup-3.0)

WARNFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 \
             -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
             -Werror=int-conversion -Werror=return-type
ALL_CFLAGS  = $(CFLAGS) $(WARNFLAGS) -D_GNU_SOURCE -DUC_WEBVIEW_VERSION=\"$(VERSION)\"
ALL_LDFLAGS = $(LDFLAGS)

# Expanded lazily so that `make check` works on a host without WPE.
LIB_CFLAGS  = $(shell $(PKG_CONFIG) --cflags $(LIB_PKGS))
LIB_LIBS    = $(shell $(PKG_CONFIG) --libs $(LIB_PKGS))
TEST_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(TEST_PKGS))
TEST_LIBS   = $(shell $(PKG_CONFIG) --libs $(TEST_PKGS))
HOST_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(HOST_PKGS))
HOST_LIBS   = $(shell $(PKG_CONFIG) --libs $(HOST_PKGS))
EXT_CFLAGS  = $(shell $(PKG_CONFIG) --cflags $(EXT_PKGS))
EXT_LIBS    = $(shell $(PKG_CONFIG) --libs $(EXT_PKGS))
GMODULE_CFLAGS = $(shell $(PKG_CONFIG) --cflags gmodule-2.0)
GMODULE_LIBS   = $(shell $(PKG_CONFIG) --libs gmodule-2.0)

# GLib-only modules (host testable) and the WPE-dependent ones.
PURE_SRCS = uc_events.c uc_formats.c uc_ime_state.c uc_egl.c uc_uri_policy.c uc_refresh.c uc_vsync.c uc_trace.c \
            uc_abi.c uc_rendered.c uc_pinch.c uc_magnify.c uc_pdf_policy.c uc_requests.c uc_headers.c uc_site.c uc_device.c \
            uc_load_error.c uc_cache_model.c
WPE_SRCS  = uc_webview.c uc_wpe_display.c uc_wpe_screen.c uc_wpe_view.c uc_wpe_ime.c uc_wpe_gesture.c uc_requests_wk.c \
            uc_site_wk.c uc_device_wk.c uc_page_wk.c
LIB_SRCS  = $(WPE_SRCS) $(PURE_SRCS)
EXT_SRCS  = uc_extension.c uc_headers.c uc_ext_device.c uc_device.c
EXT_HDRS  = uc_headers.h uc_ext_device.h uc_ext_protocol.h uc_device.h uc_device_shim.h
HEADERS   = uc_webview.h uc_internal.h uc_events.h uc_formats.h uc_ime_state.h uc_egl.h uc_uri_policy.h \
            uc_refresh.h uc_vsync.h uc_trace.h uc_abi.h uc_rendered.h uc_pinch.h uc_magnify.h uc_pdf_policy.h \
            uc_requests.h uc_requests_wk.h uc_headers.h uc_site.h uc_site_wk.h uc_device.h uc_device_wk.h uc_device_shim.h \
            uc_page_wk.h uc_ext_protocol.h uc_ext_device.h uc_load_error.h uc_cache_model.h

.PHONY: all lib check check-js check-asan syntax-check syntax-check-negative check-wpe-fake ucwebview-test-gtk install clean

ifeq ($(WITH_TEST),1)
all: lib ucwebview-test
else
all: lib
endif

lib: $(SONAME) $(EXTENSION)

# The page script of the device API (library 0.9.0) as the bytes of a C array with a trailing NUL, for the web process
# extension (uc_ext_device.c): od is in every build environment, and bytes need no escaping. The file is ASCII only
# (tests/test-device-shim.mjs checks it, tests/test-device.c compares the bytes with the file).
uc_device_shim.h: device-host-shim.js
	{ echo '/* generated by the Makefile from device-host-shim.js: its bytes and a NUL */'; \
	  echo 'static const unsigned char uc_device_shim_source[] = {'; \
	  od -An -v -tx1 device-host-shim.js | sed -e 's/[0-9a-f][0-9a-f]/0x&,/g'; \
	  echo '0x00 };'; } > $@.tmp && mv $@.tmp $@

# Only uc_webview_* is exported (-fvisibility=hidden + UC_EXPORT); the GObject types stay private.
$(SONAME): $(LIB_SRCS) $(HEADERS)
	$(CC) $(ALL_CFLAGS) $(LIB_CFLAGS) -DUC_WEBVIEW_EXTENSION_DIR=\"$(EXT_DIR)\" -fPIC -fvisibility=hidden -shared \
	    -Wl,-soname,$(SONAME) -Wl,--no-undefined -Wl,-z,defs -o $@ $(LIB_SRCS) $(ALL_LDFLAGS) $(LIB_LIBS)
	ln -sf $(SONAME) libucwebview.so

# Exports only webkit_web_process_extension_initialize_with_user_data (G_MODULE_EXPORT). The JavaScriptCore GLib API
# is part of libWPEWebKit-2.0.
$(EXTENSION): $(EXT_SRCS) $(EXT_HDRS)
	$(CC) $(ALL_CFLAGS) $(EXT_CFLAGS) -fPIC -fvisibility=hidden -shared -Wl,--no-undefined -Wl,-z,defs -o $@ \
	    $(EXT_SRCS) $(ALL_LDFLAGS) $(EXT_LIBS)

# The test host is a consumer of uc_webview.h: it links the library like remote-ui would dlopen it. It compiles
# the URI log rules in (uc_uri_policy.c, GLib only; the library's own copy is not exported) so that its output
# follows the library's logging policy. --link-test lives in ucwebview-test-links.c (GIO's socket service, no libsoup).
# --ime-swap-test (0.7.5) lives in ucwebview-test-ime.c, the same way.
# --site-test (0.8.0) lives in ucwebview-test-site.c (libsoup servers, like the request modes); it compiles uc_site.c
# in (GLib only) for the user agent token it expects (0.8.2).
# --device-test (0.9.0) lives in ucwebview-test-device.c (pages from load_html(), no server).
# --load-fail-test (0.10.0) lives in ucwebview-test-loadfail.c (a libsoup server, like the request modes).
# --process-test (0.10.4) lives in ucwebview-test-process.c (a libsoup server); it compiles uc_cache_model.c in (GLib
# only) to read UC_WEBVIEW_CACHE_MODEL like the library.
TOOL_SRCS = ucwebview-test.c ucwebview-test-requests.c ucwebview-test-links.c ucwebview-test-ime.c ucwebview-test-site.c \
            ucwebview-test-device.c ucwebview-test-loadfail.c ucwebview-test-process.c uc_uri_policy.c uc_site.c \
            uc_cache_model.c
TOOL_HDRS = ucwebview-test-requests.h ucwebview-test-links.h ucwebview-test-ime.h ucwebview-test-site.h \
            ucwebview-test-device.h ucwebview-test-loadfail.h ucwebview-test-process.h
ucwebview-test: $(TOOL_SRCS) $(TOOL_HDRS) uc_webview.h uc_uri_policy.h uc_site.h uc_cache_model.h $(SONAME)
	$(CC) $(ALL_CFLAGS) $(HOST_CFLAGS) $(SOUP_CFLAGS) -o $@ $(TOOL_SRCS) -L. -lucwebview -Wl,-rpath-link,. $(ALL_LDFLAGS) \
	    $(HOST_LIBS) $(SOUP_LIBS)

ucwebview.pc: ucwebview.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(libdir)|' -e 's|@INCLUDEDIR@|$(includedir)|' \
	    -e 's|@VERSION@|$(VERSION)|' $< > $@

# ---- host tests -------------------------------------------------------------

TESTS = tests/test-events tests/test-formats tests/test-ime tests/test-egl tests/test-uri-policy \
        tests/test-refresh tests/test-vsync tests/test-trace tests/test-abi tests/test-rendered tests/test-pinch \
        tests/test-magnify tests/test-pdf-policy tests/test-requests tests/test-headers tests/test-site tests/test-device \
        tests/test-load-error tests/test-cache-model

tests/test-events: tests/test-events.c uc_events.c uc_events.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-events.c uc_events.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-formats: tests/test-formats.c uc_formats.c uc_formats.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-formats.c uc_formats.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-ime: tests/test-ime.c uc_ime_state.c uc_ime_state.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-ime.c uc_ime_state.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-egl: tests/test-egl.c uc_egl.c uc_egl.h uc_formats.c
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-egl.c uc_egl.c uc_formats.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-uri-policy: tests/test-uri-policy.c uc_uri_policy.c uc_uri_policy.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-uri-policy.c uc_uri_policy.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-refresh: tests/test-refresh.c uc_refresh.c uc_refresh.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-refresh.c uc_refresh.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-vsync: tests/test-vsync.c uc_vsync.c uc_vsync.h uc_refresh.c uc_refresh.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-vsync.c uc_vsync.c uc_refresh.c $(ALL_LDFLAGS) $(TEST_LIBS)

TRACE_TEST_SRCS = tests/test-trace.c uc_trace.c uc_ime_state.c uc_requests.c uc_pdf_policy.c uc_uri_policy.c
tests/test-trace: $(TRACE_TEST_SRCS) uc_trace.h uc_ime_state.h uc_rendered.h uc_pinch.h uc_requests.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ $(TRACE_TEST_SRCS) $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-pinch: tests/test-pinch.c uc_pinch.c uc_pinch.h uc_events.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-pinch.c uc_pinch.c $(ALL_LDFLAGS) $(TEST_LIBS)

# The scripts also run in JavaScriptCore when the host has libjavascriptcoregtk (GModule, no build dependency).
tests/test-magnify: tests/test-magnify.c uc_magnify.c uc_magnify.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) $(GMODULE_CFLAGS) -I. -o $@ tests/test-magnify.c uc_magnify.c $(ALL_LDFLAGS) \
	    $(TEST_LIBS) $(GMODULE_LIBS)

# With WebKitGTK on the host the feature switch also runs against WebKit's feature list (GModule, no build dependency).
tests/test-pdf-policy: tests/test-pdf-policy.c uc_pdf_policy.c uc_pdf_policy.h uc_uri_policy.c uc_uri_policy.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) $(GMODULE_CFLAGS) -I. -o $@ tests/test-pdf-policy.c uc_pdf_policy.c uc_uri_policy.c \
	    $(ALL_LDFLAGS) $(TEST_LIBS) $(GMODULE_LIBS)

REQUESTS_TEST_SRCS = tests/test-requests.c uc_requests.c uc_pdf_policy.c uc_uri_policy.c
tests/test-requests: $(REQUESTS_TEST_SRCS) uc_requests.h uc_pdf_policy.h uc_uri_policy.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ $(REQUESTS_TEST_SRCS) $(ALL_LDFLAGS) $(TEST_LIBS)

# Origin headers (0.8.0), shared by the library and its web process extension.
tests/test-headers: tests/test-headers.c uc_headers.c uc_headers.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-headers.c uc_headers.c $(ALL_LDFLAGS) $(TEST_LIBS)

# Hosts, certificate fingerprints, cookie checks, user agent presets, website data types (0.8.0).
tests/test-site: tests/test-site.c uc_site.c uc_site.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-site.c uc_site.c $(ALL_LDFLAGS) $(TEST_LIBS)

# Device state for web pages (0.9.0): the state JSON, the start script, the changes and their JSON, the compiled-in
# shim; with WebKitGTK on the host also the throttling features in WebKit's own list (GModule, no build dependency).
tests/test-device: tests/test-device.c uc_device.c uc_device.h uc_device_shim.h uc_ext_protocol.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) $(GMODULE_CFLAGS) -I. -o $@ tests/test-device.c uc_device.c $(ALL_LDFLAGS) \
	    $(TEST_LIBS) $(GMODULE_LIBS)
tests/test-device-asan: tests/test-device.c uc_device.c uc_device.h uc_device_shim.h uc_ext_protocol.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) $(GMODULE_CFLAGS) -I. -o $@ tests/test-device.c uc_device.c \
	    $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS) $(GMODULE_LIBS)

# Why a main-frame load failed (0.10.0): the GError of load-failed as a UC_WEBVIEW_FAIL_* reason; with WebKitGTK on
# the host also WebKit's network error domain string (GModule, no build dependency).
tests/test-load-error: tests/test-load-error.c uc_load_error.c uc_load_error.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) $(GMODULE_CFLAGS) -I. -o $@ tests/test-load-error.c uc_load_error.c $(ALL_LDFLAGS) \
	    $(TEST_LIBS) $(GMODULE_LIBS)

# WebKit's cache model and UC_WEBVIEW_CACHE_MODEL (0.10.4).
tests/test-cache-model: tests/test-cache-model.c uc_cache_model.c uc_cache_model.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-cache-model.c uc_cache_model.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-abi: tests/test-abi.c uc_abi.c uc_abi.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-abi.c uc_abi.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-rendered: tests/test-rendered.c uc_rendered.c uc_rendered.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-rendered.c uc_rendered.c $(ALL_LDFLAGS) $(TEST_LIBS)

# The same test with AddressSanitizer and UBSan: its heap cases hold the host's struct in a block of exactly its
# size, so ASan reports an over-read where no guard page is (ASan does not watch mmap()ed memory).
ASAN_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
tests/test-abi-asan: tests/test-abi.c uc_abi.c uc_abi.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-abi.c uc_abi.c $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS)

# The request queue of uc_webview_when_rendered() frees requests while their callbacks add and end others.
tests/test-rendered-asan: tests/test-rendered.c uc_rendered.c uc_rendered.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-rendered.c uc_rendered.c $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS)

# The pinch tracker keeps its fingers in a fixed array and removes them by swapping.
tests/test-pinch-asan: tests/test-pinch.c uc_pinch.c uc_pinch.h uc_events.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-pinch.c uc_pinch.c $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS)

# The request table hands requests out while callbacks answer and end others (every request ends once).
tests/test-requests-asan: $(REQUESTS_TEST_SRCS) uc_requests.h uc_pdf_policy.h uc_uri_policy.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) -I. -o $@ $(REQUESTS_TEST_SRCS) $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS)

# The IME state machine with its pre-edit buffer and the focus reports of 0.7.5.
tests/test-ime-asan: tests/test-ime.c uc_ime_state.c uc_ime_state.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-ime.c uc_ime_state.c $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS)

# The header table and its (de)serialization, the TLS exception table and the cookie checks (0.8.0).
tests/test-headers-asan: tests/test-headers.c uc_headers.c uc_headers.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-headers.c uc_headers.c $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS)
tests/test-site-asan: tests/test-site.c uc_site.c uc_site.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-site.c uc_site.c $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS)

# The loop guard of the loads the library repeats keeps their URI (0.10.3).
tests/test-load-error-asan: tests/test-load-error.c uc_load_error.c uc_load_error.h uc_webview.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(TEST_CFLAGS) $(GMODULE_CFLAGS) -I. -o $@ tests/test-load-error.c uc_load_error.c \
	    $(ALL_LDFLAGS) $(ASAN_FLAGS) $(TEST_LIBS) $(GMODULE_LIBS)

check-asan: tests/test-abi-asan tests/test-rendered-asan tests/test-pinch-asan tests/test-requests-asan tests/test-ime-asan \
            tests/test-headers-asan tests/test-site-asan tests/test-device-asan tests/test-load-error-asan
	./tests/test-abi-asan
	./tests/test-rendered-asan
	./tests/test-pinch-asan
	./tests/test-requests-asan
	./tests/test-ime-asan
	./tests/test-headers-asan
	./tests/test-site-asan
	./tests/test-device-asan
	./tests/test-load-error-asan

NODE ?= node
check-js:
	$(NODE) tests/test-device-shim.mjs

check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done
	@if ! command -v $(NODE) >/dev/null 2>&1; then echo "== tests/test-device-shim.mjs skipped (no $(NODE))"; \
	elif ! $(NODE) -e 'process.exit(typeof CustomEvent === "function" ? 0 : 1)'; then \
	    echo "== tests/test-device-shim.mjs skipped ($(NODE) $$($(NODE) --version) has no global CustomEvent: Node 19 or newer)"; \
	else echo "== tests/test-device-shim.mjs"; $(NODE) tests/test-device-shim.mjs || exit 1; fi
	@echo "header is plain C: compiling uc_webview.h as C89/C++"
	echo '#include "uc_webview.h"' | $(CC) -std=c89 -pedantic -Wall -Wextra -Werror -I. -fsyntax-only -x c -
	echo '#include "uc_webview.h"' | $(CC) -std=c++11 -Wall -Wextra -Werror -I. -fsyntax-only -x c++ -

# WPE_HOST_INCLUDE: directory produced by tests/mk-host-headers.py <wpewebkit-src> <dir>
WPE_HOST_INCLUDE ?= /nonexistent
SYNTAX_DEFS = -DWTF_PLATFORM_GTK=0 -DWTF_PLATFORM_WPE=1 -DUSE_GTK4=0 \
              -DENABLE_2022_GLIB_API=1 -DENABLE_WPE_PLATFORM=1 -DUSE_LIBWPE=0
SYNTAX_INCLUDES = -I$(WPE_HOST_INCLUDE)/wpe-webkit-2.0 -I$(WPE_HOST_INCLUDE)/wpe-webkit-2.0/wpe-platform \
                  -I$(WPE_HOST_INCLUDE)/stubs
syntax-check: $(LIB_SRCS) $(EXT_SRCS) $(TOOL_SRCS) $(TOOL_HDRS) $(HEADERS)
	$(CC) $(ALL_CFLAGS) $(SYNTAX_DEFS) $(TEST_CFLAGS) $(SYNTAX_INCLUDES) -fvisibility=hidden -fsyntax-only $(LIB_SRCS)
	$(CC) $(ALL_CFLAGS) $(SYNTAX_DEFS) $(TEST_CFLAGS) $(SYNTAX_INCLUDES) -fvisibility=hidden -fsyntax-only $(EXT_SRCS)
	$(CC) $(ALL_CFLAGS) $(HOST_CFLAGS) $(SOUP_CFLAGS) -fsyntax-only $(TOOL_SRCS)

# The screen and its vsync observer (uc_wpe_screen.c) run against a fake of WPEScreen/WPEScreenSyncObserver
# (tests/fake-wpe-screen.c), compiled with the same generated headers: runtime coverage of the WPE glue on a host
# without WPE.
WPE_FAKE_SRCS = tests/test-wpe-screen.c tests/fake-wpe-screen.c uc_wpe_screen.c uc_vsync.c uc_refresh.c
tests/test-wpe-screen: $(WPE_FAKE_SRCS) $(HEADERS)
	$(CC) $(ALL_CFLAGS) $(SYNTAX_DEFS) $(TEST_CFLAGS) $(SYNTAX_INCLUDES) -I. -o $@ $(WPE_FAKE_SRCS) $(ALL_LDFLAGS) $(TEST_LIBS)

check-wpe-fake: tests/test-wpe-screen
	./tests/test-wpe-screen

# The request modes of ucwebview-test on a build host: the tool against tests/webkitgtk-host.c, which implements
# uc_webview.h on WebKitGTK 2.54 (libwebkit2gtk-4.1) with the library's own request code compiled in, against the
# generated WPE headers (WebKitGTK shares that API). Needs WPE_HOST_INCLUDE, the WebKitGTK 4.1, GTK 3 and libsoup-3.0
# runtime libraries (no development files), SOUP_CFLAGS when libsoup-3.0's are missing, and a display for GTK
# (broadway: GDK_BACKEND=broadway with broadwayd). Run: ./tests/ucwebview-test-gtk --egl none --dialog-test
GTK_HOST_LIBDIR ?= /usr/lib/x86_64-linux-gnu
GTK_HOST_LIBS = $(GTK_HOST_LIBDIR)/libwebkit2gtk-4.1.so.0 $(GTK_HOST_LIBDIR)/libjavascriptcoregtk-4.1.so.0 \
                $(GTK_HOST_LIBDIR)/libgtk-3.so.0 $(GTK_HOST_LIBDIR)/libsoup-3.0.so.0
GTK_SHIM_SRCS = tests/webkitgtk-host.c uc_requests_wk.c uc_requests.c uc_pdf_policy.c uc_trace.c uc_abi.c uc_site_wk.c \
                uc_site.c uc_headers.c uc_device_wk.c uc_device.c uc_page_wk.c uc_rendered.c uc_load_error.c
# the web process extension for WebKitGTK 4.1's web process (uc_extension.c with its old entry point name)
GTK_EXT_DIR = $(CURDIR)/tests/gtk-ext
ucwebview-test-gtk: tests/ucwebview-test-gtk tests/gtk-ext/$(EXTENSION)
tests/ucwebview-test-gtk: $(GTK_SHIM_SRCS) $(TOOL_SRCS) $(HEADERS) $(TOOL_HDRS)
	rm -rf tests/gtk-obj && mkdir -p tests/gtk-obj
	for f in $(GTK_SHIM_SRCS); do \
	    $(CC) $(ALL_CFLAGS) $(SYNTAX_DEFS) $(TEST_CFLAGS) $(SYNTAX_INCLUDES) -DUC_HARNESS_EXTENSION_DIR=\"$(GTK_EXT_DIR)\" \
	    -I. -c $$f -o tests/gtk-obj/shim-$$(basename $$f .c).o || exit 1; done
	for f in $(filter-out $(GTK_SHIM_SRCS),$(TOOL_SRCS)); do \
	    $(CC) $(ALL_CFLAGS) $(HOST_CFLAGS) $(SOUP_CFLAGS) -I. -c $$f -o tests/gtk-obj/$$(basename $$f .c).o || exit 1; done
	$(CC) -o $@ tests/gtk-obj/*.o $(ALL_LDFLAGS) $(GTK_HOST_LIBS) $(HOST_LIBS)
tests/gtk-ext/$(EXTENSION): $(EXT_SRCS) $(EXT_HDRS)
	mkdir -p tests/gtk-ext
	$(CC) $(ALL_CFLAGS) $(SYNTAX_DEFS) $(TEST_CFLAGS) $(SYNTAX_INCLUDES) -DUC_EXTENSION_WEBKITGTK41 -I. -fPIC \
	    -fvisibility=hidden -shared -Wl,--no-undefined -o $@ $(EXT_SRCS) $(ALL_LDFLAGS) \
	    $(GTK_HOST_LIBDIR)/libwebkit2gtk-4.1.so.0 $(GTK_HOST_LIBDIR)/libjavascriptcoregtk-4.1.so.0 \
	    $(GTK_HOST_LIBDIR)/libsoup-3.0.so.0 $(TEST_LIBS)

# Deliberately wrong calls against the WPE/WebKit API; the target succeeds only if the compiler rejects them.
syntax-check-negative: tests/negative-probe.c
	@if $(CC) $(ALL_CFLAGS) $(SYNTAX_DEFS) $(TEST_CFLAGS) $(SYNTAX_INCLUDES) -I. -fsyntax-only tests/negative-probe.c 2>tests/negative-probe.log; then \
	    echo "negative probe compiled: the syntax check does not bite"; exit 1; \
	else \
	    echo "negative probe rejected as expected ($$(grep -c 'error:' tests/negative-probe.log) errors)"; fi

install: $(SONAME) $(EXTENSION) ucwebview.pc
	install -D -m 0755 $(SONAME) $(DESTDIR)$(libdir)/$(SONAME)
	install -D -m 0755 $(EXTENSION) $(DESTDIR)$(EXT_DIR)/$(EXTENSION)
	ln -sf $(SONAME) $(DESTDIR)$(libdir)/libucwebview.so
	install -D -m 0644 uc_webview.h $(DESTDIR)$(includedir)/ucwebview/uc_webview.h
	install -D -m 0644 ucwebview.pc $(DESTDIR)$(libdir)/pkgconfig/ucwebview.pc
ifeq ($(WITH_TEST),1)
	install -D -m 0755 ucwebview-test $(DESTDIR)$(bindir)/ucwebview-test
endif

clean:
	rm -f $(SONAME) libucwebview.so $(EXTENSION) ucwebview-test ucwebview.pc $(TESTS) tests/test-abi-asan tests/test-rendered-asan \
	    tests/test-pinch-asan tests/test-requests-asan tests/test-ime-asan tests/test-headers-asan tests/test-site-asan \
	    tests/test-device-asan tests/test-load-error-asan uc_device_shim.h uc_device_shim.h.tmp \
	    tests/test-wpe-screen tests/ucwebview-test-gtk \
	    tests/negative-probe.log
	rm -rf tests/gtk-obj tests/gtk-ext
