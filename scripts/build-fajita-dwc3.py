#!/usr/bin/env python3
"""Compile the fajita NativeDwc3 USB device-mode probe against this source kernel's
actual ARM64/iOS 17 IOKit ABI and audit the resulting Mach-O, the way
scripts/build-arm-driver.py does for the other built-in drivers.

Output (ignored, never committed): build/fajita-dwc3/libNativeDwc3.a, force-loaded
into the kernel through CONFIG_XNU_EXTRA_LD_ARCHIVES (board.toml / fajita_defconfig).
The driver is default-off; it reads nothing unless native-dwc3-probe=1 is set.
"""
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/NativeDwc3/NativeDwc3.cpp'
OUT = ROOT / 'build/fajita-dwc3'
OUT.mkdir(parents=True, exist_ok=True)
DEST = OUT / 'NativeDwc3.o'

# The recorded compile command of a representative IOKit C++ translation unit: the
# same recipe build-arm-driver.py replays, so the driver is built with the exact
# flags, defines and header set of this kernel's IOKit.
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))

command = []
index = 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(SOURCE))
    elif value == '-o':
        command.extend(['-o', str(DEST)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
                '-Wno-missing-designated-field-initializers',
                '-mgeneral-regs-only',
                '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-I', str(ROOT / 'drivers/NativeDwc3'),
                '-I', str(ROOT / 'drivers/NativeUfs')])
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(OUT / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])

# Audit the Mach-O: ARM64 (cpu 0x100000c, subtype 0), iOS deployment (LC_BUILD_VERSION
# platform 2, min 0x110000 = 17.0), the same checks build-arm-driver.py applies.
data = DEST.read_bytes()
magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('Unexpected NativeDwc3 object architecture')
offset = 32
deployment = None
for _ in range(count):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:   # LC_BUILD_VERSION
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
if deployment != (2, 0x110000):
    raise SystemExit(f'Unexpected NativeDwc3 deployment {deployment}')

archive = OUT / 'libNativeDwc3.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(DEST)], check=True)
print('NativeDwc3: -Werror build and ARM64/iOS 17 object audit passed')
