# Contributing

Thanks for taking the time to contribute!

Found a bug, a missing feature or a description that doesn't make sense or needs clarification? Please let us know.

### Bug reports

Search the [GitHub issues](https://github.com/unfoldedcircle/ucwebview/issues) first, and if it isn't already tracked,
create a new issue. Name the WPE WebKit version (`ucwebview-test` and `wpe-launcher --version` print it), the
distribution or firmware, and the host (remote-ui, `ucwebview-test`, your own). Log lines of the library are made to
be shared (they never contain typed text, dialog texts, header values or full URLs, see
[lib/README.md](lib/README.md#what-the-logs-carry)); still check what you paste.

⚠️ Please don't report security vulnerabilities as public issues: follow Unfolded Circle's
[security policy](https://github.com/unfoldedcircle/.github/blob/main/SECURITY.md) instead.

### Pull requests

**Any pull request needs to be reviewed and approved by the Unfolded Circle development team.**

If you plan functional changes or new features, please open an issue first and describe them. The library is part of
the firmware of the Remote Two and Remote 3, its ABI is a contract with hosts of other releases (see "ABI rules"
below), and unsolicited pull requests for functional changes may not be accepted.

1. Fork the repository and work on a feature branch (`feat/<short-description>`, `fix/<issue-number>`).
2. Contributed code must be licensed under the MIT license. Every new source file starts with the SPDX identifier and
   a copyright line, as the existing files do:

   ```c
   /*
    * SPDX-License-Identifier: MIT
    * Copyright (c) 2026 Unfolded Circle ApS
    */
   ```

   Files of other authors name their own copyright holder (`Copyright (c) <year> <name> <email>`). `make lint`
   checks the identifier and the copyright line.
3. Build and test locally (see the [README](README.md#test)):

   ```shell
   make check check-asan syntax-check lint
   ```

   Changes to WPE or WebKit code paths (`uc_webview.c`, `uc_wpe_*.c`, `*_wk.c`, `uc_extension.c`, the launcher) also
   need a run against a real WPE WebKit: `scripts/ci-distro.sh` in a container (CI level 2) or on a device.
4. Add a `CHANGELOG.md` entry for every user-visible change (see below).
5. Push to your fork and open a pull request. Pull requests with failing checks are not reviewed until they pass.

### Code style

- C11 with GLib; GLib/GObject style as in the existing files: four spaces, no tabs, a space before the parenthesis of
  a call (`g_strdup (s)`), the return type on its own line in definitions, braces on the same line except for
  functions. Lines up to 120 columns where practical. There is no formatter configuration; follow the surrounding
  code and don't reformat lines you don't change.
- Warnings are part of the build (`-Wall -Wextra -Wshadow -Wformat=2` and a few `-Werror=` flags); new code adds none.
- Plain logic goes into GLib-only modules with a unit test in `lib/tests/` (`make check`, and `make check-asan` for
  code that handles memory of the host or tables of requests); WPE and WebKit glue stays thin and is covered by the
  syntax check and by `ucwebview-test`.
- A non-obvious rule that comes from WebKit, WPEPlatform or Qt names its source file and line in a comment
  (`WPEBufferDMABuf.cpp:117-178`), against the release the code was written for.
- The library logs through GLib and never logs what a user types, dialog texts, credentials, header or cookie values,
  or more of a URI than its origin (see [lib/README.md](lib/README.md#what-the-logs-carry)); `tests/test-trace.c`
  checks it.
- `.editorconfig` has the white space rules; `make lint` checks them.

### ABI rules

`uc_webview.h` is a contract with hosts built against other releases (see the [README](README.md#abi-and-versioning)
and "Compatibility" at the top of the header). Within API version 1:

- new functions only, each with the library release that added it in its comment; hosts resolve them with `dlsym()`;
- new callbacks only at the end of `uc_webview_callbacks`, read only through `uc_webview_new_sized()`; update the
  layout checks in `uc_abi.c` and `tests/test-abi.c`;
- `uc_webview_frame` and `uc_webview_cookie` do not change;
- only `uc_webview_*` is exported (`-fvisibility=hidden`).

A change that cannot follow these rules needs a new API version, which we avoid.

### Commit messages

Commits follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>[optional scope]: <description>

<body: why the change was made>

<trailers>
```

- **Header:** the type (`feat`, `fix`, `docs`, `refactor`, `test`, `build`, `ci`, `chore`), an optional scope
  (`lib`, `launcher`, `ci`, ...) and a short description, at most 72 characters.
- **Body:** why the change was made: the problem, its effect, and what the change achieves. The diff shows how.
  Wrap at 72 characters.
- **One logical change per commit.** A user-visible change carries its changelog entry in the same commit.
- Rebase on `main` instead of merging it into your branch.

### Changelog

Every user-visible fix or feature gets an entry in `CHANGELOG.md`, in the same commit. The file follows
[Keep a Changelog](https://keepachangelog.com/): entries go under `## Unreleased` in `### Added`, `### Changed`,
`### Removed` or `### Fixed`, written for the users of the library and the launcher (host developers and page
authors), not as commit messages. A release that adds an export or a callback names it.

### AI-assisted contributions

The use of AI coding assistants is permitted, **but must be disclosed and used responsibly**:

- **Disclosure required**: if you used AI tools to generate or assist with any part of your contribution, say so in
  the pull request description and name the files or sections.
- **Attribution in commits**: a commit that an AI tool wrote or helped to write names the model in a
  `Co-authored-by:` trailer. Commit messages and pull requests must not contain agent session information, such as
  session IDs or session links.
- **You are responsible** for all code you submit: understand every line, test it, and be able to explain the design
  decisions in the review. Low-quality, unreviewed AI-generated contributions are rejected without review.

### Feedback

- [Create a new issue](https://github.com/unfoldedcircle/ucwebview/issues/new)
- [Visit our community forum](http://unfolded.community/)
- [Chat with us in our Discord channel](http://unfolded.chat/)
- [Send us a message on our website](https://unfoldedcircle.com/contact)
