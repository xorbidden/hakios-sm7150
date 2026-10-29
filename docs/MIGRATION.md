# From hackios-xnu-kernel-davinci to this repository

The Mi 9T's port was made on a fork of the old common repository (xorbidden/hackios-xnu-kernel-davinci, branch
`davinci-metal`, last 6d40948, 2026-10-27). In October 2026 the common repository became `hakios`: a fresh history, one
repository per device, drivers as configuration symbols. Nothing of the old fork can be merged (no common ancestor), so
the port comes over by content, renamed (`hk_*` -> `hakios.*` / `hakios_*`, `kern.hackios` -> `kern.hakios`,
`/dev/hk-*` -> `/dev/hakios-*`), one part at a time. Until a part is here and seen working again on the phone, the old
branch stays the one to use for it.

The rule for where a piece goes:

| The piece is | It goes |
|---|---|
| a file of this phone only | `drivers/<subsystem>/hakios.<name>.c`, a `driver` in `drivers/<subsystem>/conf` |
| a file of the SM7150 family | `soc/drivers/<subsystem>/...`, the same |
| a common driver changed for this SoC beyond a switch | a driver of its own with `replaces <NAME>` (patch 0001) |
| a common file that needs a hook, a table entry or a fix | a patch in `patches/`, small, of one concern, meant upstream |

## Done

| What | Where |
|---|---|
| The repository, device.conf with every part's state, defconfig, SoC layer, soc.args | here |
| The firmware out of Xiaomi's ROM | `pull-firmware.sh` (was tools/davinci-firmware.sh) |
| UEFI: the davinci platform, its three switches on hakios-uefi, the ROM patcher | `uefi/` (the Qualcomm drivers stay local) |
| A SoC's or device's driver in place of a common one | patch 0001 (`replaces` in tools/hakios-config.py) |
| The boot image, xl and the run scripts for the configured device | patch 0002 (mkxnuimg.py, Makefile) |
| The loader's davinci platform (framebuffer 0x9C000000, no flash log, timer PPI 19, UFS connect, charger) | patch 0003 |
| The kernel's timer on the PPI the loader names | patch 0004 (gic_v3_irq.c) |
| A panel driver can register two framebuffers (no tearing) | patch 0005 (MiniIOMobileFramebuffer, MiniIOSurface) |
| Device drivers register their quiesce/stop on the way back | patch 0006 (loader_return.c) |
| The kernel's log mirrored to the loader from kprintf's start | patch 0007 |
| Kernel builds on Ubuntu (mawk, python3 only) | patches 0008, 0009 |
| `make phone` on Linux (macOS shims only on macOS) | patch 0011 |
| RPMh helpers for the SoC's drivers (LDOs, bus votes) on upstream's hakios_rsc_lock | `soc/drivers/core/hakios.rpmh.c` -- seen on the phone 2026-10-29 |
| The panel (DSI command mode) with two framebuffers, via the panel interface and return hooks | `drivers/gpu/hakios.dsi.c` -- seen on the phone 2026-10-29 |
| Touch: GT9886 in place of the common Goodix driver (`touchscreen=1`) | `drivers/input/hakios.gt9886.c`, `replaces GOODIX` -- seen on the phone 2026-10-29 |
| Buttons: the PM6150's power-on block and PM6150L GPIO 2 | patch 0010 (common buttons driver: gen2 PON, `pmic_pon=`, `pmic_volume_up=`) -- seen on the phone 2026-10-29 |
| Storage through the loader's block service | the common UFS driver, unchanged (`CONFIG_HAKIOS_UFS=y`, `ufs=2`) -- seen on the phone 2026-10-29 |
| GPU: the Adreno 618 as the common Adreno driver's backend (`gpu=1`) | `soc/drivers/gpu/hakios.a618.c` + patch 0012 -- seen on the phone 2026-10-29 (21 steps, 610 MHz, Mesa draws) |
| Audio: SM7150 DSP and its modem copy (`adsp=1 adsp_modem=1`), sound over MI2S (`sound=4`), mic on bolero 1.x (`mic=4`), TFA9874 speaker, battery (`battery=1`, common driver) | `soc/drivers/audio/` (`ADSP_SM7150`, `SOUND_SM7150`, `MIC_SM7150`, replacing the common ones), `drivers/audio/hakios.tfa9874.c` (`replaces SPEAKER`), patch 0013 (GUARD split out of SOUND) (built; not yet run) |
| The phone's firmware in the images (TFA container, modem and Wi-Fi images, BT patch, sensors, camera tables) | `userland.sh` (`device_root`, `device_system`) + patch 0014 (userland/build.sh sources it) |
| CPU clocks through the OSM (`cpufreq=1`) | `soc/drivers/power/hakios.cpufreq_osm.c`, `replaces CPUFREQ` -- seen on the phone 2026-10-29 |

The SoC's audio drivers are copies of the common ones with the SM7150's changes woven through (memory map, APCS
doorbell, MI2S, bolero 1.x, FastRPC): they do not take upstream's later fixes to drivers/audio/ by themselves. When
upstream changes one of those files, compare and carry the fix over (`git log upstream/hakios -- drivers/audio/`).

Left out on purpose: the old bring-up diagnostics (XNU_DIAG_* in the loader and UEFI, gic_v3_timer_report), the
install-to-flash mode of the metal release (flashboot.c, darwinfile.c, crypto.c: upstream dropped it), and the kernel's
own UFS driver's first steps (ufs_alone, ufs_hc_area: never usable). They stay in the old branch.

## To do, in this order (each: port, build, run on the phone, `status` line checked)

| # | Part | Old files | Goes to | Notes |
|---|---|---|---|---|
| 9 | Wi-Fi | hk_wlfw10.c, hk_wifi.c, hk_lx.c, hk_apple80211.c, hk_net.c changes, wifi/lx (include10, lx_snoc.c), tools/mk-ath10k.sh | `soc/drivers/wifi/` (`WIFI_ATH10K` `replaces WIFI`), its Linux glue under `soc/wifi/` | |
| 10 | Vibration | hk_vibrator.c | `drivers/haptic/hakios.vibrator.c` `replaces HAPTIC` | the PM6150's LDO |
| 11 | Cameras, flashlight | hk_cam_davinci.c, hk_camera.c, hk_torch.c changes | `drivers/camera/`: `CAM_DAVINCI` `replaces CAMERA`, the PM6150L torch | userland camera_device.c changes too |
| 12 | Bluetooth | hk_bt.c (WCN3990, bt=5) | `soc/drivers/bluetooth/hakios.bt_wcn3990.c` `replaces BT` | then on with the to-do list: Bluetooth in iOS |
| 13 | Userland bits | mkjobs.py, net_test.m, inject*.c (brightness), camera_device.c (the firmware staging is done: userland.sh) | patches, or the device's own image fragment if upstream has one | |
| 14 | Docs | docs/DAVINCI.md | `docs/` here, and the device's page through device.conf | |
