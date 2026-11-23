#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unfolded Circle ApS
#
# Every source file of the repository (C, headers, scripts, Makefiles, JavaScript, Python, templates, workflows)
# carries the MIT SPDX identifier and the copyright line of Unfolded Circle ApS in its first lines.
# Not checked: LICENSE, Markdown, .gitignore/.editorconfig files and webkit-patches/ (patches to WebKit carry the
# license of the WebKit files they change, see webkit-patches/README.md).
#
# usage: scripts/check-spdx.sh   (in a git checkout; checks the tracked and the new, not ignored files)
set -eu

cd "$(dirname "$0")/.."
status=0
files=$(git ls-files --cached --others --exclude-standard | grep -E '(\.(c|h|py|js|mjs|sh|yml|yaml|in)|(^|/)Makefile)$' \
    | grep -v '^webkit-patches/' || true)
for f in $files; do
    [ -f "$f" ] || continue
    if ! head -n 6 "$f" | grep -q 'SPDX-License-Identifier: MIT$'; then
        echo "$f: no 'SPDX-License-Identifier: MIT' in the first lines"
        status=1
    fi
    if ! head -n 6 "$f" | grep -qE 'Copyright \(c\) [0-9]{4}(-[0-9]{4})? Unfolded Circle ApS'; then
        echo "$f: no 'Copyright (c) <year> Unfolded Circle ApS' in the first lines"
        status=1
    fi
done
[ $status -eq 0 ] && echo "check-spdx: $(echo "$files" | wc -w) files ok"
exit $status
