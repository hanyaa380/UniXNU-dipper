#!/usr/bin/env python3
"""Build libFajitaCpuFreq.a: the fajita CPU clock probe and control (package/FajitaCpuFreq.cpp, fajita-cpufreq=<n>, default off)
against the recorded XNU compile flags (those of iokit/IOPlatformExpert.cpo, as build-fajita-touch.py).

Linked with -force_load through CONFIG_XNU_EXTRA_LD_ARCHIVES. It defines sheng_cpufreq_poll, native_cpufreq_row,
native_cpufreq_stage and fajita_cpufreq_rows, which take precedence over the weak stand-ins of BootProgressStubs.c. A changed
archive removes the linked kernel so that the next link uses it (the extra archives carry no make dependency).
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/fajita-cpufreq'
OBJ_ROOT = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
KIT = ROOT / 'kits/fajita'
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
            '-Wno-missing-designated-field-initializers']
INCLUDES = ['-I', str(KIT), '-I', str(ROOT / '.cache/xnu-work/osfmk')]


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
    rec = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
    source = KIT / 'FajitaCpuFreq.cpp'
    obj = OUT / 'FajitaCpuFreq.o'
    command = command_for(rec, source, obj) + WARNINGS + INCLUDES
    result = subprocess.run(command, cwd=rec['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    (OUT / 'FajitaCpuFreq.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-4000:])
    check_object(obj)
    (OUT / 'compile-commands.json').write_text(json.dumps({source.name: command}, indent=2) + '\n')
    archive = OUT / 'libFajitaCpuFreq.a'
    previous = hashlib.sha256(archive.read_bytes()).hexdigest() if archive.is_file() else None
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True, timeout=60)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != previous:
        (OBJ_ROOT / 'mach.development.bcm2837.unstripped.noctf').unlink(missing_ok=True)
    (OUT / 'build-report.json').write_text(json.dumps({
        'archive_sha256': digest,
        'source_sha256': {str(source.relative_to(ROOT)): hashlib.sha256(source.read_bytes()).hexdigest()},
        'hardware_execution': False,
    }, indent=2) + '\n')
    print(f'PASS: libFajitaCpuFreq.a (sha256 {digest[:16]})')


if __name__ == '__main__':
    main()
