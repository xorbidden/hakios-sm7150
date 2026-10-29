# hakios-sm7150: the Xiaomi Mi 9T / Redmi K20 (davinci) and its Snapdragon 730 (SM7150)

One HakiOS device repository, in the layout of [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325): the device
(`device.conf`, `configs/`, its drivers, its UEFI platform, its firmware script) and its SoC family's layer (`soc/`: SM7150,
"sdmmagpie"). It lives in `devices/davinci/` of a checkout of the common repository
([oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), branch `hakios`), which it never changes
in place: everything the port needs that is not a file of its own is a patch in `patches/`.

| What | Where |
|---|---|
| What works, part by part | `device.conf` (`status` lines: only what was seen on the phone is `works`) |
| The configuration | `configs/davinci_defconfig` = common + `soc/configs/sm7150.config` + `configs/davinci.config` |
| Drivers of the SoC (Adreno 618, DSI, the audio DSP's memory map, ...) | `soc/drivers/<subsystem>/` |
| Drivers of this phone only (GT9886 touch, TFA9874 amplifier, the cameras, ...) | `drivers/<subsystem>/` |
| Changes to common files, until upstream has them | `patches/NNNN-*.patch`, applied by `patches/apply.sh` |
| The SoC's addresses for the common drivers | `soc.args` (`docs/soc-args.md`: where each comes from) |
| UEFI | `uefi/`: the davinci platform for HakiOS's UEFI (`hakios-uefi`) and `davinci.patch` |
| Prebuilt boot pieces | `prebuilt/`: `BootShim.bin` (for this UEFI's 0x9FC00000/0x300000), `device-tree.dtb`, `ramdisk`, `mkbootimg.py` |
| The phone's firmware | **not in the repository**: `pull-firmware.sh` takes it out of Xiaomi's fastboot ROM into `fw-*/` |
| How to build and run it | **`docs/GUIDE.md`** |
| Bringing the old port over | `docs/MIGRATION.md`: what of hackios-xnu-kernel-davinci is here already, what is still to come |

## Setting up, building, running

`docs/GUIDE.md`: what the phone needs, the firmware out of Xiaomi's ROM, the UEFI, the build (`make davinci_defconfig`,
`soc/wifi/mk-ath10k.sh`, `make kernel`), a session on the phone (`make phone`), and taking the common repository's
updates (`patches/apply.sh`).
