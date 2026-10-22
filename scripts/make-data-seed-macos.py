#!/usr/bin/env python3
"""Build the fajita Data volume seed (/private/var) on macOS from data-seed.json, packed to DBRDISK1.

  make-data-seed-macos.py --packer PATH/packed_ramdisk.py [--spec data-seed.json] [--out DIR]

A raw image of size_mib MiB is attached without mounting; diskutil makes an APFS container on it and adds one
case-sensitive, unencrypted volume with the spec's role (none: iOS refuses an unencrypted Data-role volume). The volume is mounted with ownership enabled, the directories
of the spec are created with their owners and modes, and anything macOS adds on mount (.fseventsd, Spotlight) is
removed before the image is detached. The raw image is packed with packed_ramdisk.pack (only nonzero pages kept).
Writes data-macos.dbr and data-macos.json (volume UUID for CONFIG_XNU_OWNER_DATA_VOLUME_UUID) to --out.
Needs sudo for chown (GitHub's macOS runners have it). No Apple files, keys or user records go into the seed."""
import argparse
import hashlib
import importlib.util
import json
import os
import plistlib
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAC_EXTRAS = ('.fseventsd', '.Spotlight-V100', '.Trashes', '.TemporaryItems', '.DocumentRevisions-V100', '.DS_Store')


def run(*command, capture=False, check=True):
    print('+', ' '.join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), capture_output=True)
    if not capture:
        print(result.stdout.decode(errors='replace'), end='', flush=True)
    if result.returncode:
        print(result.stderr.decode(errors='replace'), end='', flush=True)
        if check:
            raise SystemExit(f'failed ({result.returncode}): {" ".join(map(str, command))}')
    return result.stdout if capture else result.returncode


def plist(*command):
    # diskutil wants -plist right after its verb (diskutil info -plist DEV); hdiutil takes it anywhere.
    at = 3 if command[:2] == ('diskutil', 'apfs') else 2
    return plistlib.loads(run(*command[:at], '-plist', *command[at:], capture=True))


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--packer', type=Path, required=True, help='drivers/DarwinBoot/Test/packed_ramdisk.py of UniXNU')
    parser.add_argument('--spec', type=Path, default=ROOT / 'data-seed.json')
    parser.add_argument('--out', type=Path, default=ROOT / '.cache/data-seed')
    args = parser.parse_args()
    spec = json.loads(args.spec.read_text())
    loader = importlib.util.spec_from_file_location('packed_ramdisk', args.packer)
    packer = importlib.util.module_from_spec(loader)
    loader.loader.exec_module(packer)

    args.out.mkdir(parents=True, exist_ok=True)
    raw, packed, report = args.out / 'data-macos.raw', args.out / 'data-macos.dbr', args.out / 'data-macos.json'
    raw.unlink(missing_ok=True)
    with open(raw, 'wb') as stream:
        stream.truncate(spec['size_mib'] << 20)

    attached = plist('hdiutil', 'attach', '-nomount', '-imagekey', 'diskimage-class=CRawDiskImage', raw)
    disk = attached['system-entities'][0]['dev-entry']
    try:
        # A container on the whole raw disk (no partition map); a diskutil that refuses that gets one GPT partition.
        if run('diskutil', 'apfs', 'createContainer', disk, check=False):
            run('diskutil', 'partitionDisk', disk, '1', 'GPT', 'free', 'none', '100%')
            store = plist('diskutil', 'list', disk)['AllDisksAndPartitions'][0]['Partitions'][0]['DeviceIdentifier']
            run('diskutil', 'apfs', 'createContainer', store)
        name = disk.replace('/dev/', '')
        listing = plist('diskutil', 'apfs', 'list')
        container_disk = next(c['ContainerReference'] for c in listing['Containers']
                              if any(s['DeviceIdentifier'] == name or s['DeviceIdentifier'].startswith(name + 's') for s in c['PhysicalStores']))
        fs = 'Case-sensitive APFS' if spec['case_sensitive'] else 'APFS'
        role = [] if spec.get('role', 'none') == 'none' else ['-role', spec['role']]
        run('diskutil', 'apfs', 'addVolume', container_disk, fs, spec['volume_name'], *role)
        listing = plist('diskutil', 'apfs', 'list')
        volumes = [v for c in listing['Containers'] if c['ContainerReference'] == container_disk
                   for v in c['Volumes']]
        if len(volumes) != 1:
            raise SystemExit(f'expected one volume in {container_disk}, found {len(volumes)}')
        volume = volumes[0]
        info = plist('diskutil', 'info', volume['DeviceIdentifier'])
        mount = Path(info.get('MountPoint') or '')
        if not mount.is_dir():
            run('diskutil', 'mount', volume['DeviceIdentifier'])
            mount = Path(plist('diskutil', 'info', volume['DeviceIdentifier'])['MountPoint'])
        run('sudo', 'mdutil', '-i', 'off', mount)
        run('sudo', 'diskutil', 'enableOwnership', mount)
        for path, uid, gid, mode in spec['directories']:
            target = mount / path
            run('sudo', 'mkdir', '-p', target)
            run('sudo', 'chown', f'{uid}:{gid}', target)
            run('sudo', 'chmod', mode, target)
        # files: [path, uid, gid, mode, "plist", value] — small preference files the system would otherwise write on its first boot
        # (written here as binary plists; our own content, no Apple files).
        for path, uid, gid, mode, kind, value in spec.get('files', []):
            if kind != 'plist':
                raise SystemExit(f'unsupported file kind {kind!r} for {path}')
            staged = args.out / ('file-' + path.replace('/', '_'))
            staged.write_bytes(plistlib.dumps(value, fmt=plistlib.FMT_BINARY))
            target = mount / path
            run('sudo', 'install', '-o', str(uid), '-g', str(gid), '-m', mode, staged, target)
            staged.unlink()
        for name in MAC_EXTRAS:
            if (mount / name).exists() or (mount / name).is_symlink():
                run('sudo', 'rm', '-rf', mount / name)
        tree = run('sudo', 'ls', '-laR', mount, capture=True).decode()
        run('diskutil', 'unmount', 'force', mount)
        info = plist('diskutil', 'info', volume['DeviceIdentifier'])
    finally:
        run('hdiutil', 'detach', disk)

    packed.unlink(missing_ok=True)
    packer.pack(raw, packed)
    result = {
        'logical_bytes': raw.stat().st_size,
        'volume_name': spec['volume_name'],
        'volume_uuid': info['VolumeUUID'],
        'container_uuid': info.get('APFSContainerUUID') or volume.get('ContainerUUID'),
        'role': volume.get('Roles'),
        'filesystem': info.get('FilesystemName') or info.get('FilesystemUserVisibleName'),
        'encrypted': bool(info.get('Encrypted')),
        'raw_sha256': sha256(raw),
        'packed_sha256': sha256(packed),
        'packed_bytes': packed.stat().st_size,
        'directories': spec['directories'],
        'files': [f[:4] for f in spec.get('files', [])],
        'tree': tree,
        'builder': 'macos-diskutil',
    }
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('directories', 'tree')}, indent=2))
    if os.environ.get('KEEP_RAW') != '1':
        raw.unlink()


if __name__ == '__main__':
    main()
