#!/usr/bin/env python3
"""Build and 4-KiB-sign the owner oslogprobe, then emit its trust-cache module."""
import argparse
import hashlib
import json
import plistlib
import re
import struct
import subprocess
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)

    sdk = Path(subprocess.check_output(
        ['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip())
    source = ROOT / 'package/oslog-tools/oslogprobe.c'
    binary = out / 'oslogprobe'
    entitlements = out / 'oslogprobe-entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({
        'com.apple.private.security.no-container': True,
        'com.apple.private.logging.admin': True,
        'com.apple.private.logging.diagnostic': True,
        'com.apple.private.logging.stream': True,
        'platform-application': True,
    }))

    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
         '-O2', '-Wall', '-Wextra', '-Werror', '-fstack-protector-strong',
         source, '-o', binary])
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
         '--identifier', 'org.unixnu.oslogprobe', '--entitlements',
         entitlements, binary])
    run(['codesign', '--verify', '--strict', binary])
    detail = subprocess.run(['codesign', '--display', '--verbose=4', binary],
                            capture_output=True, text=True, check=True).stderr
    match = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    if not match or not re.search(r'^Page size=4096$', detail, re.M):
        raise SystemExit('oslogprobe must have a 4-KiB-page ad-hoc signature')
    cdhash = bytes.fromhex(match.group(1))

    identifier = uuid.uuid5(uuid.NAMESPACE_URL,
                            'org.unixnu.oslogprobe:' + cdhash.hex()).bytes
    trust_module = (out / 'oslogprobe.trustmodule')
    trust_module.write_bytes(
        struct.pack('<I16sI', 2, identifier, 1) + cdhash + bytes([2, 0, 0, 0]))
    report = {
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'cdhash': cdhash.hex(),
        'trust_module_sha256': hashlib.sha256(trust_module.read_bytes()).hexdigest(),
        'entitlements': plistlib.loads(entitlements.read_bytes()),
        'purpose': 'owner-built live OSLog probe; trust only through an owner trust module',
    }
    (out / 'oslogprobe.json').write_text(
        json.dumps(report, indent=2) + '\n')
    print('PASS:', cdhash.hex())


if __name__ == '__main__':
    main()
