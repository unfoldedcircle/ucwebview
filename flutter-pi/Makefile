# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Host tests of the flutter-pi plugin. The plugin itself is built by flutter-pi's CMake (uc_webview.cmake), not here.
#
#   make check            the core's unit tests (GLib only) and the GLib loop test (GLib and libsystemd)
#   make check-asan       the same with AddressSanitizer and UBSan
#   make flutter-pi-build FLUTTER_PI_SRC=<flutter-pi checkout>
#                         builds flutter-pi with the plugin on the host (scripts/build-flutter-pi.sh)
#   make clean
#
# Variables: CC, CFLAGS, LDFLAGS, PKG_CONFIG; FLUTTER_PI_SRC, FLUTTER_PI_BUILD (default .cache/flutter-pi-build)

PKG_CONFIG ?= pkg-config
WARNFLAGS  := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 \
              -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
              -Werror=int-conversion -Werror=return-type
ALL_CFLAGS  = $(CFLAGS) $(WARNFLAGS) -D_GNU_SOURCE -Icore -I../lib -Iflutterpi -DFIXTURES_DIR=\"$(CURDIR)/tests/fixtures\" \
              -DFAKE_DIR=\"$(CURDIR)/tests\"
GLIB_CFLAGS = $(shell $(PKG_CONFIG) --cflags glib-2.0)
GLIB_LIBS   = $(shell $(PKG_CONFIG) --libs glib-2.0)
SD_CFLAGS   = $(shell $(PKG_CONFIG) --cflags libsystemd)
SD_LIBS     = $(shell $(PKG_CONFIG) --libs libsystemd)
ASAN_FLAGS  = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g

CORE_HDRS = core/ucfl_codec.h core/ucfl_protocol.h core/ucfl_loader.h core/ucfl_views.h core/ucfl_dump.h \
            ../lib/uc_webview.h
FAKES     = tests/fake-ok.so tests/fake-old.so tests/fake-abi2.so tests/fake-incomplete.so
TESTS     = tests/test-codec tests/test-protocol tests/test-loader tests/test-views tests/test-dump tests/test-glib-loop

FLUTTER_PI_SRC   ?=
FLUTTER_PI_BUILD ?= $(CURDIR)/.cache/flutter-pi-build

.PHONY: check check-asan flutter-pi-build clean

tests/test-codec: tests/test-codec.c core/ucfl_codec.c $(CORE_HDRS)
	$(CC) $(ALL_CFLAGS) $(GLIB_CFLAGS) -o $@ tests/test-codec.c core/ucfl_codec.c $(LDFLAGS) $(GLIB_LIBS)

tests/test-protocol: tests/test-protocol.c core/ucfl_protocol.c core/ucfl_codec.c $(CORE_HDRS)
	$(CC) $(ALL_CFLAGS) $(GLIB_CFLAGS) -o $@ tests/test-protocol.c core/ucfl_protocol.c core/ucfl_codec.c $(LDFLAGS) \
	    $(GLIB_LIBS) -lm

tests/test-loader: tests/test-loader.c core/ucfl_loader.c $(CORE_HDRS) $(FAKES)
	$(CC) $(ALL_CFLAGS) $(GLIB_CFLAGS) -o $@ tests/test-loader.c core/ucfl_loader.c $(LDFLAGS) $(GLIB_LIBS) -ldl

tests/test-views: tests/test-views.c core/ucfl_views.c core/ucfl_views.h
	$(CC) $(ALL_CFLAGS) $(GLIB_CFLAGS) -o $@ tests/test-views.c core/ucfl_views.c $(LDFLAGS) $(GLIB_LIBS)

tests/test-dump: tests/test-dump.c core/ucfl_dump.c core/ucfl_dump.h
	$(CC) $(ALL_CFLAGS) $(GLIB_CFLAGS) -o $@ tests/test-dump.c core/ucfl_dump.c $(LDFLAGS) $(GLIB_LIBS)

tests/test-glib-loop: tests/test-glib-loop.c flutterpi/ucfp_glib_loop.c flutterpi/ucfp_glib_loop.h
	$(CC) $(ALL_CFLAGS) $(GLIB_CFLAGS) $(SD_CFLAGS) -o $@ tests/test-glib-loop.c flutterpi/ucfp_glib_loop.c $(LDFLAGS) \
	    $(GLIB_LIBS) $(SD_LIBS)

# the fakes of libucwebview.so.1 (tests/fake-ucwebview.c)
FAKE_FLAGS = -fPIC -shared -fvisibility=hidden -Wl,--no-undefined
tests/fake-ok.so: tests/fake-ucwebview.c ../lib/uc_webview.h
	$(CC) $(ALL_CFLAGS) $(FAKE_FLAGS) -o $@ tests/fake-ucwebview.c $(LDFLAGS)
tests/fake-old.so: tests/fake-ucwebview.c ../lib/uc_webview.h
	$(CC) $(ALL_CFLAGS) $(FAKE_FLAGS) -DFAKE_OLD -o $@ tests/fake-ucwebview.c $(LDFLAGS)
tests/fake-abi2.so: tests/fake-ucwebview.c ../lib/uc_webview.h
	$(CC) $(ALL_CFLAGS) $(FAKE_FLAGS) -DFAKE_API_VERSION=2 -o $@ tests/fake-ucwebview.c $(LDFLAGS)
tests/fake-incomplete.so: tests/fake-ucwebview.c ../lib/uc_webview.h
	$(CC) $(ALL_CFLAGS) $(FAKE_FLAGS) -DFAKE_INCOMPLETE -o $@ tests/fake-ucwebview.c $(LDFLAGS)

check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# Built in one go per test with the sanitizers (the fakes stay uninstrumented: they are loaded, not tested).
check-asan: $(FAKES)
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(GLIB_CFLAGS) -o tests/test-codec-asan tests/test-codec.c core/ucfl_codec.c \
	    $(LDFLAGS) $(GLIB_LIBS)
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(GLIB_CFLAGS) -o tests/test-protocol-asan tests/test-protocol.c \
	    core/ucfl_protocol.c core/ucfl_codec.c $(LDFLAGS) $(GLIB_LIBS) -lm
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(GLIB_CFLAGS) -o tests/test-loader-asan tests/test-loader.c core/ucfl_loader.c \
	    $(LDFLAGS) $(GLIB_LIBS) -ldl
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(GLIB_CFLAGS) -o tests/test-dump-asan tests/test-dump.c core/ucfl_dump.c \
	    $(LDFLAGS) $(GLIB_LIBS)
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(GLIB_CFLAGS) $(SD_CFLAGS) -o tests/test-glib-loop-asan tests/test-glib-loop.c \
	    flutterpi/ucfp_glib_loop.c $(LDFLAGS) $(GLIB_LIBS) $(SD_LIBS)
	@for t in tests/test-codec-asan tests/test-protocol-asan tests/test-loader-asan tests/test-dump-asan \
	    tests/test-glib-loop-asan; do \
	    echo "== $$t"; ./$$t || exit 1; done

flutter-pi-build:
	@test -n "$(FLUTTER_PI_SRC)" || { echo "set FLUTTER_PI_SRC to a flutter-pi checkout"; exit 1; }
	./scripts/build-flutter-pi.sh "$(FLUTTER_PI_SRC)" "$(FLUTTER_PI_BUILD)"

clean:
	rm -f $(TESTS) $(FAKES) tests/*-asan
	rm -rf .cache
