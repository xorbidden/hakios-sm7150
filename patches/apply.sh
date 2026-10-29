#!/bin/sh
# devices/davinci/patches/apply.sh [BASE]: the common repository as BASE (default: its remote branch `hakios`) and, on top,
# the changes to it this port still needs (the NNNN-*.patch here, in order), as commits of a local branch `davinci`.
#
# Run it after `git fetch` of the common repository to take its updates: the branch is made again from the new BASE.
# A patch that no longer applies stops `git am` on it; resolve it, `git am --continue`, then patches/export.sh.
# The patches are the only place this port touches a common file; each one is meant to go upstream as a pull request.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
cd "$top"
if ! git diff --quiet || ! git diff --cached --quiet; then
	echo "apply.sh: $top has changes not committed; commit or drop them first" >&2
	exit 1
fi
if [ $# -ge 1 ]; then
	base=$1
else
	base=
	for r in upstream origin; do
		if git rev-parse -q --verify "refs/remotes/$r/hakios" >/dev/null; then base=$r/hakios; break; fi
	done
	[ -n "$base" ] || { echo "apply.sh: no remote branch hakios; give the base" >&2; exit 1; }
fi
git checkout -q -B davinci "$base"
set -- "$here"/[0-9][0-9][0-9][0-9]-*.patch
if [ -e "$1" ]; then
	git am -q --3way "$@"
fi
echo "apply.sh: branch davinci = $base ($(git rev-parse --short "$base")) + $(git rev-list --count "$base"..davinci) patches"
