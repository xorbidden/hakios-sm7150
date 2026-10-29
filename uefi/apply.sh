#!/bin/bash
# The davinci platform on HakiOS's UEFI: uefi/apply.sh of the common repository (Mu-Silicium with HakiOS's changes and
# banner) with this phone's platform from files/ here, then davinci.patch: the three switches its davinci.dsc turns on
# (XNU_DISABLE_APSS_WDT, XNU_NO_FLASH_LOG, XNU_DIAG_SHUTDOWN). See README.md here.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
MU=${MU:-$ROOT/Mu-Silicium}
ls "$HERE"/files/Binaries/davinci/QcomPkg >/dev/null 2>&1 ||
	{ echo "devices/davinci/uefi/apply.sh: files/Binaries/davinci has no drivers yet (README.md: how to make them)" >&2; exit 1; }
"$ROOT/uefi/apply.sh" davinci
git -C "$MU" apply --whitespace=nowarn "$HERE/davinci.patch"
echo "devices/davinci/uefi/apply.sh: davinci.patch applied; build with: cd Mu-Silicium && python3 build_uefi.py -d davinci -r RELEASE"
