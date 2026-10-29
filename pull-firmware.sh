#!/bin/bash
# The Mi 9T's (davinci) own firmware, which the repository never carries (Qualcomm's and Xiaomi's), out of
# Xiaomi's fastboot ROM into the fw-* directories the build takes it from (userland/build.sh):
#
#   fw-audio    the audio DSP's firmware (adsp.*: NON-HLOS.bin) and the amplifier's (mono.cnt, tfa98xx.cnt: vendor)
#   fw-gpu      the Adreno 618's zap shader (a615_zap.*), GMU and SQE microcode (vendor /firmware)
#   fw-modem    the modem DSP (modem.*) and the Wi-Fi's (wlanmdsp.mbn, bdwlan.*: NON-HLOS.bin), and ath10k's
#               firmware-5.bin (from Linux's linux-firmware, ath10k/WCN3990/hw1.0)
#   fw-bt       the WCN3990's Bluetooth patch and NVM (crbtfw21.tlv, crnv21.bin: BTFM.bin)
#   fw-camera   the cameras' sensor modules (com.qti.sensormodule.davinci_*.bin: vendor /lib64/camera)
#   fw-sensors  hexagonfs: what the audio DSP's sensors process reads from Android's file system
#               (vendor /etc/sensors and /etc/acdbdata, dspso.bin's /adsp, six values of the SoC)
#
# usage: devices/davinci/pull-firmware.sh davinci_eea_global_images_V12.1.1.0.RFJEUXM_20210903.0000.00_11.0_eea_*.tgz
#
# The ROM this port was made with is the EEA one, V12.1.1.0.RFJEUXM (Android 11), from Xiaomi's download pages;
# another MIUI 12 ROM of the Mi 9T should do too. Needs tar, 7z (p7zip), debugfs (e2fsprogs), python3 and curl.
# (dspso.bin and vendor.img are ext4, NON-HLOS.bin and BTFM.bin FAT.)
# Unlike other devices' over adb, this one's comes out of Xiaomi's ROM, byte-identical to what the phone runs
# (`ROM=... make firmware` works too).
# Nothing is written outside the fw-* directories of this tree and a temporary directory.
set -e
cd "$(dirname "$(readlink -f "$0")")/../.."      # the top of the common repository: fw-* go there
ROM=$(readlink -f "${1:-${ROM:?usage: devices/davinci/pull-firmware.sh davinci_..._images_....tgz}}")
for t in tar 7z debugfs python3 curl; do
	command -v $t >/dev/null || { echo "pull-firmware.sh: needs $t" >&2; exit 1; }
done
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

echo "pull-firmware.sh: taking NON-HLOS.bin, BTFM.bin, dspso.bin and vendor.img out of $(basename "$ROM")"
tar -xzf "$ROM" -C "$T" --wildcards --no-anchored --strip-components=2 \
	'*/images/NON-HLOS.bin' '*/images/BTFM.bin' '*/images/dspso.bin' '*/images/vendor.img'
for f in NON-HLOS.bin BTFM.bin dspso.bin vendor.img; do
	[ -f "$T/$f" ] || { echo "pull-firmware.sh: no $f in the ROM: not a Mi 9T fastboot ROM?" >&2; exit 1; }
done
# (both FATs end before 7z expects them to: it says so and fails, having read everything taken here -- the files
# themselves are checked below)
7z x -y -o"$T/nonhlos" "$T/NON-HLOS.bin" > /dev/null 2>&1 || true
7z x -y -o"$T/btfm" "$T/BTFM.bin" > /dev/null 2>&1 || true
# vendor.img is an Android sparse image: made whole for debugfs
python3 - "$T/vendor.img" "$T/vendor.raw" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'rb'); o = open(sys.argv[2], 'wb')
magic, _, _, hs, cs, bs, _, chunks, _ = struct.unpack('<IHHHHIIII', f.read(28))
assert magic == 0xed26ff3a, 'not a sparse image'
f.read(hs - 28)
for _ in range(chunks):
    kind, _, n, total = struct.unpack('<HHII', f.read(12)); f.read(cs - 12)
    if kind == 0xcac1: o.write(f.read(total - cs))
    elif kind == 0xcac2: o.write(f.read(4) * (n * bs // 4))
    elif kind == 0xcac3: o.seek(n * bs, 1)
    else: f.read(total - cs)
o.truncate()
EOF
rm -f "$T/vendor.img"
mkdir -p "$T/vendor"
(cd "$T/vendor" && debugfs -R "rdump /firmware /lib64/camera /etc/sensors /etc/acdbdata ." "$T/vendor.raw" > /dev/null 2>&1)
(cd "$T" && mkdir -p dsp && debugfs -R "rdump /adsp dsp" dspso.bin > /dev/null 2>&1)
N=$T/nonhlos/image V=$T/vendor
for f in $N/adsp.mdt $N/modem.mdt $N/wlanmdsp.mbn $T/btfm/image/crbtfw21.tlv $V/firmware/a630_sqe.fw $V/camera $V/sensors $T/dsp/adsp; do
	[ -e "$f" ] || { echo "pull-firmware.sh: ${f#$T/} is not in the ROM's images" >&2; exit 1; }
done

rm -rf fw-audio fw-gpu fw-modem fw-bt fw-camera fw-sensors
mkdir -p fw-audio fw-gpu fw-modem fw-bt fw-camera
cp $N/adsp.* fw-audio/
cp $V/firmware/mono.cnt $V/firmware/tfa98xx.cnt fw-audio/
cp $V/firmware/a615_zap.mdt $V/firmware/a615_zap.b0* $V/firmware/a618_gmu.bin $V/firmware/a630_gmu.bin $V/firmware/a630_sqe.fw fw-gpu/
cp $N/modem.* $N/wlanmdsp.mbn $N/bdwlan.* fw-modem/
curl -sfL -o fw-modem/firmware-5.bin \
	"https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/ath10k/WCN3990/hw1.0/firmware-5.bin"
cp $T/btfm/image/crbtfw21.tlv $T/btfm/image/crnv21.bin fw-bt/
# The camera modules come from two makers; a phone has one of each sensor. CAMERAS= the phone's (Android tells:
# adb shell getprop | grep 'persist.vendor.camera.*info'); the default is the set this port was made on.
for m in ${CAMERAS:-ofilm_imx582 ofilm_ov8856 ofilm_s5k3l6 sunny_s5k3t2}; do
	cp "$V/camera/com.qti.sensormodule.davinci_$m.bin" fw-camera/
done

# hexagonfs, as the sensors process sees it (hk_adsp.c's apps_std): the vendor's sensor and audio configuration,
# the DSP's shared objects, and what Android's /sys/devices/soc0 says on this phone; then its index
H=fw-sensors/hexagonfs
mkdir -p $H/vendor/etc $H/usr/lib/qcom $H/sys/devices/soc0 $H/mnt/vendor/persist/sensors/registry/registry
cp -R $V/sensors $V/acdbdata $H/vendor/etc/
cp -R $T/dsp/adsp $H/usr/lib/qcom/
printf 'DAVINCI\n' > $H/sys/devices/soc0/hw_platform
printf 'Unknown\n' > $H/sys/devices/soc0/platform_subtype
printf '0\n' > $H/sys/devices/soc0/platform_subtype_id
printf '1638400\n' > $H/sys/devices/soc0/platform_version
printf '365\n' > $H/sys/devices/soc0/soc_id
printf '1.0\n' > $H/sys/devices/soc0/revision
chmod -R u+rwX,go+rX fw-sensors
(cd $H && find . -mindepth 1 | sort | while read -r p; do
	if [ -d "$p" ]; then echo "d 0 ${p#.}"; else echo "f $(stat -c %s "$p") ${p#.}"; fi
done) > fw-sensors/hexagonfs.idx

for d in fw-audio fw-gpu fw-modem fw-bt fw-camera fw-sensors/hexagonfs; do
	printf 'pull-firmware.sh: %-22s %4d files\n' "$d" "$(find $d -type f | wc -l)"
done
echo "pull-firmware.sh: done. For Wi-Fi, fw-wifi/network: the network's name on its first line, its password on the second."
