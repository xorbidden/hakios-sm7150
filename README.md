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
| Bringing the old port over | `docs/MIGRATION.md`: what of hackios-xnu-kernel-davinci is here already, what is still to come |

## Setting up

```sh
git clone -b hakios https://github.com/oosexgang/hakios-xnu-kernel.git hakios && cd hakios
git clone git@github.com:xorbidden/hakios-sm7150.git devices/davinci
tools/hakios-device.py sync redwood hakios-uefi   # the reference device and the UEFI, as the manifest names them
devices/davinci/patches/apply.sh                  # local branch `davinci` = upstream + this port's patches
make davinci_defconfig
```

## Taking the common repository's updates

```sh
git fetch origin                                  # (or `upstream`, whichever remote is oosexgang's)
devices/davinci/patches/apply.sh                  # the branch made again on the new upstream, the patches on top
```

A patch that no longer applies stops on it (`git am`): resolve, `git am --continue`, then `devices/davinci/patches/export.sh`
and commit the refreshed patches here. Nothing of the port is in the common checkout's own history, so a new upstream can
never take it away.

## Firmware

```sh
devices/davinci/pull-firmware.sh ~/Downloads/davinci_eea_global_images_V12.1.1.0.RFJEUXM_*.tgz   # or ROM=... make firmware
```

## UEFI

`uefi/files/Binaries/davinci/` (Qualcomm's UEFI drivers, out of the ROM's `xbl.elf` with UEFIReader, patched by
`uefi/tools/patch-davinci-uefi.py`) and `prebuilt/SILICIUM_UEFI.fd` stay on your machine: git ignores them. Then
`devices/davinci/uefi/apply.sh` and Mu-Silicium's `build_uefi.py -d davinci`.
