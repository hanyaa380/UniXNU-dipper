#!/usr/bin/env python3
"""Storage inputs of the fajita kernel: the storage ABI audit and the NativeVirtioBlock bindings NativeUfs links against.

Runs as a [kernel] archives builder, after the bootstrap pass left the compile records. The audit is static
(audit-storage-interface.py --static) against the prelinked kernel named by FAJITA_STORAGE_CANDIDATE, an earlier
fajita kernel.macho with its kernel.json prelink report beside it: the bound addresses are those of the original
IOStorageFamily at the board-independent slide, so any prelink of the same kernelcache qualifies. Copies of the
audit and the bindings report go to the logs directory, which the macOS workflow uploads.

FAJITA_STORAGE_AUDIT may name an interface-audit.json already made for the same candidate (for example by the
macOS workflow): it is used after its candidate hash is checked, instead of compiling the reference object again.

It also stages the inputs of build-sheng-platform-power.py, which runs after it: the same candidate under the name
that script reads, and the decompressed original kernelcache (fetch it first with "unixnu ipsw fetch KernelCache").
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LOGS = ROOT.parent / 'logs'


def run(*command):
    print('+', ' '.join(str(part) for part in command), flush=True)
    subprocess.run(['python3', *command], cwd=ROOT, check=True)


def main():
    candidate = os.environ.get('FAJITA_STORAGE_CANDIDATE')
    if not candidate or not Path(candidate).is_file() or not Path(candidate).with_suffix('.json').is_file():
        raise SystemExit('FAJITA_STORAGE_CANDIDATE must name a prelinked fajita kernel.macho with its kernel.json beside it')
    prelink = ROOT / 'build/native-prelink'
    prelink.mkdir(parents=True, exist_ok=True)
    for suffix in ('.macho', '.json'):
        shutil.copyfile(Path(candidate).with_suffix(suffix), prelink / f'native-owner-ufs-rootauth3-candidate{suffix}')
    original = ROOT.parents[1] / 'ipsw/21H461/kernelcache.decompressed'
    if original.is_file():
        staged = ROOT / '.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed'
        staged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, staged)
    audit = os.environ.get('FAJITA_STORAGE_AUDIT')
    if audit:
        recorded = json.loads(Path(audit).read_text())['candidate_sha256']
        if recorded != hashlib.sha256(Path(candidate).read_bytes()).hexdigest():
            raise SystemExit(f'{audit} was made for candidate {recorded}, not {candidate}')
        (ROOT / 'build/storage-abi').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(audit, ROOT / 'build/storage-abi/interface-audit.json')
        print(f'storage audit: {audit} (candidate {recorded[:16]})', flush=True)
    else:
        run(ROOT / 'scripts/audit-storage-interface.py', '--static', '--candidate', candidate)
    run(ROOT / 'scripts/build-arm-driver.py', 'NativeVirtioBlock', '--verified-root', '--verified-root-bundle')
    run(ROOT / 'scripts/build-native-virtio-bindings.py', '--component', 'NativeVirtioBlock',
        '--directory', 'build/native-virtio-block-verified-bundle')
    LOGS.mkdir(parents=True, exist_ok=True)
    for source in (ROOT / 'build/storage-abi/interface-audit.json',
                   ROOT / 'build/native-virtio-block-verified-bundle/bindings.json'):
        if source.is_file():
            shutil.copyfile(source, LOGS / f'storage-{source.name}')


if __name__ == '__main__':
    main()
