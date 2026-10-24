#!/usr/bin/env python3
"""Change a dipper FAT partition's GPT type to EFI System Partition.

No partition is moved, resized or erased. The default mode only reads GPT and
writes validated primary/backup previews on the host. --apply unmounts /cache,
writes the backup GPT first, then the primary GPT, and reads both back.
"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
ESP = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B').bytes_le


def load_gpt(unixnu):
    path = unixnu / 'scripts/dual_sys/gpt.py'
    spec = importlib.util.spec_from_file_location('dipper_gpt', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, default=ROOT.parent / 'UniXNU',
                        help='UniXNU checkout containing scripts/dual_sys/gpt.py')
    parser.add_argument('--serial', help='adb serial; required only with multiple devices')
    parser.add_argument('--disk', default='/dev/block/sda')
    parser.add_argument('--partition', default='cache',
                        help='partition whose GPT type becomes EFI System Partition')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/dipper/cache-esp')
    parser.add_argument('--apply', action='store_true', help='write the validated GPT regions')
    args = parser.parse_args()

    adb = [shutil.which('adb') or 'adb']
    if args.serial:
        adb += ['-s', args.serial]

    def adb_run(*command, binary=False):
        result = subprocess.run([*adb, *command], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise SystemExit(result.stderr.decode(errors='replace').strip() or f'adb failed: {command}')
        return result.stdout if binary else result.stdout.decode(errors='replace')

    sector = int(adb_run('shell', f'blockdev --getss {args.disk}').strip())
    total = int(adb_run('shell', f'blockdev --getsize64 {args.disk}').strip()) // sector
    if sector not in (512, 4096):
        raise SystemExit(f'unsupported logical sector size {sector}')

    def read(lba, count):
        command = f'dd if={args.disk} bs={sector} skip={lba} count={count} 2>/dev/null'
        return adb_run('exec-out', command, binary=True)

    gptmod = load_gpt(args.unixnu)
    gpt = gptmod.read_gpt(read, sector, total)
    target = [entry for entry in gpt.entries if entry['name'] == args.partition]
    if len(target) != 1:
        raise SystemExit(
            f'expected one GPT partition named {args.partition}, found {len(target)}')
    index = target[0]['index']

    table = bytearray(gpt.table)
    offset = (index - 1) * 128
    struct.pack_into('16s', table, offset, ESP)
    primary, backup = bytearray(gpt.primary), bytearray(gpt.backup)
    primary[2 * sector:2 * sector + len(table)] = table
    backup[:len(table)] = table
    for region, header_offset in ((primary, sector), (backup, len(backup) - sector)):
        struct.pack_into('<I', region, header_offset + 88, zlib.crc32(table))
        struct.pack_into('<I', region, header_offset + 16, 0)
        struct.pack_into('<I', region, header_offset + 16,
                         zlib.crc32(region[header_offset:header_offset + 92]))
    gptmod.GPT(bytes(primary), bytes(backup), sector, total)

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    files = {
        'gpt-primary.before': gpt.primary,
        'gpt-backup.before': gpt.backup,
        'gpt-primary.after': bytes(primary),
        'gpt-backup.after': bytes(backup),
    }
    for name, data in files.items():
        (output / name).write_bytes(data)
    print(f'{args.partition} is GPT partition {index}; type -> EFI System Partition')
    print(f'primary: {len(primary)} bytes, backup: {len(backup)} bytes')
    print(f'host preview: {output}')
    if not args.apply:
        print('plan only; pass --apply from TWRP to write and verify GPT')
        return

    adb_run('shell', f'umount /{args.partition} 2>/dev/null || true')
    remote = f'/tmp/dipper-{args.partition}-esp'
    adb_run('shell', f'mkdir -p {remote}')
    for name in ('gpt-backup.after', 'gpt-primary.after'):
        adb_run('push', str(output / name), f'{remote}/{name}')
    backup_lba = total - 1 - gpt.h['blocks']
    # toybox dd may be built without conv=notrunc. Omitting ftruncate semantics
    # is safe here because the destination is a block device.
    adb_run('shell', f'dd if={remote}/gpt-backup.after of={args.disk} bs={sector} '
                      f'seek={backup_lba} 2>/dev/null')
    adb_run('shell', f'dd if={remote}/gpt-primary.after of={args.disk} bs={sector} 2>/dev/null')
    adb_run('shell', 'sync')
    current = gptmod.read_gpt(read, sector, total)
    if current.primary != files['gpt-primary.after'] or current.backup != files['gpt-backup.after']:
        raise SystemExit('GPT readback differs; do not reboot')
    print('GPT backup and primary readback: PASS')


if __name__ == '__main__':
    main()
