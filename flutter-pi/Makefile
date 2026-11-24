# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Host tests of the flutter-pi plugin. The plugin itself is built by flutter-pi's CMake (uc_webview.cmake), not here.
#
#   make check            the core's unit tests (GLib only; the host test against fake libraries of tests/) and the
#                         GLib loop test (GLib and libsystemd)
#   make check-asan       the same with AddressSanitizer and UBSan
#   make fixtures         regenerates tests/fixtures/ with tests/fixtures/gen-fixtures.py (python3)
#   make flutter-pi-build FLUTTER_PI_SRC=<flutter-pi checkout>
#                         builds flutter-pi with the plugin on the host (scripts/build-flutter-pi.sh)
#   make clean
#
# Variables: CC, CFLAGS, LDFLAGS, PKG_CONFIG, PYTHON; FLUTTER_PI_SRC, FLUTTER_PI_BUILD (default .cache/flutter-pi-build)

PKG_CONFIG ?= pkg-config
PYTHON     ?= python3
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
            core/ucfl_pacing.h core/ucfl_host.h ../lib/uc_webview.h
FAKES     = tests/fake-ok.so tests/fake-old.so tests/fake-abi2.so tests/fake-incomplete.so \
            tests/fake-no-device-state.so
TESTS     = tests/test-codec tests/test-protocol tests/test-loader tests/test-views tests/test-dump tests/test-pacing \
            tests/test-host tests/test-glib-loop

# the sources of each test (the libraries are added by the rules below)
SRC_test-codec     = tests/test-codec.c core/ucfl_codec.c
SRC_test-protocol  = tests/test-protocol.c core/ucfl_protocol.c core/ucfl_codec.c
SRC_test-loader    = tests/test-loader.c core/ucfl_loader.c
SRC_test-views     = tests/test-views.c core/ucfl_views.c
SRC_test-dump      = tests/test-dump.c core/ucfl_dump.c
SRC_test-pacing    = tests/test-pacing.c core/ucfl_pacing.c
SRC_test-host      = tests/test-host.c core/ucfl_host.c core/ucfl_protocol.c core/ucfl_codec.c core/ucfl_loader.c \
                     core/ucfl_views.c
SRC_test-glib-loop = tests/test-glib-loop.c flutterpi/ucfp_glib_loop.c
# libraries beyond GLib
LIBS_test-protocol  = -lm
LIBS_test-loader    = -ldl
LIBS_test-host      = -ldl -lm
LIBS_test-glib-loop = $(SD_LIBS)
CFLAGS_test-glib-loop = $(SD_CFLAGS)

FLUTTER_PI_SRC   ?=
FLUTTER_PI_BUILD ?= $(CURDIR)/.cache/flutter-pi-build

.PHONY: check check-asan fixtures flutter-pi-build clean

.SECONDEXPANSION:
$(TESTS): tests/test-%: $$(SRC_test-%) $(CORE_HDRS) flutterpi/ucfp_glib_loop.h
	$(CC) $(ALL_CFLAGS) $(GLIB_CFLAGS) $(CFLAGS_test-$*) -o $@ $(SRC_test-$*) $(LDFLAGS) $(GLIB_LIBS) $(LIBS_test-$*)

$(addsuffix -asan,$(TESTS)): tests/test-%-asan: $$(SRC_test-%) $(CORE_HDRS) flutterpi/ucfp_glib_loop.h
	$(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $(GLIB_CFLAGS) $(CFLAGS_test-$*) -o $@ $(SRC_test-$*) $(LDFLAGS) $(GLIB_LIBS) \
	    $(LIBS_test-$*)

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
tests/fake-no-device-state.so: tests/fake-ucwebview.c ../lib/uc_webview.h
	$(CC) $(ALL_CFLAGS) $(FAKE_FLAGS) -DFAKE_NO_DEVICE_STATE -o $@ tests/fake-ucwebview.c $(LDFLAGS)

check: $(TESTS) $(FAKES)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# The fakes stay uninstrumented: they are loaded, not tested.
check-asan: $(addsuffix -asan,$(TESTS)) $(FAKES)
	@for t in $(addsuffix -asan,$(TESTS)); do echo "== $$t"; ./$$t || exit 1; done

fixtures:
	$(PYTHON) tests/fixtures/gen-fixtures.py tests/fixtures

flutter-pi-build:
	@test -n "$(FLUTTER_PI_SRC)" || { echo "set FLUTTER_PI_SRC to a flutter-pi checkout"; exit 1; }
	./scripts/build-flutter-pi.sh "$(FLUTTER_PI_SRC)" "$(FLUTTER_PI_BUILD)"

clean:
	rm -f $(TESTS) $(FAKES) tests/*-asan
	rm -rf .cache
