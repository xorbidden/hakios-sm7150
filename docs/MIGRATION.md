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

Left out on purpose: the old bring-up diagnostics (XNU_DIAG_* in the loader and UEFI, gic_v3_timer_report), the
install-to-flash mode of the metal release (flashboot.c, darwinfile.c, crypto.c: upstream dropped it), and the kernel's
own UFS driver's first steps (ufs_alone, ufs_hc_area: never usable). They stay in the old branch.

## To do, in this order (each: port, build, run on the phone, `status` line checked)

| # | Part | Old files | Goes to | Notes |
|---|---|---|---|---|
| 1 | Hooks on the way back to the loader | loader_return.c (dsi, adsp_modem, vibrator quiesce/stop), GenericARMPE.cpp (dsi init) | a patch: a list drivers register their quiesce/stop in | so no common file names a davinci driver |
| 2 | The log mirrored to the loader from kprintf's start | loader_return.c, xnu/hackios.patch | a patch | how a hung kernel's log reaches the computer on core 7 |
| 3 | CPU clocks (OSM) | hk_cpufreq.c (cpufreq_osm=1) | `soc/drivers/power/hakios.cpufreq_osm.c` | |
| 4 | Panel (DSI) and its framebuffers | hk_dsi.c, hk_display.c, MiniIOMobileFramebuffer.cpp, MiniIOSurface.* | `soc/drivers/gpu/hakios.dsi.c`; the framebuffer's two buffers and MiniIOSurfaceDisplayUse as a patch | the no-tearing work (562d911) is generic |
| 5 | Touch, buttons | hk_gt9886.c; hk_buttons.c (board_pon, board_volume_up) | `drivers/input/hakios.gt9886.c`; buttons: a patch for the PMIC addresses as arguments | |
| 6 | Storage through the loader | (works with patch 0003) | `CONFIG_HAKIOS_UFS=y` | check that the reads still go |
| 7 | GPU | hk_a618.c, hk_adreno.c changes | `soc/drivers/gpu/hakios.a618.c` `replaces ADRENO` | Mesa's freedreno for the A618: userland GL=1 |
| 8 | Audio DSP, sound, microphone, battery | hk_adsp.c (board_smem, board_adsp_mem, ...), hk_adsp_modem.c, hk_sound.c, hk_tfa.c, hk_mic.c, hk_battery.c | `soc/drivers/audio/hakios.adsp_sm7150.c` `replaces ADSP` (+ its modem copy); `drivers/audio/hakios.tfa9874.c` `replaces SPEAKER`; mic, sound: replace or patch, after a diff | the biggest part; the board_* addresses become soc_* |
| 9 | Wi-Fi | hk_wlfw10.c, hk_wifi.c, hk_lx.c, hk_apple80211.c, hk_net.c changes, wifi/lx (include10, lx_snoc.c), tools/mk-ath10k.sh | `soc/drivers/wifi/` (`WIFI_ATH10K` `replaces WIFI`), its Linux glue under `soc/wifi/` | |
| 10 | Vibration | hk_vibrator.c | `drivers/haptic/hakios.vibrator.c` `replaces HAPTIC` | the PM6150's LDO |
| 11 | Cameras, flashlight | hk_cam_davinci.c, hk_camera.c, hk_torch.c changes | `drivers/camera/`: `CAM_DAVINCI` `replaces CAMERA`, the PM6150L torch | userland camera_device.c changes too |
| 12 | Bluetooth | hk_bt.c (WCN3990, bt=5) | `soc/drivers/bluetooth/hakios.bt_wcn3990.c` `replaces BT` | then on with the to-do list: Bluetooth in iOS |
| 13 | Userland bits | userland/build.sh (fw-bt, ath10k firmware, the network), mkjobs.py, net_test.m, inject*.c, camera_device.c | patches, or the device's own image fragment if upstream has one | |
| 14 | Docs | docs/DAVINCI.md | `docs/` here, and the device's page through device.conf | |
