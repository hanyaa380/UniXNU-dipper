#!/usr/bin/env python3
"""Build the fajita Data volume seed (/private/var) on macOS from data-seed.json, packed to DBRDISK1.

  make-data-seed-macos.py --packer PATH/packed_ramdisk.py [--spec data-seed.json] [--out DIR]

A raw image of size_mib MiB is attached without mounting; diskutil makes an APFS container on it and adds one
case-sensitive, unencrypted volume with the Data role. The volume is mounted with ownership enabled, the directories
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


def run(*command, capture=False):
    print('+', ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, capture_output=capture).stdout


def plist(*command):
    return plistlib.loads(run(*command, '-plist', capture=True))


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
        container = plist('diskutil', 'apfs', 'createContainer', disk)
        container_disk = container.get('ContainerDevice') or container.get('Container')
        if not container_disk:  # older diskutil: find the synthesized container from the APFS list
            listing = plist('diskutil', 'apfs', 'list')
            container_disk = next(c['ContainerReference'] for c in listing['Containers']
                                  if any(s['DeviceIdentifier'] in disk for s in c['PhysicalStores']))
        fs = 'Case-sensitive APFS' if spec['case_sensitive'] else 'APFS'
        run('diskutil', 'apfs', 'addVolume', container_disk, fs, spec['volume_name'], '-role', 'D')
        listing = plist('diskutil', 'apfs', 'list')
        volumes = [v for c in listing['Containers'] if c['ContainerReference'] == container_disk.replace('/dev/', '')
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
        'tree': tree,
        'builder': 'macos-diskutil',
    }
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('directories', 'tree')}, indent=2))
    if os.environ.get('KEEP_RAW') != '1':
        raw.unlink()


if __name__ == '__main__':
    main()
