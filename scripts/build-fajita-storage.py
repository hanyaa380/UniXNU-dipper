#!/usr/bin/env python3
"""Storage inputs of the fajita kernel: the storage ABI audit and the NativeVirtioBlock bindings NativeUfs links against.

Runs as a [kernel] archives builder, after the bootstrap pass left the compile records. The audit is static
(audit-storage-interface.py --static) against the prelinked kernel named by FAJITA_STORAGE_CANDIDATE, an earlier
fajita kernel.macho with its kernel.json prelink report beside it: the bound addresses are those of the original
IOStorageFamily at the board-independent slide, so any prelink of the same kernelcache qualifies. Copies of the
audit and the bindings report go to the logs directory, which the macOS workflow uploads.
"""
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
