# CI level 2: build, link and run against WPE WebKit

Level 1 of the CI (jobs `unit`, `syntax`, `flutter-pi`, `lint` in [.github/workflows/ci.yml](../.github/workflows/ci.yml))
needs no WPE WebKit: it compiles every WPE source with `-fsyntax-only` against headers generated from the release
tarball, tests the flutter-pi plugin against a fake library and builds it into flutter-pi, but it neither links the
library nor runs it. Level 2 does: it builds `lib/` and `launcher/` against a real WPE WebKit,
installs them and runs `ucwebview-test` and `wpe-launcher` on it.

## Which distributions ship WPE WebKit with WPEPlatform

The library needs WPE WebKit 2.54 (it calls API that is new in 2.54, such as `webkit_web_view_set_magnification()`)
built with the WPEPlatform API: the pkg-config modules `wpe-webkit-2.0`, `wpe-platform-2.0` and
`wpe-web-process-extension-2.0`; the launcher also uses the DRM and headless platforms. Survey of November 2026, from
the package indexes and by installing the packages in the distributions' official container images:

| Distribution | WPE WebKit | WPEPlatform | Result |
|--------------|------------|-------------|--------|
| Debian 13 trixie (stable) | 2.48.3 | | too old for the library |
| Debian forky (testing), sid (unstable) | 2.54.3 (`libwpewebkit-2.0-dev`) | `wpe-platform-2.0`, `-drm-2.0`, `-headless-2.0`, `-wayland-2.0`, `wpe-web-process-extension-2.0` | builds, all smoke tests pass |
| Debian experimental | none of its own (sid's 2.54.3) | | as sid |
| Ubuntu 25.10, 26.04 | none | | the package was removed in 22.10 ("Unmaintained web engine, security concerns", LP #1981592); 22.04 has 2.36 |
| Fedora 43, rawhide | none | | no `wpewebkit` package in the repositories |
| Arch Linux | 2.54.3 (`wpewebkit`, extra) | as Debian | builds, all smoke tests pass |
| Alpine edge | none | | no `wpewebkit` package |

## What the CI job does

The job `distro` runs [scripts/ci-distro.sh](../scripts/ci-distro.sh) in `debian:testing` and `archlinux:latest`
containers on GitHub's hosted runners:

1. installs the build and test dependencies from the distribution (`--deps`);
2. builds the library, its web process extension, `ucwebview-test` and the launcher with the distribution's compiler
   (GCC 16 at the time of the survey) and prints the number of compiler warnings (0);
3. checks the exports: only `uc_webview_*` from the library (51 functions), only
   `webkit_web_process_extension_initialize_with_user_data` from the extension;
4. installs everything under `/usr` (the extension directory is compiled into the library) and runs `make check`;
5. runs `ucwebview-test --selftest`, renders a page in software (shared memory frames, `--egl none`) and runs the
   self-checking modes `--ime-test`, `--ime-swap-test`, `--dialog-test`, `--auth-test`, `--window-test`,
   `--download-test`, `--device-test`, `--link-test`, `--site-test` (with a self-signed certificate generated for the
   run and a persistent data directory, twice), `--load-fail-test`, `--process-test` (with each of the three
   `UC_WEBVIEW_CACHE_MODEL` values), `--magnify`, `--pinch` (also with `--prevent-touch`) and `--zoom-test`;
6. runs `wpe-launcher --headless` on WPE's headless platform and `wpe-launcher --version`.

`--raf-test` is left out: it measures frame timing, which a shared CI machine does not keep. Distribution builds of
WebKit enable the bubblewrap sandbox, whose namespaces a container cannot create, so the script sets
`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` for the test runs; the pages are the test's own, served on 127.0.0.1.

Run it locally the same way (it installs into the container's `/usr`):

```shell
docker run --rm -v "$PWD":/src:ro debian:testing sh -c 'cp -a /src /build && /build/scripts/ci-distro.sh --deps'
docker run --rm -v "$PWD":/src:ro archlinux:latest sh -c 'cp -a /src /build && /build/scripts/ci-distro.sh --deps'
```

`UC_SMOKE_MODES` selects other `ucwebview-test` runs (one command line per line), `UC_SMOKE_TIMEOUT` sets the timeout
per run (300 s). A full run takes about five minutes, most of it `--link-test` (about two minutes) and the dependency
installation.

### Switching it off

The two distributions are rolling: a new WebKit, GLib or compiler can break the job without a change here. So the job
is separate from level 1 and should not be a required status check. To skip it, set the repository variable
`CI_DISTRO_BUILD` to `false` (Settings, Secrets and variables, Actions, Variables); to drop one distribution, remove
it from the job's `matrix.image`.

### What it does not cover

- The firmware's WebKit release and build options: the distributions have 2.54.3 with their own options (bubblewrap
  sandbox, multimedia), the remotes 2.54.1 and 2.54.2. The syntax check of level 1 pins the headers of 2.54.2.
- The DMA-BUF path: without a GPU the web process renders in shared memory, so the EGLImage import, buffer
  modifiers and the vsync pacing on a real panel run only on a device ([lib/README.md](../lib/README.md#checks-on-a-device)).
- The launcher on the DRM platform (needs a KMS device and DRM master).
- aarch64.

## Alternative: a cached container with the firmware's WPE WebKit

If the distributions stop shipping a usable WPE WebKit, or the CI must test exactly the release the firmware ships,
the job can run in a container image with WPE WebKit built from the release tarball. Proposal, not implemented:

- A workflow `wpe-image.yml`, started by hand or when a file `ci/wpe-version` changes, builds an image
  `ghcr.io/unfoldedcircle/ucwebview-ci:wpe-<version>` from a Dockerfile on a Debian base: the build dependencies of
  WebKit, the tarball fetched and checked with the SHA-256 of `wpewebkit-<version>.tar.xz.sums` (as
  `scripts/wpe-host-headers.sh` does), configured close to the firmware (`-DPORT=WPE -DENABLE_WPE_PLATFORM=ON
  -DENABLE_WPE_PLATFORM_DRM=ON -DENABLE_WPE_PLATFORM_HEADLESS=ON -DENABLE_WPE_PLATFORM_WAYLAND=OFF
  -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DUSE_GSTREAMER=OFF -DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF
  -DCMAKE_BUILD_TYPE=Release`), installed under `/usr` and stripped; optionally with the patches of
  `webkit-patches/`. The image keeps the runtime libraries and the development files, not the build tree.
- The `distro` job gets a matrix entry with that image; `ci-distro.sh` then skips `--deps` (or gets a branch for the
  image's package manager).
- Estimates, from cross builds of the same release for aarch64: the `wpewebkit` package alone took 30 to 32 minutes
  at 8 to 10 parallel jobs on a 30-core host, and each compiler job needs about 2 GB of RAM. GitHub's standard
  runner (4 vCPUs, 16 GB) therefore builds with 4 jobs in roughly 1.5 to 2 hours, inside the 6-hour job limit, once
  per WPE version (ccache does not help a one-off build). The unstripped `libWPEWebKit-2.0.so` is about 160 MB, the
  installed tree about 40 MB as `tar.xz`; stripped, the library shrinks to a fraction of that. The image would be
  about 0.8 to 1 GB with the runtime and development packages, stored in the GitHub Container Registry
  of the repository; the CI jobs pull it in under a minute.
- Costs: the Dockerfile and its WebKit dependency list need care at every WPE version, and a build that fails after
  an hour is slow to debug. The distribution packages avoid all of that while they ship 2.54 or newer.
