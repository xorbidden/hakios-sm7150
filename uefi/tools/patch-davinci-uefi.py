#!/usr/bin/env python3
"""Apply the SM7150 Silicium patches (as in Mu-Silicium's Binaries/phoenix) to davinci's own UEFI drivers.

usage: patch-davinci-uefi.py <extracted BOOT.XF... directory>

Writes X.patched.efi next to each X.efi and points X.inf at it, the way Binaries/phoenix does. Every
site was located by matching phoenix's original against davinci's driver (instructions compared with
branch/address fields masked, 16 instructions of context each side, exactly one match), and the original
word is checked here before it is replaced: a different firmware build stops with an error instead of
patching the wrong place.
"""
import os, re, sys

# driver -> [(offset, original word, patched word, what)]  (little-endian bytes as hex)
PATCHES = {
    'QcomPkg/Drivers/ButtonsDxe/ButtonsDxe': [
        (0x3490, '51208052', 'b1018052', 'power key code: SUSPEND -> ENTER (mov w17, #0xd)'),
        (0x3494, 'f1330079', 'f1370079', '... stored in the other field'),
    ],
    'QcomPkg/Drivers/DisplayDxe/DisplayDxe': [
        (0x16c8, 'e1031f32', '21008052', 'MDP init without MMU init (mov w1, #1)'),
        (0x1f7c, '49030034', '1a000014', 'backlight never switched off on the charger state (cbz w9 -> b)'),
    ],
    'QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe': [
        (0x15b8, '6e010094', '000080d2', 'do not recreate an existing MMU domain (bl -> mov x0, #0)'),
        (0x1600, '5c010094', '000080d2', '...'),
        (0x1648, '4a010094', '000080d2', '...'),
        # 0x1690 (USB3-0 domain) left stock: skipping it leaves the USB controller's DMA unmapped (no USB at all)
    ],
    'QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA': [
        (0x1970, '041c0012', '04008052', 'charger init: never arm the watchdog (and w4 -> mov w4, #0)'),
        (0x1994, '051c0012', '05008052', '... (and w5 -> mov w5, #0)'),
    ],
    'QcomPkg/Drivers/UFSDxe/UFSDxe': [
        (0x24f8, 'ff0301d1', '00008052', 'MMU domain setup -> return 0 (mov w0, #0)'),
        (0x24fc, 'f44f02a9', 'c0035fd6', '... ret'),
    ],
    'QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe': [
        (0x37ac, 'eb031932', '0b008052', 'internal storage is not removable (mov w11, #0)'),
    ],
}
# NOTE: UFSDxe must stay unpatched on davinci (its inf points at UFSDxe.efi); re-running this script
# re-points it at UFSDxe.patched.efi -- restore that by hand afterwards (or drop the UFSDxe entry).

root = sys.argv[1]
for drv, sites in PATCHES.items():
    src = os.path.join(root, drv + '.efi')
    data = bytearray(open(src, 'rb').read())
    for off, old, new, what in sites:
        have = data[off:off + 4].hex()
        if have != old:
            sys.exit(f'{drv}.efi at {off:#x}: expected {old}, found {have} -- not the firmware these patches are for')
        data[off:off + 4] = bytes.fromhex(new)
        print(f'{drv}.efi {off:#06x}: {old} -> {new}  {what}')
    open(os.path.join(root, drv + '.patched.efi'), 'wb').write(data)
    inf = os.path.join(root, drv + '.inf')
    text = open(inf).read()
    name = os.path.basename(drv)
    new_text = re.sub(rf'\|{re.escape(name)}\.efi\|', f'|{name}.patched.efi|', text)
    if new_text == text and f'{name}.patched.efi' not in text:
        sys.exit(f'{inf}: no "|{name}.efi|" line to point at the patched file')
    open(inf, 'w').write(new_text)
print('done')
