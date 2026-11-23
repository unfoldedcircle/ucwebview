#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# The whitespace rules of .editorconfig: LF line ends, no trailing white space, a final newline, and no tab
# characters outside Makefiles (device-host-shim.js is compiled into the web process extension as bytes and must not
# have any either). webkit-patches/ is skipped: a patch keeps the white space of its context lines.
#
# usage: scripts/check-whitespace.sh   (in a git checkout; checks the tracked and the new, not ignored files)
set -eu

cd "$(dirname "$0")/.."
status=0
n=0
tab=$(printf '\t')
cr=$(printf '\r')
for f in $(git ls-files --cached --others --exclude-standard | grep -v '^webkit-patches/'); do
    [ -f "$f" ] || continue
    # binary files have no rules here
    if ! grep -Iq . "$f" 2>/dev/null && [ -s "$f" ]; then
        continue
    fi
    n=$((n + 1))
    if grep -n "[[:blank:]]$" "$f" >/dev/null; then
        grep -n "[[:blank:]]$" "$f" | head -n 3 | sed "s|^|$f:|; s|\$| <- trailing white space|"
        status=1
    fi
    if grep -q "$cr" "$f"; then
        echo "$f: CR line ends"
        status=1
    fi
    if [ -s "$f" ] && [ -n "$(tail -c 1 "$f")" ]; then
        echo "$f: no newline at the end of the file"
        status=1
    fi
    case "$f" in
    Makefile | */Makefile) ;;
    *)
        if grep -n "$tab" "$f" >/dev/null; then
            grep -n "$tab" "$f" | head -n 3 | sed "s|^|$f:|; s|\$| <- tab|"
            status=1
        fi
        ;;
    esac
done
[ $status -eq 0 ] && echo "check-whitespace: $n files ok"
exit $status
