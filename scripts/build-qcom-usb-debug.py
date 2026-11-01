#!/usr/bin/env python3
"""Build QcomXnuUsbDebug for dipper/SDM845 with the kernel's compile record."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / 'drivers/QcomXnuUsbDebug'
OUT = ROOT / 'build/qcom-usb-debug'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json'
PROFILE = 'soc/Sm845.h'

if not RECORD.is_file():
    raise SystemExit(f'XNU compile record not found: {RECORD}')
if not (MODULE / PROFILE).is_file():
    raise SystemExit(f'SDM845 profile not found: {MODULE / PROFILE}')

OUT.mkdir(parents=True, exist_ok=True)
work_root = ROOT / 'tmp'
work_root.mkdir(parents=True, exist_ok=True)
work = Path(tempfile.mkdtemp(prefix='qcom-usb-debug-', dir=work_root))

command = [
    'python3', '-B', str(MODULE / 'scripts/build.py'),
    '--compile-record', str(RECORD),
    '--soc', PROFILE,
    '--output', str(work / 'module'),
]
result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
(OUT / 'compile.log').write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit((result.stdout + result.stderr)[-5000:])

source_archive = work / 'module/libQcomXnuUsbDebug.a'
archive = OUT / 'libQcomXnuUsbDebug.a'
shutil.copy2(source_archive, archive)
report = json.loads((work / 'module/build-report.json').read_text())
report.update({
    'consumer': 'dipper',
    'consumer_profile': PROFILE,
    'archive': str(archive.relative_to(ROOT)),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'hardware_execution': False,
})
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'QcomXnuUsbDebug: {PROFILE} {report["archive_sha256"]}')
