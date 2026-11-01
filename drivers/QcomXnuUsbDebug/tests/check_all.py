#!/usr/bin/env python3
"""Build and check the complete module, without modifying or booting a consumer."""
import argparse
import hashlib
import json
import importlib.util
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--records', type=Path, required=True)
    parser.add_argument('--target-clang', type=Path, required=True)
    parser.add_argument('--user-clang', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--ar', type=Path, required=True)
    parser.add_argument('--nm', type=Path, required=True)
    parser.add_argument('--soc', default='soc/Sm8550.h')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if ROOT == out or ROOT in out.parents or not any(p in ('tmp', '.cache') for p in out.parts):
        parser.error('output must be a new directory outside module source under tmp/ or .cache/')
    def record(stem):
        matches = list(args.records.resolve().glob('**/' + stem + '.o.json'))
        if len(matches) != 1:
            parser.error('missing/ambiguous compile record: ' + stem)
        return matches[0]
    pe, bsd = record('pe_kprintf'), record('subr_prf')
    spec = importlib.util.spec_from_file_location('qud_patches', ROOT/'scripts/xnu_patches.py')
    patches = importlib.util.module_from_spec(spec); spec.loader.exec_module(patches)
    revision = subprocess.check_output(['git','-C',str(args.xnu.resolve()),'rev-parse','HEAD'],text=True).strip()
    if revision != patches.XNU_PROFILES['darwin22']['xnu_revision']:
        parser.error('unsupported XNU revision')
    def digest_sources():
        return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in ROOT.rglob('*') if p.is_file() and '.git' not in p.parts and p.name != '.DS_Store'}
    before = digest_sources()
    out.mkdir(parents=True, exist_ok=False)
    archive = out / 'kernel' / 'libQcomXnuUsbDebug.a'
    steps = (
        ('models', 'tests/check.py', ['--target-clang', args.target_clang.resolve(), '--output', out/'models']),
        ('logging', 'tests/check_logging.py', ['--xnu', args.xnu.resolve(), '--output', out/'logging']),
        ('kernel', 'scripts/build.py', ['--compile-record', pe, '--userlog-compile-record', bsd,
                                     '--soc', args.soc, '--ar', args.ar.resolve(), '--nm', args.nm.resolve(), '--output', out/'kernel']),
        ('userland', 'scripts/build-userlog.py', ['--clang', args.user_clang.absolute(), '--sdk', args.sdk.resolve(), '--output', out/'userland']),
        ('xnu', 'tests/check_xnu.py', ['--xnu', args.xnu.resolve(), '--records', args.records.resolve(),
                                    '--archive', archive, '--nm', args.nm.resolve(), '--output', out/'xnu']),
    )
    report = dict(passed=False, device_verified=False, kernel_linked=False,
                  full_start_assembly_compiled=False, steps={}, source_sha256=before)
    try:
        for name, script, options in steps:
            command = [sys.executable, '-B', str(ROOT/script), *map(str, options)]
            with (out/(name+'.log')).open('wb') as log:
                status = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
            report['steps'][name] = dict(command=command, returncode=status)
            if status:
                print('Failed '+name+'; see '+str(out/(name+'.log')), file=sys.stderr)
                return 1
        if digest_sources() != before:
            raise RuntimeError('module source changed during checks')
        report['passed'] = True
        report['archive_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
        print('Passed complete module checks:', out/'check-all-report.json')
        return 0
    finally:
        (out/'check-all-report.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    raise SystemExit(main())
