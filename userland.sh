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
	# BlueTool's scripts (BT_JOBS=1)
	device_bluetool
}

# Darwin's Bluetooth daemon and its helper, with BT_JOBS=1 (the kernel's side of iOS's Bluetooth is not done yet: without
# it bluetoothd finds no controller; the image is the same otherwise). On the iPhone X the controller is on uart1, H4 at
# 3 Mbaud (/dev/uart.bluetooth); BlueTool sets it up for bluetoothd.
[ "${BT_JOBS-0}" != 0 ] && DEVICE_JOBS="${DEVICE_JOBS-} com.apple.bluetoothd com.apple.BlueTool"
# ... and BlueTool told it is a vendor's install, for it to read our scripts below (src/inject_bt.c)
[ "${BT_JOBS-0}" != 0 ] && DEVICE_INJECT="${DEVICE_INJECT-}/redwood/lib/libhakios.bt.dylib BlueTool"

# The device's libraries for the small image's lib/ (build.sh calls this before it writes the injection list)
device_libraries() {
	if [ "${BT_JOBS-0}" != 0 ]; then
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.bt.dylib -framework CoreFoundation -lMobileGestalt \
			-o obj/libhakios.bt.dylib ../devices/davinci/userland/src/inject_bt.c
		cp obj/libhakios.bt.dylib root/lib/
	fi
}

# BlueTool's scripts for this phone, with BT_JOBS=1: its own for the iPhone X download a Broadcom chip's firmware;
# the WCN3990 is a Qualcomm one, patched and reset by the kernel (bt=6), so they only open the port (-D: the kernel
# control com.apple.uart.bluetooth, hakios.bt_uart.c) and reset it.
# BlueTool looks for /etc/bluetool/<hardware model>.<sequence>.script, then <platform hash>.<sequence>.script.
device_bluetool() {
	[ "${BT_JOBS-0}" != 0 ] || return 0
	mkdir -p sysroot/private/etc/bluetool
	for name in D22AP fd079b298144a7deb4d7c87fef3eb474d8f0e7eb; do
		for sequence in boot init; do
			printf 'device -D -s 115200\nhci reset\n' > sysroot/private/etc/bluetool/$name.$sequence.script
		done
	done
}
