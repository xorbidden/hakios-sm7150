#!/bin/bash
# devices/davinci/soc/wifi/mk-ath10k.sh: build the driver of the Mi 9T's Wi-Fi radio for the kernel: ath10k, Qualcomm's driver for the
# WCN3990 (the radio is in the SoC, its firmware on the modem; the bus is ath10k's snoc.c), as it is
# in Linux, unmodified, on the same layer as redwood's ath11k (tools/mk-ath11k.sh, whose FreeBSD
# LinuxKPI tree and generated headers this uses as they are):
#
#   Linux's ath10k        drivers/net/wireless/ath/ath10k of the pinned release of Linux, with the
#                         files of ath/ it uses (main.c, regd.c, key.c and their headers); all ISC
#                         (checked below, not assumed);
#   FreeBSD's LinuxKPI    xnu-deps/build/ath11k/freebsd, patched with wifi/lx/patches (mk-ath11k.sh);
#   wifi/lx               ours: the runtime (lx_base.c), the station (lx_80211.c) and WPA2 (lx_wpa.c)
#                         shared with ath11k; lx_bus.c is ath11k's bus and is not used here.
#                         include10/ (here) holds the headers ath10k needs beyond ath11k's, found first.
#
# The driver's files and ours are compiled, and tied into one object that shows nothing but its
# entry points (lx_start(), lx_stop(), lx_transmit(): the ones ath11k's object shows, so the common host,
# drivers/wifi/hakios.lx.c, drives it as it drives ath11k; devices/davinci/lx.object names it for xnu-build.sh). lx_snoc.c (here) is the platform under ath10k's own snoc.c.
# What the object still wants of the driver's files that are not built becomes a function that
# says its name in the log and answers 0 (out/stubs.txt); anything else missing ends the build.
#
#   VERSION   of Linux: mk-ath11k.sh's, so that one LinuxKPI serves both drivers.
#
# What comes out, in xnu-deps/build/ath10k:
#   src/ath10k/ src/*.c    the driver's files and ath/'s, untouched
#   obj/                   one object per file; FILE.o.err holds what the compiler said
#   out/ath10k.o           the object for the kernel (xnu-build.sh links it when it is there)
#   out/stubs.txt          what is not there yet
#
# usage: devices/davinci/soc/wifi/mk-ath10k.sh
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)       # devices/davinci/soc/wifi: ath10k's platform and headers
ROOT=$(cd "$HERE/../../../.." && pwd)                        # the common repository
SELF=$(readlink -f "$0")            # (by its whole path: the compiling below is done from the build directory)
A11=$ROOT/xnu-deps/build/ath11k
B=$ROOT/xnu-deps/build/ath10k
ATH=drivers/net/wireless/ath
W=$ROOT/wifi/lx
# (mk-ath11k.sh's release, and the FreeBSD tree and generated headers it makes)
VERSION=$(sed -n 's/^VERSION=//p' "$ROOT/tools/mk-ath11k.sh")
if [ ! -f "$A11/freebsd/.release" ] || [ ! -d "$A11/gen" ] || [ ! -f "$A11/linux-$VERSION.tar.xz" ]; then
	"$ROOT/tools/mk-ath11k.sh" >/dev/null
fi
mkdir -p "$B"
cd "$B"

UNPACK="$VERSION, with reg.h"             # (what the tree below holds: a change here unpacks it again)
if [ "$(cat src/.release 2>/dev/null)" != "$UNPACK" ]; then
	rm -rf src unpacked
	mkdir unpacked
	tar xJf "$A11/linux-$VERSION.tar.xz" -C unpacked --strip-components=1 linux-$VERSION/$ATH/ath10k \
		$(for f in main.c regd.c key.c ath.h reg.h regd.h regd_common.h trace.h spectral_common.h \
			dfs_pattern_detector.h dfs_pri_detector.h; do echo linux-$VERSION/$ATH/$f; done)
	# every file must say itself that it may be used so
	for f in unpacked/$ATH/ath10k/*.[ch] unpacked/$ATH/*.[ch]; do
		case "$(head -8 "$f")" in
		*"SPDX-License-Identifier: ISC"*) ;;
		*"Permission to use, copy, modify, and/or distribute this software"*) ;;      # (ISC, spelt out)
		*) echo "mk-ath10k.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mkdir src
	mv unpacked/$ATH/ath10k src/ath10k
	mv unpacked/$ATH/*.[ch] src/
	rm -rf unpacked
	echo "$UNPACK" > src/.release
fi
S=$A11/freebsd/sys

# --- compile: what a kernel with CONFIG_ATH10K and CONFIG_ATH10K_SNOC builds (ath10k's Makefile:
# ath10k_core.o, ath10k_snoc.o; no debugfs, tracing, testmode, thermal, LEDs, WoWLAN or core dumps),
# and ath/'s ath.o; then ours
DRIVER="ath10k/mac ath10k/debug ath10k/core ath10k/htc ath10k/htt ath10k/htt_rx ath10k/htt_tx ath10k/txrx
	ath10k/wmi ath10k/wmi-tlv ath10k/bmi ath10k/hw ath10k/p2p ath10k/swap ath10k/ce
	ath10k/snoc ath10k/qmi ath10k/qmi_wlfw_v01 main regd key"
CC="clang -target arm64-apple-ios16.5"
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$HERE/include10 -I$W/include -I$A11/gen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Isrc/ath10k -Isrc -include $HERE/include10/lx_prelude10.h -DCONFIG_ATH10K_SNOC -DCONFIG_ATH10K_DEBUG
	-DKBUILD_MODNAME=\"ath10k\""
source "$ROOT/xnu-env.sh"
for h in lx qmi; do
	[ -f "$XNU_SRC/bsd/sys/hakios.$h.h" ] || { echo "mk-ath10k.sh: the kernel tree has no bsd/sys/hakios.$h.h" >&2; exit 1; }
done
DFLAGS="$DFLAGS -DLX_HAKIOS_LX_H=\"$XNU_SRC/bsd/sys/hakios.lx.h\" -DLX_HAKIOS_QMI_H=\"$XNU_SRC/bsd/sys/hakios.qmi.h\""
OURS="lx_base lx_80211 lx_wpa"                  # the common runtime (wifi/lx/src), and ath10k's platform from here
mkdir -p obj
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$SELF" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
for f in $DRIVER; do compile src/$f.c obj/$(echo $f | tr / _).o $DFLAGS & done
for f in $OURS; do compile "$W/src/$f.c" obj/$f.o $DFLAGS -I$W/src & done
compile "$HERE/lx_snoc.c" obj/lx_snoc.o $DFLAGS -I$W/src &
wait
bad=0
for f in obj/*.o.err; do
	if [ -s "$f" ] && [ ! -f "${f%.err}" ]; then
		echo "--- $(basename "$f" .o.err): $(grep -c 'error:' "$f") error(s)"
		grep 'error:' "$f" | sed -e 's|^.*/src/||' -e "s|^$W/||" | sort -u | head -8
		bad=$((bad + 1))
	fi
done
total=$(( $(echo $DRIVER | wc -w) + $(echo $OURS | wc -w) + 1 ))
[ $bad -eq 0 ] || { echo "mk-ath10k.sh: $((total - bad)) of $total files compile"; exit 1; }
OBJECTS=$(for f in $DRIVER; do echo obj/$(echo $f | tr / _).o; done; for f in $OURS; do echo obj/$f.o; done; echo obj/lx_snoc.o)

# --- one object (as mk-ath11k.sh's): the kernel's calls, the C library's that XNU has, stubs for
# the driver's files not built; anything else missing ends the build
LD=$ROOT/xnu-deps/host/bin/ld64
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
mkdir -p out
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
llvm-nm -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|qmi_*|hakios_qmi_*|section\$*) continue ;;
	ath10k_*|__ath10k_*) ;;
	*) echo "mk-ath10k.sh: nothing defines $s" >&2; missing=1; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
[ -z "$missing" ] || exit 1
$CC -c out/stubs.s -o out/stubs.o
printf '_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt    # (ath11k's names: the common host drives it)
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/ath10k.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
echo "mk-ath10k.sh: out/ath10k.o: $(llvm-size out/ath10k.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data from" \
	"$(echo $DRIVER | wc -w) files; $(wc -l < out/stubs.txt) functions are stubs (out/stubs.txt); asks the kernel for" \
	"$(llvm-nm -u --format=just-symbols out/ath10k.o | grep -vc 'section\$') names"
