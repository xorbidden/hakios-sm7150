# HakiOS on the Xiaomi Mi 9T (davinci)

iOS 16.5 on a Xiaomi Mi 9T (`davinci`, Snapdragon 730 / SM7150), started from a computer over USB with
`fastboot boot`. **Nothing is written to the phone**: the UEFI, the loader, the kernel and the whole system live in
RAM, and a reboot brings back Android. The port is this repository on top of the common HakiOS repository
([oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), branch `hakios`), which it does not
change in place: its changes to common files are the patches in `patches/`.

## What works

| Works | Not yet |
|---|---|
| Display (the kernel drives the panel; Adreno 618 with `gpu=1`), touch, power and volume keys | Bluetooth in iOS (the controller itself comes up: `bt=5`) |
| Sound from the speaker, the microphone (Voice Memos), the battery and its charging bolt | Cellular, GPS |
| Vibration (iOS's haptics) | The focus motor, the tele and ultra-wide cameras |
| Wi-Fi (WPA2-Personal), in a session started from Android | The App Store (see "Known limits") |
| Main camera (IMX582) and the pop-up front camera (S5K3T2) in the Camera app, the flashlight | Fingerprint reader |
| All eight cores at their top speed | Installing to the phone's storage (the system runs from RAM) |

`device.conf` has the state of every part with the boot argument that turns it on; `docs/MIGRATION.md` says which
parts have been seen again since the move to this layout.

## What you need

- A **Mi 9T (davinci)** -- not the 9T Pro (raphael) -- with an **unlocked bootloader**, running **MIUI 12 (Android 11)**,
  with USB debugging on and this computer allowed (`adb devices` lists it). The port was made on the EEA ROM
  `V12.1.1.0.RFJEUXM`.
- A **Linux** computer (made on Ubuntu, x86-64) with ~25 GB free and what the common repository's `make doctor` asks
  for (git, python3, 7-Zip 23 or newer, e2fsprogs, curl, clang, lld, adb, fastboot, theos with the iPhoneOS16.5 SDK).
- **Xiaomi's fastboot ROM** of the phone, for its firmware (Qualcomm's and Xiaomi's files are never in a repository):
  `davinci_eea_global_images_V12.1.1.0.RFJEUXM_20210903.0000.00_11.0_eea_*.tgz` (about 2.6 GB). Nothing is flashed
  from it; only files are taken out of it.
- **The iOS 16.5 IPSW** (`iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`), which you download yourself.

## Setting up

```sh
git clone -b hakios https://github.com/oosexgang/hakios-xnu-kernel.git hakios && cd hakios
git clone https://github.com/xorbidden/hakios-sm7150.git devices/davinci
tools/hakios-device.py sync redwood hakios-uefi   # the reference device and HakiOS's UEFI, as the manifest names them
devices/davinci/patches/apply.sh                  # local branch `davinci`: upstream + this port's patches
umask 022                                         # the system image keeps the files' modes as made

sysimage/unpack.sh ~/Downloads/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
devices/davinci/pull-firmware.sh ~/Downloads/davinci_eea_global_images_V12.1.1.0.RFJEUXM_*.tgz   # -> fw-*

# Wi-Fi (optional): the network to join -- its name on the first line, its password on the second.
# git ignores this file; it goes into your image only (SHARE=1 on the image build leaves it out).
mkdir -p fw-wifi && printf '%s\n%s\n' 'MyNetwork' 'MyPassword' > fw-wifi/network && chmod 600 fw-wifi/network
```

Mi 9T units have camera modules from two makers; the default is the set this port was made on (OFilm and Sunny).
Check yours with `adb shell getprop | grep 'persist.vendor.camera.*info'`; if it differs, say so:
`CAMERAS="luxvisions_imx582 luxvisions_ov8856 luxvisions_s5k3l6 sunny_s5k3t2" devices/davinci/pull-firmware.sh ...`.

### The UEFI

The UEFI carries Qualcomm's own UEFI drivers, which are never in a repository. Make them once from the ROM's
`xbl.elf` with [UEFIReader](https://github.com/WOA-Project/UEFIReader), apply the patches that
`devices/davinci/uefi/tools/patch-davinci-uefi.py` makes (`uefi/files/Binaries/davinci/README.md` lists them), and put
the result in `devices/davinci/uefi/files/Binaries/davinci/`. Then:

```sh
devices/davinci/uefi/apply.sh                     # Mu-Silicium + HakiOS's UEFI changes + the davinci platform
cd Mu-Silicium && python3 build_uefi.py -d davinci -r RELEASE && cd ..
```

`mkxnuimg.py` takes the image from `Mu-Silicium/Build/davinciPkg/`, or from `devices/davinci/prebuilt/SILICIUM_UEFI.fd`
(git ignores it) when there is no build.

## Building

```sh
make davinci_defconfig                            # common + SM7150 (soc/) + the phone's own (configs/davinci.config)
devices/davinci/soc/wifi/mk-ath10k.sh             # the Wi-Fi driver's object (ath10k), linked by the kernel build
make kernel
make loader
REST=1 GL=1 userland/build.sh                     # the system image and the small image, Mesa for the GPU
```

## Running it

With the phone in Android, connected over USB:

```sh
make phone SECS=900                               # a 15-minute session
```

It reboots the phone into fastboot, starts the boot image with `fastboot boot` (the UEFI, which starts the loader; the
loader then speaks USB itself), uploads the system image into the phone's RAM (2.4 GB, about 100 s of a still screen:
do not reset the phone meanwhile), and boots the kernel. iOS's lock screen comes about a minute later; sound and the
microphone are ready about a minute after the kernel's start. At the session's end the phone is back in the loader;
`./xl reboot` brings back Android.

- `./xl screenshot shot.png` the screen; `./xl` alone lists every command. Each session's log is in `logs/`.
- **Wi-Fi comes up only in a session started from Android** (the radio does not start a second time from the
  loader): for Wi-Fi, `./xl reboot` first, then `make phone`.
- If a session hangs, the phone's watchdog resets it into Android after a while. If the next session hangs at once, the
  system image the phone still held was spoiled: `./xl reboot`, and run again (it is uploaded anew).
- The boot arguments are the configuration's (`make config` shows them, `tools/hakios-config.py args` prints them);
  `ARGS="... cam=4" make phone` and the like run with others (the head of each driver says its levels).

## Taking the common repository's updates

```sh
git fetch upstream                                # (or origin, whichever remote is oosexgang's)
devices/davinci/patches/apply.sh                  # the branch made again on the new upstream, the patches on top
make davinci_defconfig && make kernel
```

A patch that no longer applies stops on it: resolve it, `git am --continue`, then `devices/davinci/patches/export.sh`
and commit the refreshed patches in this repository.

## Known limits

- The App Store opens but loads nothing: signing in to an Apple ID and decrypting store apps need the iPhone's
  secure hardware, which this phone has not. Unsigned apps can be added to the image.
- iOS 16.5 only (the newer releases need pointer authentication, which the Snapdragon 730 lacks).
