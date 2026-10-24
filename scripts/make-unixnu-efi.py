#!/usr/bin/env python3
"""Carve a dedicated EFI System Partition from empty space after the unixnu image.

The unixnu GPT partition is larger than its raw root image. This script shrinks
that partition only in GPT, then adds a following ESP inside the old tail. It
checks that the complete existing unixnu image remains outside the new
partition before writing primary or backup GPT.
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
MIB = 1 << 20
ESP = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B').bytes_le
DEFAULT_IMAGE = Path('/root/mi8uefi/toolchain/UniXNU-fajita-linux/out/dipper/userdata/userdata-root.img')


def load_gpt(unixnu):
    path = unixnu / 'scripts/dual_sys/gpt.py'
    spec = importlib.util.spec_from_file_location('dipper_gpt', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, default=ROOT.parent / 'UniXNU')
    parser.add_argument('--serial')
    parser.add_argument('--disk', default='/dev/block/sda')
    parser.add_argument('--size-mib', type=int, default=256)
    parser.add_argument('--name', default='UNIXNUEFI')
    parser.add_argument('--reserved-image', type=Path, default=DEFAULT_IMAGE,
                        help='raw image that must remain before the new partition')
    parser.add_argument('--image', type=Path,
                        help='FAT image of exactly --size-mib to write into the new partition')
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'out/dipper/unixnu-efi')
    parser.add_argument('--apply', action='store_true')
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

    def read(lba, count):
        return adb_run('exec-out', f'dd if={args.disk} bs={sector} skip={lba} count={count} 2>/dev/null',
                       binary=True)

    gptmod = load_gpt(args.unixnu)
    gpt = gptmod.read_gpt(read, sector, total)
    unix = [entry for entry in gpt.entries if entry['name'] == 'unixnu']
    if len(unix) != 1:
        raise SystemExit(f'expected one unixnu partition, found {len(unix)}')
    unix = unix[0]
    reserved = args.reserved_image.stat().st_size
    reserved_end = unix['start'] + (reserved + sector - 1) // sector - 1
    size_sectors = args.size_mib * MIB // sector
    efi_start = unix['end'] + 1 - size_sectors
    efi_end = unix['end']
    if args.size_mib <= 0 or args.size_mib * MIB != size_sectors * sector:
        raise SystemExit('size must be a positive whole number of MiB')
    if efi_start <= reserved_end:
        raise SystemExit('new ESP would overlap the existing unixnu image')
    used = {entry['index'] for entry in gpt.entries}
    empty = next((index for index in range(1, gpt.h['count'] + 1) if index not in used), None)
    if empty is None:
        raise SystemExit('no free GPT entry')

    table = bytearray(gpt.table)
    struct.pack_into('<Q', table, (unix['index'] - 1) * 128 + 40, efi_start - 1)
    entry = bytearray(128)
    entry[:16] = ESP
    entry[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQQ', entry, 32, efi_start, efi_end, 0)
    name = args.name.encode('utf-16-le')
    if len(name) > 72:
        raise SystemExit('partition name is longer than 36 UTF-16 characters')
    entry[56:56 + len(name)] = name
    table[(empty - 1) * 128:empty * 128] = entry

    primary, backup = bytearray(gpt.primary), bytearray(gpt.backup)
    primary[2 * sector:2 * sector + len(table)] = table
    backup[:len(table)] = table
    for region, offset in ((primary, sector), (backup, len(backup) - sector)):
        struct.pack_into('<I', region, offset + 88, zlib.crc32(table))
        struct.pack_into('<I', region, offset + 16, 0)
        struct.pack_into('<I', region, offset + 16, zlib.crc32(region[offset:offset + 92]))
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
    print(f'unixnu image end LBA {reserved_end}; new {args.name} partition {empty}: '
          f'LBA {efi_start}..{efi_end} ({args.size_mib} MiB)')
    print(f'host preview: {output}')

    if args.image:
        expected = (efi_end - efi_start + 1) * sector
        if args.image.stat().st_size != expected:
            raise SystemExit(f'ESP image is {args.image.stat().st_size} bytes, expected {expected}')
    if not args.apply:
        print('plan only; pass --apply from TWRP to write and verify')
        return

    adb_run('shell', 'sync')
    remote = f'/tmp/dipper-{args.name.lower()}'
    adb_run('shell', f'mkdir -p {remote}')
    for name in ('gpt-backup.after', 'gpt-primary.after'):
        adb_run('push', str(output / name), f'{remote}/{name}')
    backup_lba = total - 1 - gpt.h['blocks']
    adb_run('shell', f'dd if={remote}/gpt-backup.after of={args.disk} bs={sector} seek={backup_lba} 2>/dev/null')
    adb_run('shell', f'dd if={remote}/gpt-primary.after of={args.disk} bs={sector} 2>/dev/null')
    adb_run('shell', 'sync')
    current = gptmod.read_gpt(read, sector, total)
    if current.primary != files['gpt-primary.after'] or current.backup != files['gpt-backup.after']:
        raise SystemExit('GPT readback differs; do not reboot')
    print('GPT backup and primary readback: PASS')

    if args.image:
        adb_run('push', str(args.image), f'{remote}/esp.img')
        adb_run('shell', f'dd if={remote}/esp.img of={args.disk} bs={sector} seek={efi_start} 2>/dev/null')
        adb_run('shell', 'sync')
        first = read(efi_start, 256)
        expected_first = args.image.read_bytes()[:256 * sector]
        if first != expected_first:
            raise SystemExit('ESP image readback differs at its start')
        print('ESP image start readback: PASS')

    print(f'new partition block should be /dev/block/sda{empty} after reboot')


if __name__ == '__main__':
    main()
