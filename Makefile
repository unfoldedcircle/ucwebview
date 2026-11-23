# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Convenience targets over lib/ (libucwebview, ucwebview-test) and launcher/ (wpe-launcher). Firmware and
# distribution builds call the two sub-Makefiles directly (make -C lib ..., make -C launcher ...); every variable of
# those (CC, CFLAGS, PKG_CONFIG, VERSION, PREFIX, ...) passes through. CI runs the targets below.
#
#   make                  build the library, ucwebview-test and wpe-launcher (needs WPE WebKit 2.54 with WPEPlatform)
#   make check            host unit tests of lib/ and launcher/ (GLib only, no WPE needed)
#   make check-asan       lib/'s tests with AddressSanitizer and UBSan
#   make wpe-headers      the public headers of WPE WebKit $(WPE_VERSION) in $(WPE_HOST_INCLUDE), generated from the
#                         release tarball (downloaded once into .cache/, SHA-256 checked)
#   make syntax-check     wpe-headers, then -fsyntax-only of every WPE source of lib/ and launcher/ against them,
#                         syntax-check-negative (must be rejected) and check-wpe-fake
#   make lint             SPDX headers, white space, shellcheck
#   make install          lib/ and launcher/ install (DESTDIR, PREFIX)
#   make clean
#
# Variables: WPE_VERSION (default 2.54.2), WPE_HOST_INCLUDE (default .cache/wpe-host-include-$(WPE_VERSION)),
# SHELLCHECK (default shellcheck); see scripts/wpe-host-headers.sh for WPE_SHA256, WPE_CACHE_DIR, WPE_TARBALL.

WPE_VERSION      ?= 2.54.2
WPE_HOST_INCLUDE ?= $(CURDIR)/.cache/wpe-host-include-$(WPE_VERSION)
SHELLCHECK       ?= shellcheck

export WPE_VERSION

.PHONY: all check check-asan wpe-headers syntax-check lint install clean

all:
	$(MAKE) -C lib all
	$(MAKE) -C launcher all

check:
	$(MAKE) -C lib check
	$(MAKE) -C launcher check

check-asan:
	$(MAKE) -C lib check-asan

wpe-headers:
	./scripts/wpe-host-headers.sh $(WPE_HOST_INCLUDE)

syntax-check: wpe-headers
	$(MAKE) -C lib syntax-check WPE_HOST_INCLUDE=$(WPE_HOST_INCLUDE)
	$(MAKE) -C lib syntax-check-negative WPE_HOST_INCLUDE=$(WPE_HOST_INCLUDE)
	$(MAKE) -C lib check-wpe-fake WPE_HOST_INCLUDE=$(WPE_HOST_INCLUDE)
	$(MAKE) -C launcher syntax-check WPE_HOST_INCLUDE=$(WPE_HOST_INCLUDE)

lint:
	./scripts/check-spdx.sh
	./scripts/check-whitespace.sh
	$(SHELLCHECK) scripts/*.sh

install:
	$(MAKE) -C lib install
	$(MAKE) -C launcher install

clean:
	$(MAKE) -C lib clean
	$(MAKE) -C launcher clean
