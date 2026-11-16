#!/usr/bin/env python3
"""Verify generic patches on immutable XNU source copies; never edit --xnu.

Compile enabled/disabled output and worker units with their actual records.
Exercise patch lifecycle/conflict handling in a tiny independent Git fixture.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location('qud_' + name, ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(argv, cwd=None):
    return subprocess.check_output([str(a) for a in argv], cwd=cwd, stderr=subprocess.STDOUT)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--xnu', type=Path, required=True)
    p.add_argument('--records', type=Path, required=True, help='target architecture build directory')
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--nm', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    out = a.output.resolve()
    if ROOT == out or ROOT in out.parents or not any(s in ('tmp', '.cache') for s in out.parts):
        p.error('new output must be outside module source under tmp/ or .cache/')
    patches = load('xnu_patches')
    build = load('build')
    config, series = patches.patch_set('darwin22')
    revision = run(['git', '-C', a.xnu.resolve(), 'rev-parse', 'HEAD']).decode().strip()
    if revision != config['xnu_revision']:
        p.error('unsupported XNU revision')
    out.mkdir(parents=True, exist_ok=False)
    tree = out / 'source'
    before = {}
    for name in config['translation_units']:
        content = run(['git', '-C', a.xnu.resolve(), 'show', revision + ':' + name])
        target = tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        before[name] = hashlib.sha256(content).hexdigest()
    # Independent fixture's commit has different identity; test lifecycle by
    # temporarily binding only the in-memory config, not the shipped pin.
    run(['git', 'init', '-q'], tree)
    run(['git', 'add', '.'], tree)
    run(['git', '-c', 'user.name=QUD check', '-c', 'user.email=qud@example.invalid',
         '-c', 'commit.gpgsign=false', 'commit', '-qm', 'immutable source fixture'], tree)
    fixture_revision = run(['git', 'rev-parse', 'HEAD'], tree).decode().strip()
    fixture_config = dict(config, xnu_revision=fixture_revision)
    patches.patch_set = lambda profile: (fixture_config, series)
    checks = {}
    def expect_error(label, action):
        try:
            patches.operate(tree, 'darwin22', action)
        except ValueError:
            checks[label] = 'correctly refused'
        else:
            raise RuntimeError(label + ' unexpectedly accepted')
    expect_error('reverse-unapplied', 'reverse')
    checks['check-clean'] = patches.operate(tree, 'darwin22', 'check')['state_before']
    patches.operate(tree, 'darwin22', 'apply')
    expect_error('double-apply', 'apply')
    checks['status-applied'] = patches.operate(tree, 'darwin22', 'status')['state_before']
    # Audit all generic hook references resolve to the actual new archive.
    nm = a.nm.resolve()
    def symbols(args):
        return {s.split()[-1] for s in run([nm, *args]).decode().splitlines()
                if s.strip() and not s.endswith(':')}
    exports = symbols(['--defined-only', '--extern-only', a.archive.resolve()])
    for name in config['translation_units']:
        stem = Path(name).stem
        candidates = list(a.records.resolve().glob('**/' + stem + '.o.json'))
        if name.endswith('.s') and not candidates:
            candidates = list(a.records.resolve().glob('**/start.*.json'))
        if len(candidates) != 1:
            raise ValueError('missing/ambiguous compile record for ' + name)
        record = json.loads(candidates[0].read_text().rstrip().rstrip(','))
        flags, cwd = build.compile_flags(record)
        if name.endswith('.s'):
            flags = [f for f in flags if f not in ('-xc', '-std=gnu11')] + ['-x', 'assembler-with-cpp']
        compile_source = tree / name
        if name.endswith('.s'):
            # Public XNU omits Apple CPU tunables and requires consumer platform
            # patches to assemble the whole entry. Compile only this module's
            # exact checkpoint; do not silently alter unrelated platform code.
            text = compile_source.read_text()
            begin = text.index('#if XNU_QCOM_USB_DEBUG', text.index('LEXT(start_first_cpu)'))
            end = text.index('#endif', begin) + len('#endif')
            checkpoint = text[begin:end].replace('EXT(qcom_early_entry_ticks)', '_qcom_early_entry_ticks')
            compile_source = out / 'cold-entry.s'
            compile_source.write_text('.text\n.globl _qud_cold_entry_check\n_qud_cold_entry_check:\n' + checkpoint + '\nret\n')
        # Prove generic integration works without any legacy device macro.
        for enabled in (0, 1):
            obj = out / (stem + '-' + str(enabled) + '.o')
            args = flags + ['-UXNU_SOCRATES_USB_LOG', '-UXNU_SOCRATES_FB_CAPTURE',
                            '-UXNU_QCOM_USB_DEBUG', '-DXNU_QCOM_USB_DEBUG=' + str(enabled),
                            '-I' + str(ROOT / 'include'), '-Wno-reserved-identifier',
                            '-Wno-pre-c11-compat', '-c', str(compile_source), '-o', str(obj)]
            with (out / (obj.stem + '.log')).open('wb') as log:
                subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
            imports = symbols(['--undefined-only', obj])
            hooks = {s for s in imports if s.startswith(('_qcom_usb_debug_', '_qcom_early_'))}
            if enabled and name != 'osfmk/console/serial_general.c' and not hooks:
                raise RuntimeError('enabled unit has no generic hooks: ' + name)
            if not enabled and hooks:
                raise RuntimeError('disabled unit imports USB hooks: ' + name)
            if not hooks <= exports | {'_qcom_early_log_stage', '_qcom_early_entry_ticks'} or any('socrates' in s.lower() for s in imports):
                raise RuntimeError('unresolved or device-specific imports: ' + name)
            checks[name + (':entry-fragment' if name.endswith('.s') else '') + ':enabled=' + str(enabled)] = sorted(hooks)
    pe_symbols = run([nm, '--format=posix', out/'pe_kprintf-1.o']).decode().splitlines()
    if not any(line.split()[:2] == ['_qcom_early_entry_ticks', 'D'] for line in pe_symbols):
        raise RuntimeError('cold entry timestamp must be initialized data, not BSS')
    checks['cold-entry-initialized-data'] = 'D symbol, not zero-initialized BSS'
    if not {'_qcom_early_log_stage', '_qcom_early_entry_ticks'} <= symbols(['--defined-only', out/'pe_kprintf-1.o']):
        raise RuntimeError('missing early stage implementation or timestamp definition')
    checks['cold-entry-stage-definitions'] = 'both internal early symbols resolve to PE object'
    patches.operate(tree, 'darwin22', 'reverse')
    for name, digest in before.items():
        if hashlib.sha256((tree / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('reverse did not restore source: ' + name)
    checks['roundtrip-source-hashes'] = 'exact match'
    # Corrupt one insertion anchor; all-patches precheck must leave other files alone.
    target = tree / 'pexpert/arm/pe_kprintf.c'
    target.write_text(target.read_text().replace('static void serial_putc_crlf(char c);', '/* changed anchor */'))
    snapshot = {n: (tree / n).read_bytes() for n in before}
    expect_error('conflicting-apply', 'apply')
    assert all((tree / n).read_bytes() == content for n, content in snapshot.items())
    checks['conflict-preserves-all-files'] = 'exact match'
    # The shipped version pin must also refuse the fixture's unrelated commit.
    patches.patch_set = lambda profile: (config, series)
    expect_error('unsupported-revision', 'check')
    report = {'xnu_revision': revision, 'hardware_verified': False, 'kernel_linked': False, 'full_start_assembly_compiled': False,
              'archive_sha256': hashlib.sha256(a.archive.read_bytes()).hexdigest(),
              'patch_sha256': {s.name: hashlib.sha256(s.read_bytes()).hexdigest() for s in series},
              'checks': checks}
    (out / 'xnu-check-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Passed', len(checks), 'XNU integration checks:', out / 'xnu-check-report.json')


if __name__ == '__main__':
    main()
