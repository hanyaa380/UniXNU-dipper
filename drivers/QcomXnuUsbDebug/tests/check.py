#!/usr/bin/env python3
"""Sanitized real-source models plus relocated SoC and freestanding builds."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(argv):
    r = subprocess.run([str(a) for a in argv], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(' '.join(map(str, argv)) + '\n' + r.stdout + r.stderr)
    return r.stdout.strip()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path, help='new tmp/ or .cache/ directory')
    p.add_argument('--target-clang', required=True, type=Path)
    a = p.parse_args()
    out = a.output.resolve()
    if not any(part in ('tmp', '.cache') for part in out.parts) or out == ROOT or ROOT in out.parents:
        p.error('output must be outside source under tmp/ or .cache/')
    out.mkdir(parents=True, exist_ok=False)
    target = a.target_clang.resolve()
    nm = target.with_name('llvm-nm')
    host = shutil.which('clang')
    base = ['-D_POSIX_C_SOURCE=200112L', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT),
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    checks = {}
    # Shared controller must compile and run with no QUD_SOC_HEADER at all.
    exe = out / 'generic-dwc3'
    run([host, *base, ROOT / 'tests/test_dwc3.c', ROOT / 'usb/Dwc3.c', '-o', exe])
    checks['generic-dwc3'] = run([exe])
    for triple in ('aarch64-none-elf', 'aarch64-unknown-windows-msvc'):
        obj = out / ('generic-' + triple + '-Dwc3.o')
        run([target, '--target=' + triple, '-ffreestanding', '-fno-builtin',
             '-mgeneral-regs-only', '-mstrict-align', '-Oz', '-Wall', '-Wextra',
             '-Werror', '-I' + str(ROOT), '-c', ROOT / 'usb/Dwc3.c', '-o', obj])
        assert not run([nm, '--undefined-only', obj]), 'shared DWC3 runtime imports'
        checks[obj.name] = 'compile pass without SoC profile or runtime imports'
    profiles = {'sm8550': ROOT / 'soc/Sm8550.h'}
    # Different controller/GCC/SMMU bases, clock offsets, stream ID and IOVA.
    # Uses the same register *layout*, not a claim of another supported SoC.
    profile = (ROOT / 'soc/Sm8550.h').read_text()
    for old, new in [('0x00100000ULL', '0x02100000ULL'),
                     ('0x0a600000ULL', '0x0b600000ULL'),
                     ('0x15000000ULL', '0x16000000ULL'),
                     ('0x40u', '0x123u'), ('0x90000000ULL', '0x82000000ULL'),
                     ('0x390', '0x290')]:
        profile = profile.replace(old, new)
    relocated = out / 'Relocated.h'
    relocated.write_text(profile)
    profiles['relocated'] = relocated
    for name, profile in profiles.items():
        flag = '-DQUD_SOC_HEADER="' + str(profile) + '"'
        for label, units in {
            'domain': ['tests/test_domain.c', 'soc/QcomUsbDmaDomain.c'],
            'buffer': ['tests/test_usb_buffer.c'],
            'control': ['tests/test_usb_control.c'],
            'screenshot': ['tests/test_usb_screenshot.c', 'kernel/QcomUsbScreenshot.c'],
        }.items():
            exe = out / (name + '-' + label)
            run([host, *base, flag, '-pthread', *[ROOT / u for u in units], '-o', exe])
            checks[name + '-' + label] = run([exe])
            if label == 'screenshot':
                checks[name + '-fixed-framebuffer'] = run([exe, 'fixed'])
        # The inherited-state inspector must read exactly the selected layout.
        fixture = ROOT / 'tests/test_inspect.c'

        exe = out / (name + '-inspect')
        run([host, *base, flag, fixture, ROOT / 'soc/QcomUsbQualify.c', '-o', exe])
        run([exe])
        checks[name + '-inspect'] = 'pass'
        for triple in ('aarch64-none-elf', 'aarch64-unknown-windows-msvc'):
            for unit in ('soc/QcomUsbQualify.c', 'soc/QcomUsbDmaDomain.c'):
                obj = out / (name + '-' + triple + '-' + Path(unit).stem + '.o')
                run([target, '--target=' + triple, '-ffreestanding', '-fno-builtin',
                     '-mgeneral-regs-only', '-mstrict-align', '-Oz', '-Wall', '-Wextra',
                     '-Werror', '-I' + str(ROOT), flag, '-c', ROOT / unit, '-o', obj])
                imports = {line.split()[-1] for line in run([nm, '--undefined-only', obj]).splitlines() if line.strip()}
                # Struct copies may lower to memcpy even with -fno-builtin.
                allowed = {'memcpy', 'memset'} if 'DmaDomain' in unit else set()
                assert imports <= allowed, (unit, imports)
                checks[obj.name] = 'compile pass; runtime imports=' + ','.join(sorted(imports))
    # SDM845 uses the full 0x80000 apps_smmu window for 64 global pages plus
    # 64 context banks, and the live USB stream is SID 0x740.
    exe = out / 'sm845-live-geometry'
    run([host, *base, '-DQUD_SOC_HEADER="' + str(ROOT / 'soc/Sm845.h') + '"',
         ROOT / 'tests/test_sm845.c', ROOT / 'soc/QcomUsbQualify.c',
         ROOT / 'soc/QcomUsbDmaDomain.c', '-o', exe])
    checks['sm845-live-geometry'] = run([exe])
    for triple in ('aarch64-none-elf', 'aarch64-unknown-windows-msvc'):
        obj = out / ('sm845-' + triple + '-native.o')
        run([target, '--target=' + triple, '-ffreestanding', '-fno-builtin',
             '-mgeneral-regs-only', '-mstrict-align', '-Oz', '-Wall', '-Wextra',
             '-Werror', '-I' + str(ROOT), '-DQUD_SOC_HEADER="' + str(ROOT / 'soc/Sm845.h') + '"',
             '-c', ROOT / 'soc/QcomUsbDmaDomain.c', '-o', obj])
        checks[obj.name] = 'SDM845 profile cross-compile pass'
    # Historical identity-qualifier fixtures pin SM8550 SID/register offsets.
    # The native backend still selects only the private translated domain.
    exe = out / 'sm8550-qualify'
    run([host, *base, '-DQUD_SOC_HEADER="' + str(ROOT / 'soc/Sm8550.h') + '"',
         ROOT / 'tests/test_qualify.c', ROOT / 'soc/QcomUsbQualify.c', '-o', exe])
    checks['sm8550-qualify'] = run([exe])
    # Missing profile and overlapping windows must fail at compile time.
    probe = out / 'probe.c'
    probe.write_text('#include "include/QcomUsbSoc.h"\n')
    for name, args in [('missing-profile', []), ('overlap', ['-DQUD_SOC_HEADER="' + str(out / 'Overlap.h') + '"'])]:
        if name == 'overlap':
            (out / 'Overlap.h').write_text(profile.read_text().replace('0x0b600000ULL', '0x02100000ULL'))
        r = subprocess.run([host, '-std=c11', '-I' + str(ROOT), *args, '-fsyntax-only', str(probe)], capture_output=True)
        assert r.returncode, name
        checks[name] = 'correctly refused'
    # Compile the actual native command handler with a modelled atomic queue.
    native = (ROOT / 'kernel/QcomXnuUsbDebug.c').read_text()
    begin = native.index('static size_t control_reply_reboot(')
    end = native.index('static void control_service(', begin)
    (out / 'control-handler.inc').write_text(native[begin:end])
    exe = out / 'screenshot-native-control'
    run([host, *base, '-I' + str(out), ROOT / 'tests/test_usb_screenshot_control.c',
         ROOT / 'kernel/QcomUsbScreenshot.c', '-o', exe])
    wire = run([exe])
    (out / 'native-screenshot.wire').write_text(wire + '\n')
    checks['native-screenshot-control'] = 'backpressure, stale sequence, capture failure, reboot gate passed'
    checks['screenshot-host-cli'] = run([sys.executable, '-B', ROOT / 'tests/test_screenshot.py',
        '--wire', out / 'native-screenshot.wire', '--output', out])
    report = {'hardware_verified': False, 'checks': checks,
              'source_sha256': {str(f.relative_to(ROOT)): hashlib.sha256(f.read_bytes()).hexdigest()
                                for f in ROOT.rglob('*') if f.is_file() and f.suffix in ('.c', '.h')}}
    (out / 'check-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Passed', len(checks), 'checks:', out / 'check-report.json')


if __name__ == '__main__':
    main()
