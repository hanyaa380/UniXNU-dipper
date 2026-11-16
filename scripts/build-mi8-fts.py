#!/usr/bin/env python3
"""Build libMi8Fts.a: the Dipper ST FTS (FTM5) touch implementation.

The source is the device-validated `mi8_fts.c` from reference/dipper-xnu,
adapted to publish through NativeHidInput's native_touch_* seqlock. It runs
after the XNU bootstrap pass has produced the recorded ARM64/iOS compile flags.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/mi8-fts'
OBJ_ROOT = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
SOURCE = ROOT / 'drivers/Mi8Fts/Mi8Fts.c'
WARNINGS = [
    '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-Wno-missing-field-initializers',
    '-Wno-missing-designated-field-initializers', '-Wno-unknown-warning-option',
]


def record(relative):
    return json.loads((OBJ_ROOT / relative).read_text().strip().rstrip(','))


def command_for(rec, source, obj):
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
    rec = record('osfmk/DEVELOPMENT/startup.o.json')
    obj = OUT / 'Mi8Fts.o'
    command = command_for(rec, SOURCE, obj) + WARNINGS + ['-mgeneral-regs-only']
    result = subprocess.run(
        command, cwd=rec['directory'], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=180,
    )
    (OUT / 'Mi8Fts.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-6000:])
    check_object(obj)
    (OUT / 'compile-commands.json').write_text(json.dumps({'Mi8Fts.c': command}, indent=2) + '\n')

    archive = OUT / 'libMi8Fts.a'
    previous = hashlib.sha256(archive.read_bytes()).hexdigest() if archive.is_file() else None
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True, timeout=60)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != previous:
        (OBJ_ROOT / 'mach.development.bcm2837.unstripped.noctf').unlink(missing_ok=True)
    (OUT / 'build-report.json').write_text(json.dumps({
        'archive_sha256': digest,
        'source_sha256': {str(SOURCE.relative_to(ROOT)): hashlib.sha256(SOURCE.read_bytes()).hexdigest()},
        'hardware_execution': False,
    }, indent=2) + '\n')
    print(f'PASS: libMi8Fts.a (sha256 {digest[:16]})')


if __name__ == '__main__':
    main()
