#!/usr/bin/env python3
"""Build libFajitaTouch.a: the fajita touchscreen bring-up (package/FajitaTouchProbe.cpp, the GENI I2C engine of
drivers/NativeGeniI2c and the RMI4 core of drivers/NativeRmi4Touch) against the recorded XNU compile flags.

Runs as a [kernel] archives builder, after the bootstrap pass left the compile records. The C++ probe is compiled with the flags of
iokit/IOPlatformExpert.cpo (as build-arm-driver.py does), the two C libraries with those of osfmk/startup.o plus
-mgeneral-regs-only (as sheng's GENI SPI and Novatek objects). The archive is linked with -force_load through
CONFIG_XNU_EXTRA_LD_ARCHIVES; it defines the native_touch_* diagnostics and native_touch_probe, which take precedence over the weak
stand-ins of BootProgressStubs.c, and the touch snapshot NativeHidInput reads (native_touch_seq/count/xs/ys/ids,
native_touch_set_sink; the display archive no longer carries a stand-in for it). It needs nothing of the display, HID or keystore archive: NativeHidInput
consumes its globals and calls native_touch_set_sink when it is linked, and nothing breaks when it is not.

The kernel build does not track this archive (the extra archives carry no make dependency), so a changed archive removes the
linked kernel to make the next link use it.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/fajita-touch'
OBJ_ROOT = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
KIT = ROOT / 'kits/fajita'
I2C = ROOT / 'drivers/NativeGeniI2c'
RMI = ROOT / 'drivers/NativeRmi4Touch'
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
            '-Wno-missing-designated-field-initializers']
# drivers/NativeMobileDisplay last: FajitaDisplayLogical.h (the display's logical mode) once fajita's copy of that driver carries it.
INCLUDES = ['-I', str(KIT), '-I', str(I2C), '-I', str(RMI), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-I', str(ROOT / 'drivers/NativeMobileDisplay')]


def record(relative):
    return json.loads((OBJ_ROOT / relative).read_text().strip().rstrip(','))


def command_for(rec, source, obj):
    """The recorded compile with another source and object, and without the record's own warnings."""
    command, args, i = [], rec['arguments'], 0
    while i < len(args):
        arg = args[i]
        if arg == rec['file']:
            command.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            command.append(arg)
        i += 1
    return command


def check_object(path):
    data = path.read_bytes()
    magic, cpu, subtype, kind = struct.unpack_from('<4I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'{path.name}: not an arm64 Mach-O object')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cpp_rec, c_rec = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json')
    units = [
        (cpp_rec, KIT / 'FajitaTouchProbe.cpp', []),
        (c_rec, I2C / 'GeniI2c.c', ['-mgeneral-regs-only']),
        (c_rec, RMI / 'Rmi4.c', ['-mgeneral-regs-only']),
    ]
    objects, commands = [], {}
    for rec, source, extra in units:
        obj = OUT / (source.stem + '.o')
        command = command_for(rec, source, obj) + WARNINGS + extra + INCLUDES
        commands[source.name] = command
        result = subprocess.run(command, cwd=rec['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-4000:])
        check_object(obj)
        objects.append(obj)
    (OUT / 'compile-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    archive = OUT / 'libFajitaTouch.a'
    previous = hashlib.sha256(archive.read_bytes()).hexdigest() if archive.is_file() else None
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=60)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != previous:
        (OBJ_ROOT / 'mach.development.bcm2837.unstripped.noctf').unlink(missing_ok=True)
    sources = [KIT / 'FajitaTouchProbe.cpp', KIT / 'FajitaTouchBoard.h', I2C / 'GeniI2c.c', I2C / 'GeniI2c.h', RMI / 'Rmi4.c',
               RMI / 'Rmi4.h']
    display = ROOT / 'drivers/NativeMobileDisplay/FajitaDisplayLogical.h'
    if display.is_file():
        sources.append(display)
    (OUT / 'build-report.json').write_text(json.dumps({
        'archive_sha256': digest,
        'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        'hardware_execution': False,
    }, indent=2) + '\n')
    print(f'PASS: libFajitaTouch.a ({len(objects)} objects, sha256 {digest[:16]})')


if __name__ == '__main__':
    main()
