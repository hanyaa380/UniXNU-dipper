#!/usr/bin/env python3
"""Derive the dipper (Xiaomi Mi 8) port from Mu-Silicium's equuleus package.

dipper and equuleus are both SDM845 Xiaomi Mi 8 variants. The equuleus vendor binaries are kept as the
same-generation donor set, while the panel-specific DisplayDxe is replaced by SimpleFbDxe on the ABL
splash framebuffer. UniXNU, DarwinBoot and DipperBootMemoryDxe are added to the firmware volume, and the
shared BootShim and SEC get the uefi-sheng handoff, cache and ARM64-header fixes.

The first milestone is the UniXNU menu. Charging, panel-specific display, touch on dipper, UFS root and
the data volume remain bring-up items that must be verified on hardware.

Usage: make-dipper-uefi.py MU_SILICIUM_DIR [--force]
"""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

GUID = '9A0DE0A7-6A12-4A62-B7AA-9AD12F1DBE31'

# (file, original, replacement): each must match exactly once, so upstream drift stops the script.
EDITS = [
    ('dipper.dsc', 'PLATFORM_GUID                  = 7B28B566-C3FF-4457-A0D2-B3F43849CB07',
     f'PLATFORM_GUID                  = {GUID}'),
    ('dipper.dsc', 'USE_CUSTOM_DISPLAY_DRIVER      = 1', 'USE_CUSTOM_DISPLAY_DRIVER      = 0'),
    ('dipper.dsc', 'PcdSmbiosSystemModel|"Mi 8 Pro"', 'PcdSmbiosSystemModel|"Mi 8"'),
    ('dipper.dsc', 'PcdSmbiosSystemRetailSku|"Mi 8_pro_dipper"', 'PcdSmbiosSystemRetailSku|"Mi 8_dipper"'),
    ('dipper.dsc', 'PcdSmbiosSystemBoardModel|"Mi 8 Pro"', 'PcdSmbiosSystemBoardModel|"Mi 8"'),
    ('dipper.dsc', '  ConfigurationMapLib|dipperPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf\n',
     '  ConfigurationMapLib|dipperPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf\n'
     '\n'
     '  # UniXNU\n'
     '  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf\n'
     '  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf\n'),
    # UniXNU and DarwinBoot, as in uefi-sheng. The loader is part of the firmware volume (loader = "firmware").
    ('dipper.dsc', '  #equuleus/AcpiTables.inf\n',
     '  #equuleus/AcpiTables.inf\n\n'
     '  #\n  # UniXNU\n  #\n'
     '  UniXNUPkg/Application/UniXNU/UniXNU.inf\n'
     '  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf\n'
     '  DarwinBootPkg/Driver/DarwinBoot.inf\n'
     '  dipperPkg/Drivers/DipperBootMemoryDxe/DipperBootMemoryDxe.inf\n'
     '  dipperPkg/Drivers/DipperUsbLogDxe/DipperUsbLogDxe.inf\n'),
    ('dipper.fdf', '  INF SiliciumPkg/Drivers/SmBiosTableDxe/SmBiosTableDxe.inf\n',
     '  INF SiliciumPkg/Drivers/SmBiosTableDxe/SmBiosTableDxe.inf\n\n'
     '  # UniXNU\n'
     '  INF UniXNUPkg/Application/UniXNU/UniXNU.inf\n'
     '  INF UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf\n'
     '  INF DarwinBootPkg/Driver/DarwinBoot.inf\n'),
    # The boot image's ramdisk (/chosen linux,initrd-start/-end of the ABL's DT) as a block device, so the payload FAT
    # is a volume; loaded where uefi-sheng loads ShengBootMemoryDxe.
    ('Include/DXE.inc', '  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf\n',
     '  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf\n'
     '  INF dipperPkg/Drivers/DipperBootMemoryDxe/DipperBootMemoryDxe.inf\n'
     '  INF dipperPkg/Drivers/DipperUsbLogDxe/DipperUsbLogDxe.inf\n'),
]

# SEC of SiliciumPkg, shared by every device of the checkout; uefi-sheng c5ede6f6b and 3a7f5d4ad on the current SEC.
# x0 of the previous stage (the ABL's DT) is kept in a HOB for DipperBootMemoryDxe, and the data caches the ABL filled
# are cleaned and invalidated by set/way before the first store, so stale lines cannot hide or overwrite the HOBs.
SEC = 'Silicon/Silicium/SiliciumPkg'
SEC_MARK = 'gSiliciumEntryArgHobGuid'
SEC_EDITS = [
    ('SiliciumPkg.dec', '[Guids]\n  gSiliciumPkgTokenSpaceGuid          = { 0x055720A6, 0xA739, 0x0047, { 0xBE, 0xC1, 0x67, 0xFC, 0x38, 0x34, 0x54, 0x53 } }\n',
     '[Guids]\n  gSiliciumPkgTokenSpaceGuid          = { 0x055720A6, 0xA739, 0x0047, { 0xBE, 0xC1, 0x67, 0xFC, 0x38, 0x34, 0x54, 0x53 } }\n\n'
     '  # HOB holding the first argument (x0) the previous stage entered SEC with.\n'
     '  # A Linux-style boot loader passes the physical address of a flattened DT.\n'
     '  gSiliciumEntryArgHobGuid            = { 0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 } }\n'),
    ('Sec/Sec.inf', '[Guids]\n  gArmMpCoreInfoGuid\n', '[Guids]\n  gArmMpCoreInfoGuid\n  gSiliciumEntryArgHobGuid\n'),
    ('Sec/Sec.c', '#include "Sec.h"\n',
     '#include "Sec.h"\n\n'
     '// First argument (x0) of the previous stage, saved by ModuleEntryPoint.S\n'
     'extern UINT64  gSecEntryArg0;\n\n'
     'VOID\nSecCleanInvalidateDataCacheAll (\n  VOID\n  );\n'),
    ('Sec/Sec.c', '  // Build CPU HOB\n',
     '  // Pass on what the previous stage handed over in x0 (a DT address for a\n'
     '  // Linux-style boot loader), so a DXE driver can use it.\n'
     '  {\n    UINT64  EntryArg = gSecEntryArg0;\n\n'
     '    BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg, sizeof (EntryArg));\n  }\n\n'
     '  // Build CPU HOB\n'),
    ('Sec/Sec.c', 'SecEntry (VOID)\n{\n  // Disable Data Cache\n',
     'SecEntry (VOID)\n{\n'
     '  // The previous stage ran with its caches on. Clean and invalidate them before anything else: the UEFI writes its HOB tables and heap with\n'
     '  // the MMU and caches off, and lines the previous stage left would hide those writes (or be written back over them) once the caches are on.\n'
     '  SecCleanInvalidateDataCacheAll ();\n\n'
     '  // Disable Data Cache\n'),
    ('Sec/AArch64/ModuleEntryPoint.S', 'ASM_FUNC (_ModuleEntryPoint)\n  // Do Platform Specific Initialization\n',
     'ASM_FUNC (_ModuleEntryPoint)\n'
     '  // Before the first store of this stage: clean and invalidate the data caches the previous stage filled (x0 is kept in x20).\n'
     '  mov   x20, x0\n  bl    ASM_PFX (SecCleanInvalidateDataCacheAll)\n  mov   x0, x20\n\n'
     '  // Keep x0 for the platform. It is the address of the DT a Linux-style boot\n'
     '  // loader hands over. The slot is in this section, which is in RAM here.\n'
     '  adr   x1, ASM_PFX (gSecEntryArg0)\n  str   x0, [x1]\n\n'
     '  // Do Platform Specific Initialization\n'),
]
SEC_APPEND = {
    'Sec/AArch64/ModuleEntryPoint.S': '''
  // First argument of the previous stage, written once at entry
  .align 3
  .global ASM_PFX (gSecEntryArg0)
ASM_PFX (gSecEntryArg0):
  .quad 0
''',
}
SEC_HELPER = '''//
// Cleans and invalidates every data cache level up to the Level of Coherence by set/way (the standard sequence: CLIDR_EL1 levels,
// CSSELR_EL1 to select, CCSIDR_EL1 geometry, DC CISW). The previous stage ran with its caches on and leaves lines behind that the UEFI,
// running with the MMU and caches off until ArmConfigureMmu, never wrote: once the caches are on, those stale lines would hide the
// DRAM contents (HOB tables, heap) written in between, or be written back over them. Cleaning keeps what the previous stage wrote.
// Corrupts x0-x11; callable with the MMU on or off.
//
ASM_FUNC (SecCleanInvalidateDataCacheAll)
  dmb   sy
  mrs   x0, clidr_el1
  and   x3, x0, #0x7000000          // LoC, shifted
  lsr   x3, x3, #23                 // LoC * 2
  cbz   x3, 5f
  mrs   x1, id_aa64mmfr2_el1
  ubfx  x8, x1, #20, #4             // CCIDX
  mov   x10, #0                     // level * 2
1:
  add   x2, x10, x10, lsr #1
  lsr   x1, x0, x2
  and   x1, x1, #7                  // cache type of this level
  cmp   x1, #2
  b.lt  4f                          // no data cache here
  msr   csselr_el1, x10
  isb
  mrs   x1, ccsidr_el1
  and   x2, x1, #7
  add   x2, x2, #4                  // log2 (line bytes)
  cbz   x8, 6f
  ubfx  x4, x1, #3, #21             // CCIDX: associativity - 1
  ubfx  x7, x1, #32, #24            // CCIDX: sets - 1
  b     7f
6:
  ubfx  x4, x1, #3, #10             // associativity - 1
  ubfx  x7, x1, #13, #15            // sets - 1
7:
  clz   w5, w4                      // bit position of the way field
2:
  mov   x9, x4
3:
  lsl   x6, x9, x5
  orr   x11, x10, x6                // way and level
  lsl   x6, x7, x2
  orr   x11, x11, x6                // set
  dc    cisw, x11
  subs  x9, x9, #1
  b.ge  3b
  subs  x7, x7, #1
  b.ge  2b
4:
  add   x10, x10, #2
  cmp   x3, x10
  b.gt  1b
5:
  mov   x10, #0
  msr   csselr_el1, x10
  dsb   sy
  isb
  ret

'''

# BootShim, shared by every device of the checkout; the uefi-sheng fixes. The ARM64 header describes the shim
# plus the firmware volume, which unixnu bootimg checks, and the copied firmware volume is cleaned to the point
# of coherency and the instruction cache invalidated before the jump into it.
SHIM = 'BootShim/BootShim.S'
SHIM_MARK = '/* Complete image */'
SHIM_EDITS = [
    ('  .quad FD_SIZE         /* Image Size */', f'  .quad FD_SIZE + (_Payload - _Head) + APPENDED_PAYLOAD_SIZE {SHIM_MARK}'),
    ('  b.ne  _CopyLoop\n  ldr   x5, =FD_BASE\n',
     '  b.ne  _CopyLoop\n  ldr   x5, =FD_BASE\n\n'
     '  /* Publish the copied instructions before executing them. */\n'
     '  dsb   sy\n  mrs   x9, ctr_el0\n  ubfx  x9, x9, #16, #4\n  mov   x10, #4\n  lsl   x9, x10, x9\n'
     '  sub   x10, x9, #1\n  bic   x8, x5, x10\n  ldr   x7, =FD_SIZE\n  add   x7, x5, x7\n\n'
     '_CleanLoop:\n  dc    cvac, x8\n  add   x8, x8, x9\n  cmp   x8, x7\n  b.lo  _CleanLoop\n'
     '  dsb   sy\n  ic    iallu\n  dsb   sy\n  isb\n'),
    ('  b     _Dead\n', '  b     _Dead\n.ltorg\n'),
]

# Optional payload carried after the firmware volume in the kernel stream.
APPENDED_SHIM_MARK = 'APPENDED_PAYLOAD_SIZE'
APPENDED_SHIM_EDITS = [
    ('_Head:\n',
     '#ifndef APPENDED_PAYLOAD_SIZE\n'
     '#define APPENDED_PAYLOAD_SIZE 0\n'
     '#endif\n\n'
     '_Head:\n'),
    ('_Start:\n  mov   x4, x1\n',
     '_Start:\n  mov   x20, x1\n  mov   x4, x1\n'),
    ('_Entry:\n  br    x5\n',
     '_Entry:\n'
     '  ldr   x2, =APPENDED_PAYLOAD_SIZE\n'
     '  cbz   x2, 7f\n'
     '  ldr   x1, =FD_SIZE\n'
     '  add   x1, x20, x1\n'
     '7:\n'
     '  br    x5\n'),
]

APPENDED_SEC_MARK = 'gSiliciumAppendedPayloadHobGuid'
APPENDED_SEC_EDITS = [
    ('SiliciumPkg.dec',
     '  gSiliciumEntryArgHobGuid            = { 0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 } }\n',
     '  gSiliciumEntryArgHobGuid            = { 0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 } }\n'
     '  gSiliciumAppendedPayloadHobGuid     = { 0x31D1E4E5, 0x9B11, 0x4B44, { 0x8F, 0x93, 0x31, 0xFF, 0x68, 0xDB, 0x47, 0xB8 } }\n'),
    ('Sec/Sec.inf',
     '  gSiliciumEntryArgHobGuid\n',
     '  gSiliciumEntryArgHobGuid\n  gSiliciumAppendedPayloadHobGuid\n'),
    ('Sec/Sec.c',
     'extern UINT64  gSecEntryArg0;\n',
     'extern UINT64  gSecEntryArg0;\nextern UINT64  gSecPayloadAddress;\nextern UINT64  gSecPayloadSize;\n'),
    ('Sec/Sec.c',
     "    BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg, sizeof (EntryArg));\n  }\n",
     "    BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg, sizeof (EntryArg));\n  }\n"
     "  if (gSecPayloadSize != 0) {\n"
     "    UINT64  Payload[2] = { gSecPayloadAddress, gSecPayloadSize };\n\n"
     "    BuildGuidDataHob (&gSiliciumAppendedPayloadHobGuid, Payload, sizeof (Payload));\n"
     "  }\n"),
    ('Sec/AArch64/ModuleEntryPoint.S',
     '  mov   x20, x0\n  bl    ASM_PFX (SecCleanInvalidateDataCacheAll)\n',
     '  mov   x20, x0\n  mov   x21, x1\n  mov   x22, x2\n  bl    ASM_PFX (SecCleanInvalidateDataCacheAll)\n'),
    ('Sec/AArch64/ModuleEntryPoint.S',
     '  adr   x1, ASM_PFX (gSecEntryArg0)\n  str   x0, [x1]\n',
     '  adr   x1, ASM_PFX (gSecEntryArg0)\n  str   x0, [x1]\n'
     '  adr   x1, ASM_PFX (gSecPayloadAddress)\n  str   x21, [x1]\n'
     '  adr   x1, ASM_PFX (gSecPayloadSize)\n  str   x22, [x1]\n'),
    ('Sec/AArch64/ModuleEntryPoint.S',
     'ASM_PFX (gSecEntryArg0):\n  .quad 0\n',
     'ASM_PFX (gSecEntryArg0):\n  .quad 0\n\n'
     '  .align 3\n'
     '  .global ASM_PFX (gSecPayloadAddress)\n'
     'ASM_PFX (gSecPayloadAddress):\n  .quad 0\n'
     '  .global ASM_PFX (gSecPayloadSize)\n'
     'ASM_PFX (gSecPayloadSize):\n  .quad 0\n'),
]

RAM_READY_MARK = 'gSiliciumRamReadyProtocolGuid'
RAM_READY_EDITS = [
    ('Silicon/Silicium/SiliciumPkg/SiliciumPkg.dec',
     '  gKeypadDeviceProtocolGuid           = { 0xA412C1AC, 0x0295, 0x4AE3, { 0xB5, 0x9F, 0x52, 0x23, 0x1F, 0x26, 0x55, 0x4D } }\n',
     '  gKeypadDeviceProtocolGuid           = { 0xA412C1AC, 0x0295, 0x4AE3, { 0xB5, 0x9F, 0x52, 0x23, 0x1F, 0x26, 0x55, 0x4D } }\n'
     '  # Installed after RamManagerDxe has mapped all usable DRAM.\n'
     f'  {RAM_READY_MARK}       = {{ 0x7DABE6D8, 0x4B62, 0x4E47, {{ 0xA8, 0x71, 0x36, 0x0D, 0x8C, 0x25, 0x93, 0x51 }} }}\n'),
    ('Silicon/Silicium/SiliciumPkg/Drivers/RamManagerDxe/RamManagerDxe.inf',
     '[Depex]\n  TRUE\n',
     '[Protocols]\n'
     f'  {RAM_READY_MARK}\n\n'
     '[Depex]\n  TRUE\n'),
    ('Silicon/Silicium/SiliciumPkg/Drivers/RamManagerDxe/RamManager.c',
     'STATIC UINTN                  DescriptorSize   = 0;\n',
     'STATIC UINTN                  DescriptorSize   = 0;\n'
     'STATIC UINT8                  mRamReadyProtocol;\n'),
    ('Silicon/Silicium/SiliciumPkg/Drivers/RamManagerDxe/RamManager.c',
     '  for (UINT8 i = 0; i < MemoryRangeCount; i++) {\n'
     '    // Parse Memory Range\n'
     '    ParseMemoryRange (MemoryRange[i].Address, (EFI_PHYSICAL_ADDRESS)(MemoryRange[i].Address + MemoryRange[i].Length));\n'
     '  }\n\n'
     'cleanup:\n',
     '  for (UINT8 i = 0; i < MemoryRangeCount; i++) {\n'
     '    // Parse Memory Range\n'
     '    ParseMemoryRange (MemoryRange[i].Address, (EFI_PHYSICAL_ADDRESS)(MemoryRange[i].Address + MemoryRange[i].Length));\n'
     '  }\n\n'
     '  if (!EFI_ERROR (Status)) {\n'
     '    EFI_HANDLE  Handle = NULL;\n\n'
     f'    Status = gBS->InstallProtocolInterface (&Handle, &{RAM_READY_MARK},\n'
     '                                            EFI_NATIVE_INTERFACE, &mRamReadyProtocol);\n'
     '  }\n\n'
     'cleanup:\n'),
]

# References that stay on equuleus: same-generation Xiaomi SDM845 vendor binaries and raw files.
KEEP = ['Binaries/equuleus/', 'equuleus/AcpiTables.inf']

# The static DTB appended to the boot image is only a fallback; ABL passes its own runtime DT in x0.
# equuleus is board-id <0x4f 0>, dipper is <0x36 0> (NekokeCore/edk2-dipper).
DTS_EDITS = [
    ('model = "Xiaomi Technologies, Inc. Equuleus MP v2.1";', 'model = "Xiaomi Technologies, Inc. Dipper MP v2.1";'),
    ('qcom,msm-id = <0x141 0x20001>;', 'qcom,msm-id = <0x141 0x20000>;'),
    ('qcom,board-id = <0x4f 0x00>;', 'qcom,board-id = <0x36 0x00>;'),
]


def read(path):
    """The text of a file and its line ending; files keep the line ending they have (several of the checkout use CRLF)."""
    with path.open(newline='') as stream:
        text = stream.read()
    return text, '\r\n' if '\r\n' in text else '\n'


def replace_once(path, original, replacement):
    text, eol = read(path)
    original, replacement = original.replace('\n', eol), replacement.replace('\n', eol)
    count = text.count(original)
    if count != 1:
        raise SystemExit(f'{path}: expected one match of {original!r}, found {count}')
    path.write_text(text.replace(original, replacement), newline='')


def append(path, addition):
    text, eol = read(path)
    path.write_text(text + addition.replace('\n', eol), newline='')


def rename_text(path):
    text, _ = read(path)
    for keep in KEEP:
        text = text.replace(keep, keep.replace('equuleus', '\0'))
    text = text.replace('equuleus', 'dipper').replace('\0', 'equuleus')
    path.write_text(text, newline='')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('uefi', type=Path, help='Mu-Silicium checkout')
    parser.add_argument('--force', action='store_true', help='replace an existing dipper port')
    args = parser.parse_args()

    uefi = args.uefi.resolve()
    source = uefi / 'Platforms/Xiaomi/equuleusPkg'
    target = uefi / 'Platforms/Xiaomi/dipperPkg'
    if not source.is_dir():
        raise SystemExit(f'not a Mu-Silicium checkout with equuleusPkg: {uefi}')
    outputs = [target, uefi / 'Resources/Configs/dipper.toml', uefi / 'Resources/DTBs/dipper.dts']
    if any(path.exists() for path in outputs) and not args.force:
        raise SystemExit('dipper port already present; pass --force to replace it')
    if target.exists():
        shutil.rmtree(target)

    shutil.copytree(source, target)
    for suffix in ('dsc', 'fdf', 'dec'):
        (target / f'equuleus.{suffix}').rename(target / f'dipper.{suffix}')
    for path in target.rglob('*'):
        if path.is_file() and path.suffix in ('.dsc', '.fdf', '.dec', '.inc', '.inf', '.py', '.c', '.h'):
            rename_text(path)
    shutil.copytree(ROOT / 'uefi/DipperBootMemoryDxe', target / 'Drivers/DipperBootMemoryDxe')
    shutil.copytree(ROOT / 'uefi/DipperUsbLogDxe', target / 'Drivers/DipperUsbLogDxe')
    if (ROOT / 'uefi/EmbeddedPayload').is_dir():
        shutil.copytree(ROOT / 'uefi/EmbeddedPayload', target / 'EmbeddedPayload')
    (target / 'dipper.dec').write_text(
        '[Defines]\n'
        '  DEC_SPECIFICATION = 0x00010005\n'
        '  PACKAGE_NAME      = dipperPkg\n'
        '  PACKAGE_GUID      = 6C9D71B2-20D8-4E78-A9A5-A8D945FC4A11\n'
        '  PACKAGE_VERSION   = 0.1\n\n'
        '[Guids]\n'
        '  gDipperPayloadFileGuid = { 0xD8B745A4, 0x0D49, 0x4A76, '
        '{ 0x9E, 0x74, 0x79, 0xD7, 0xB7, 0xD5, 0x3A, 0x61 } }\n'
    )
    for name, original, replacement in EDITS:
        replace_once(target / name, original, replacement)

    sec = uefi / SEC
    if SEC_MARK in read(sec / 'SiliciumPkg.dec')[0]:
        print(f'SEC: {sec} already patched')
    else:
        for name, original, replacement in SEC_EDITS:
            replace_once(sec / name, original, replacement)
        for name, text in SEC_APPEND.items():
            append(sec / name, text)
        replace_once(sec / 'Sec/AArch64/Helper.S', 'ASM_FUNCTION_REMOVE_IF_UNREFERENCED', SEC_HELPER + 'ASM_FUNCTION_REMOVE_IF_UNREFERENCED')
        print(f'SEC: {sec} patched')

    shim = uefi / SHIM
    if SHIM_MARK in read(shim)[0]:
        print(f'boot shim: {shim} already patched')
    else:
        for original, replacement in SHIM_EDITS:
            replace_once(shim, original, replacement)
        print(f'boot shim: {shim} patched')

    if APPENDED_SHIM_MARK in read(shim)[0]:
        print('appended payload shim: already patched')
    else:
        for original, replacement in APPENDED_SHIM_EDITS:
            replace_once(shim, original, replacement)
        print('appended payload shim: patched')

    if APPENDED_SEC_MARK in read(sec / 'SiliciumPkg.dec')[0]:
        print('appended payload SEC: already patched')
    else:
        for name, original, replacement in APPENDED_SEC_EDITS:
            replace_once(sec / name, original, replacement)
        print('appended payload SEC: patched')

    if RAM_READY_MARK in read(uefi / RAM_READY_EDITS[0][0])[0]:
        print('RAM-ready protocol: already patched')
    else:
        for name, original, replacement in RAM_READY_EDITS:
            replace_once(uefi / name, original, replacement)
        print('RAM-ready protocol: patched')

    shutil.copyfile(uefi / 'Resources/Configs/equuleus.toml', outputs[1])
    replace_once(outputs[1], 'header_version = 1', 'header_version = 0')

    dts = outputs[2]
    shutil.copyfile(uefi / 'Resources/DTBs/equuleus.dts', dts)
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
