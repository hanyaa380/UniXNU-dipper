#!/usr/bin/env python3
"""Derive the fajita (OnePlus 6T) port of a Mu-Silicium checkout from its enchilada (OnePlus 6) port.

The 6 and 6T share the SDM845 SoC, the memory map and the ABL splash region (sdm845-oneplus-common.dtsi).
They differ in the panel, the board ID the ABL matches against and the product strings. The vendor DXE
binaries of enchilada are kept; the panel-specific DisplayDxe is replaced by SimpleFbDxe on the splash
framebuffer the ABL leaves running. UniXNU and DarwinBoot are added to the firmware volume, and the shared
BootShim gets the uefi-sheng fixes.

Usage: make-fajita-uefi.py MU_SILICIUM_DIR [--force]
"""
import argparse
from pathlib import Path
import shutil
import subprocess

GUID = 'D1002BDC-AE28-4F9C-ADD1-89AE1E5AE171'

# (file, original, replacement): each must match exactly once, so upstream drift stops the script.
EDITS = [
    ('fajita.dsc', 'PLATFORM_GUID                  = F8A6C376-C5D3-4F61-B911-CE778BCB5DEB',
     f'PLATFORM_GUID                  = {GUID}'),
    ('fajita.dsc', 'USE_CUSTOM_DISPLAY_DRIVER      = 1', 'USE_CUSTOM_DISPLAY_DRIVER      = 0'),
    ('fajita.dsc', 'PcdSmbiosSystemModel|"6"', 'PcdSmbiosSystemModel|"6T"'),
    ('fajita.dsc', 'PcdSmbiosSystemRetailSku|"A6003"', 'PcdSmbiosSystemRetailSku|"A6013"'),
    ('fajita.dsc', 'PcdSmbiosSystemBoardModel|"17819"', 'PcdSmbiosSystemBoardModel|"18801"'),
    ('fajita.dsc', 'PcdFrameBufferHeight|2280', 'PcdFrameBufferHeight|2340'),
    # UniXNU and DarwinBoot, as in uefi-sheng. The loader is part of the firmware volume (loader = "firmware").
    ('fajita.dsc', '  enchilada/AcpiTables.inf\n',
     '  enchilada/AcpiTables.inf\n\n'
     '  #\n  # UniXNU\n  #\n'
     '  UniXNUPkg/Application/UniXNU/UniXNU.inf\n'
     '  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf\n'
     '  DarwinBootPkg/Driver/DarwinBoot.inf\n\n'
     '[LibraryClasses]\n'
     '  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf\n'
     '  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf\n'),
    ('fajita.fdf', '  INF SiliciumPkg/Drivers/SmBiosTableDxe/SmBiosTableDxe.inf\n',
     '  INF SiliciumPkg/Drivers/SmBiosTableDxe/SmBiosTableDxe.inf\n\n'
     '  # UniXNU\n'
     '  INF UniXNUPkg/Application/UniXNU/UniXNU.inf\n'
     '  INF UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf\n'
     '  INF DarwinBootPkg/Driver/DarwinBoot.inf\n'),
]

# BootShim, shared by every device of the checkout; the uefi-sheng fixes. The ARM64 header describes the shim
# plus the firmware volume, which unixnu bootimg checks, and the copied firmware volume is cleaned to the point
# of coherency and the instruction cache invalidated before the jump into it.
SHIM = 'BootShim/BootShim.S'
SHIM_MARK = '/* Complete shim and payload */'
SHIM_EDITS = [
    ('  .quad FD_SIZE         /* Image Size */', f'  .quad FD_SIZE + (_Payload - _Head) {SHIM_MARK}'),
    ('  b.ne  _CopyLoop\n  ldr   x5, =FD_BASE\n',
     '  b.ne  _CopyLoop\n  ldr   x5, =FD_BASE\n\n'
     '  /* Publish the copied instructions before executing them. */\n'
     '  dsb   sy\n  mrs   x9, ctr_el0\n  ubfx  x9, x9, #16, #4\n  mov   x10, #4\n  lsl   x9, x10, x9\n'
     '  sub   x10, x9, #1\n  bic   x8, x5, x10\n  ldr   x7, =FD_SIZE\n  add   x7, x5, x7\n\n'
     '_CleanLoop:\n  dc    cvac, x8\n  add   x8, x8, x9\n  cmp   x8, x7\n  b.lo  _CleanLoop\n'
     '  dsb   sy\n  ic    iallu\n  dsb   sy\n  isb\n'),
    ('  b     _Dead\n', '  b     _Dead\n.ltorg\n'),
]

# References that stay on enchilada: the vendor binaries and the ACPI tables of the same SoC.
KEEP = ['Binaries/enchilada/', 'enchilada/AcpiTables.inf']

# qcom,board-id of sdm845-oneplus-fajita.dts: <8 0 18801 41>.
DTS_EDITS = [
    ('qcom,board-id = <0x08 0x00 0x459b 0x16>;', 'qcom,board-id = <0x08 0x00 0x4971 0x29>;'),
]


def replace_once(path, original, replacement):
    text = path.read_text()
    count = text.count(original)
    if count != 1:
        raise SystemExit(f'{path}: expected one match of {original!r}, found {count}')
    path.write_text(text.replace(original, replacement))


def rename_text(path):
    text = path.read_text()
    for keep in KEEP:
        text = text.replace(keep, keep.replace('enchilada', '\0'))
    text = text.replace('enchilada', 'fajita').replace('\0', 'enchilada')
    path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('uefi', type=Path, help='Mu-Silicium checkout')
    parser.add_argument('--force', action='store_true', help='replace an existing fajita port')
    args = parser.parse_args()

    uefi = args.uefi.resolve()
    source = uefi / 'Platforms/OnePlus/enchiladaPkg'
    target = uefi / 'Platforms/OnePlus/fajitaPkg'
    if not source.is_dir():
        raise SystemExit(f'not a Mu-Silicium checkout with enchiladaPkg: {uefi}')
    outputs = [target, uefi / 'Resources/Configs/fajita.toml', uefi / 'Resources/DTBs/fajita.dts']
    if any(path.exists() for path in outputs) and not args.force:
        raise SystemExit('fajita port already present; pass --force to replace it')
    if target.exists():
        shutil.rmtree(target)

    shutil.copytree(source, target)
    for suffix in ('dsc', 'fdf', 'dec'):
        (target / f'enchilada.{suffix}').rename(target / f'fajita.{suffix}')
    for path in target.rglob('*'):
        if path.is_file() and path.suffix in ('.dsc', '.fdf', '.dec', '.inc', '.inf', '.py', '.c', '.h'):
            rename_text(path)
    for name, original, replacement in EDITS:
        replace_once(target / name, original, replacement)

    shim = uefi / SHIM
    if SHIM_MARK in shim.read_text():
        print(f'boot shim: {shim} already patched')
    else:
        for original, replacement in SHIM_EDITS:
            replace_once(shim, original, replacement)
        print(f'boot shim: {shim} patched')

    shutil.copyfile(uefi / 'Resources/Configs/enchilada.toml', outputs[1])

    dts = outputs[2]
    shutil.copyfile(uefi / 'Resources/DTBs/enchilada.dts', dts)
    for original, replacement in DTS_EDITS:
        replace_once(dts, original, replacement)
    dtb = dts.with_suffix('.dtb')
    if shutil.which('dtc'):
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtb), str(dts)], check=True)
        print(f'device tree: {dtb}')
    else:
        print(f'dtc not found (dnf install dtc, or brew install dtc); before unixnu bootimg, run:\n'
              f'  dtc -I dts -O dtb -o {dtb} {dts}')

    print(f'firmware port: {target}')


if __name__ == '__main__':
    main()
