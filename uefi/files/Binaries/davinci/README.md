## Firmware Info

- Device: Xiaomi Mi 9T (davinci)
- Region: EEA
- Version: `V12.1.1.0.RFJEUXM` / `BOOT.XF.3.1-00571-SM6150LZB-1`
- Source: `xbl.elf` of the fastboot ROM `davinci_eea_global_images_V12.1.1.0.RFJEUXM_20210903.0000.00_11.0_eea`,
  extracted with [UEFIReader](https://github.com/WOA-Project/UEFIReader) (`fc2e9e3`).

## Patches/Fixes

The phoenix (SM7150) patch set, transplanted to this build. Each site was located by matching phoenix's original
driver against this one (instructions compared with branch/address fields masked, 16 instructions of context on
each side, exactly one match) and is applied by `WORK_DIR/tools/patch-davinci-uefi.py` in the HackiOS workspace,
which checks the original word first.

### ButtonsDxe:

- Patch: Key code was patched for the power button to be mapped as ENTER instead of SUSPEND (`0x3490`, `0x3494`).

### DisplayDxe:

- Patch: DisplayDxe setup routine was patched to call MDP Init without initing MMU (`0x16c8`).
- Patch (HackiOS): the backlight check at `0x1f7c` (`cbz w9` → `b`) always takes the backlight-on path. Stock
  DisplayDxe asks the charger protocol (off-mode charging? power status ok?) and otherwise sets brightness 0,
  which on this AMOLED is a black screen; under Silicium the charger state is unreliable.

### HALIOMMUDxe:

- Patch: MMU related setup routine was patched to not recreate already existing MMU domains
  (`0x15b8`, `0x1600`, `0x1648`, `0x1690`).

### QcomChargerDxeLA:

- Patch: ChargerPlatform Init was patched to not enable the watchdog (`0x1970`, `0x1994`).

### UFSDxe:

- In use: **surya's** patched driver and depex (`UFSDxe.surya.patched.efi`, `UFSDxe.surya.depex`), exactly what
  phoenix ships. davinci's own driver with phoenix's own patch (`UFSDxe.patched.efi`, `0x24f8`/`0x24fc`, inf kept
  as `UFSDxe.inf.davinci-own`) crashes or hangs: a DXE bisection with a reset probe put the failure at this driver
  (probe after SdccDxe: back in Android at 16 s; after UFSDxe: 37 s, the external reset).

### UsbMsdDxe:

- Patch: Changed Removable State to Non-Removable (`0x37ac`).

### UsbConfigDxe (tried, not applied):

- The ginkgo/gta4l patch (first "platform type == CLS" check, `0x3100`, `b.ne` -> `b.eq`) sets the USB core up
  statically as a **host** (USB keyboard in the UEFI menu). The HackiOS loader then gets `EFI_NOT_FOUND` from
  `UsbDeviceStartEx`: there is no function controller. Kept as `UsbConfigDxe.cls-host.efi`, not used.

Not applied: redwood's UsbConfigDxe patch (USB kept alive after ExitBootServices). The HackiOS loader never
calls ExitBootServices and uses the UEFI's own USB device protocol.
