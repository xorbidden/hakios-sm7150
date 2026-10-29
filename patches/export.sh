#!/bin/sh
# devices/davinci/patches/export.sh [BASE]: the commits of the common repository's branch `davinci` over BASE (default: its
# remote branch `hakios`) written back here as NNNN-*.patch, the old ones replaced. Commit them in the device repository.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
cd "$top"
if [ $# -ge 1 ]; then
	base=$1
else
	base=
	for r in upstream origin; do
		if git rev-parse -q --verify "refs/remotes/$r/hakios" >/dev/null; then base=$r/hakios; break; fi
	done
	[ -n "$base" ] || { echo "export.sh: no remote branch hakios; give the base" >&2; exit 1; }
fi
rm -f "$here"/[0-9][0-9][0-9][0-9]-*.patch
git format-patch -q --no-signature --zero-commit --no-numbered -o "$here" "$base"..davinci
ls "$here"/[0-9][0-9][0-9][0-9]-*.patch 2>/dev/null | sed "s|$here/|export.sh: |"
