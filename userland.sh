# devices/davinci/userland.sh: the Mi 9T's own files for the images, sourced by userland/build.sh (patch 0014) from
# userland/, which it runs in. All of it is the phone's (Xiaomi's ROM: devices/davinci/pull-firmware.sh puts it into
# ../fw-*) and none of it is in a repository.

# The small image's fw/, which travels with the kernel: what is small and read before the system image is mounted.
device_root() {
	# ath10k's description of the WCN3990's firmware (firmware-5.bin, 60 bytes: its features), from Linux's
	# linux-firmware ath10k/WCN3990/hw1.0
	if [ -f ../fw-modem/firmware-5.bin ]; then
		mkdir -p root/fw
		cp ../fw-modem/firmware-5.bin root/fw/
	fi
	# the WCN3990's Bluetooth patch and NVM (crbtfw21.tlv, crnv21.bin; 230 KB), from the ROM's BTFM.bin (bt=5)
	if [ -f ../fw-bt/crbtfw21.tlv ] && [ -f ../fw-bt/crnv21.bin ]; then
		mkdir -p root/fw
		cp ../fw-bt/crbtfw21.tlv ../fw-bt/crnv21.bin root/fw/
	fi
	# the cameras' sensor tables, from com.qti.sensormodule.davinci_<maker>_<sensor>.bin (the vendor image's
	# lib64/camera): camera-<sensor>.tab
	for module in ../fw-camera/com.qti.sensormodule.davinci_*.bin; do
		[ -f "$module" ] || continue
		sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.davinci_[a-z]*_\([a-z0-9]*\)$/\1/')
		mkdir -p root/fw
		../tools/camsensor.py "$module" "root/fw/camera-$sensor.tab" >/dev/null
	done
}

# The system image's /fw: what is large, read once the root is mounted.
device_system() {
	# the TFA9874's container, from the vendor image's firmware/mono.cnt (drivers/audio/hakios.tfa9874.c)
	if [ -f ../fw-audio/mono.cnt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/mono.cnt sysroot/fw/
	fi
	# the modem's firmware (MPSS, where the Wi-Fi's firmware runs: adsp_modem=1), from NON-HLOS.bin, and the
	# Wi-Fi's own (wlanmdsp.mbn, bdwlan.*)
	if [ -f ../fw-modem/modem.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-modem/modem.mdt ../fw-modem/modem.b[0-9][0-9] sysroot/fw/
		cp ../fw-modem/wlanmdsp.mbn ../fw-modem/bdwlan.* sysroot/fw/ 2>/dev/null || true
	fi
	# the files the audio DSP's sensors process reads over FastRPC (soc/drivers/audio/hakios.adsp_sm7150.c,
	# apps_std), from the vendor and dsp images: hexagonfs and its index
	if [ -f ../fw-sensors/hexagonfs.idx ]; then
		mkdir -p sysroot/fw
		rm -rf sysroot/fw/hexagonfs
		cp -R ../fw-sensors/hexagonfs ../fw-sensors/hexagonfs.idx sysroot/fw/
	fi
}
