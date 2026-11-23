# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# wpe-launcher: a plain Makefile. Standalone builds and firmware or distribution builds (for example a Buildroot
# generic package) call it the same way; a cross build passes CC/CFLAGS/LDFLAGS/PKG_CONFIG on the make command line,
# which overrides assignments in this file, so the flags of this file are collected in ALL_CFLAGS instead of being
# appended to CFLAGS.
#
# Targets:
#   make wpe-launcher     the launcher (needs wpe-webkit-2.0 and wpe-platform-2.0); `make all` is the same
#   make check            host unit tests, GLib/GIO only (no WPE needed; tests/test-dbus starts a private bus with
#                         dbus-daemon)
#   make syntax-check     -fsyntax-only of the WPE sources against generated
#                         host headers, see ../lib/tests/mk-host-headers.py
#   make install          DESTDIR/PREFIX install of the binary
#   make clean
#
# Variables (command line or environment):
#   CC, CFLAGS, LDFLAGS   compiler and flags; this file adds its warnings and defines in ALL_CFLAGS
#   PKG_CONFIG            pkg-config to use (default pkg-config); a cross build passes its sysroot wrapper
#   VERSION               the version `wpe-launcher --version` prints (default 0.0.0-dev)
#   PREFIX, bindir        installation prefix and binary directory (default /usr and $(PREFIX)/bin)
#   DESTDIR               staging root for `make install` (default empty)
#   WPE_HOST_INCLUDE      output directory of ../lib/tests/mk-host-headers.py, for syntax-check

PKG_CONFIG ?= pkg-config
PREFIX     ?= /usr
bindir     ?= $(PREFIX)/bin
VERSION    ?= 0.0.0-dev
DESTDIR    ?=

PKGS      := wpe-webkit-2.0 wpe-platform-2.0 gio-2.0 gio-unix-2.0
TEST_PKGS := gio-2.0 gio-unix-2.0

WARNFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 \
             -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
             -Werror=int-conversion -Werror=return-type
ALL_CFLAGS  = $(CFLAGS) $(WARNFLAGS) -D_GNU_SOURCE -DUC_WPE_LAUNCHER_VERSION=\"$(VERSION)\"
ALL_LDFLAGS = $(LDFLAGS)

# Expanded lazily so that `make check` works on a host without WPE.
WPE_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PKGS))
WPE_LIBS   = $(shell $(PKG_CONFIG) --libs $(PKGS))
TEST_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(TEST_PKGS))
TEST_LIBS   = $(shell $(PKG_CONFIG) --libs $(TEST_PKGS))

COMMON_SRCS = launcher-config.c kiosk-dbus.c
LAUNCHER_SRCS = main.c $(COMMON_SRCS)

.PHONY: all check syntax-check install clean

all: wpe-launcher

wpe-launcher: $(LAUNCHER_SRCS) launcher-config.h kiosk-dbus.h
	$(CC) $(ALL_CFLAGS) $(WPE_CFLAGS) -o $@ $(LAUNCHER_SRCS) $(ALL_LDFLAGS) $(WPE_LIBS)

# ---- host tests -------------------------------------------------------------

tests/test-config: tests/test-config.c launcher-config.c launcher-config.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-config.c launcher-config.c $(ALL_LDFLAGS) $(TEST_LIBS)

tests/test-dbus: tests/test-dbus.c kiosk-dbus.c kiosk-dbus.h
	$(CC) $(ALL_CFLAGS) $(TEST_CFLAGS) -I. -o $@ tests/test-dbus.c kiosk-dbus.c $(ALL_LDFLAGS) $(TEST_LIBS)

check: tests/test-config tests/test-dbus
	./tests/test-config
	./tests/test-dbus

# WPE_HOST_INCLUDE: directory produced by ../lib/tests/mk-host-headers.py <wpewebkit-src> <dir>
WPE_HOST_INCLUDE ?= /nonexistent
SYNTAX_DEFS = -DWTF_PLATFORM_GTK=0 -DWTF_PLATFORM_WPE=1 -DUSE_GTK4=0 \
              -DENABLE_2022_GLIB_API=1 -DENABLE_WPE_PLATFORM=1 -DUSE_LIBWPE=0
syntax-check: $(LAUNCHER_SRCS)
	$(CC) $(ALL_CFLAGS) $(SYNTAX_DEFS) $(TEST_CFLAGS) \
	    -I$(WPE_HOST_INCLUDE)/wpe-webkit-2.0 -I$(WPE_HOST_INCLUDE)/wpe-webkit-2.0/wpe-platform \
	    -I$(WPE_HOST_INCLUDE)/stubs -fsyntax-only $(LAUNCHER_SRCS)

install: wpe-launcher
	install -D -m 0755 wpe-launcher $(DESTDIR)$(bindir)/wpe-launcher

clean:
	rm -f wpe-launcher tests/test-config tests/test-dbus
