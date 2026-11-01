#!/usr/bin/env python3
"""Build the standalone archive with the selected XNU's real compile record.

Never patches/builds/boots the consumer kernel. Writes only a new output dir.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
UNITS = ('kernel/QcomXnuUsbDebug.c', 'kernel/QcomUsbScreenshot.c',
         'usb/Dwc3.c', 'soc/QcomUsbQualify.c',
         'soc/QcomUsbDmaDomain.c')


def compile_flags(record):
    args = record['arguments']
    if not isinstance(args, list) or not args or not all(isinstance(a, str) for a in args):
        raise ValueError('record must contain an arguments array')
    cwd = Path(record['directory']).resolve()
    source = Path(record['file'])
    source = (source if source.is_absolute() else cwd / source).resolve()
    if not cwd.is_dir() or not source.is_file():
        raise ValueError('reference directory/source unavailable')
    # Strip source, dependency and output operands; never execute a shell.
    consumes = {'-o', '-MF', '-MT', '-MQ', '-MJ', '-dependency-file',
                '--serialize-diagnostics', '-serialize-diagnostics'}
    discard = {'-c', '-fsyntax-only', '-M', '-MM', '-MD', '-MMD', '-MP', '-MG',
               '-save-temps', '-save-temps=obj'}
    flags = [args[0]]
    i = 1
    while i < len(args):
        a = args[i]
        if a in consumes:
            if i + 1 >= len(args):
                raise ValueError('truncated output/dependency option')
            i += 2
            continue
        if a in discard or a == record['file'] or a == str(source):
            i += 1
            continue
        if not a.startswith('-') and (cwd / a).resolve() == source:
            i += 1
            continue
        if any(a.startswith(p) and len(a) > len(p) for p in ('-o', '-MF', '-MT', '-MQ', '-MJ')):
            i += 1
            continue
        flags.append(a)
        i += 1
    return flags, cwd


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compile-record', type=Path, required=True,
                   help='pe_kprintf.o.json for selected ARM64 XNU')
    p.add_argument('--userlog-compile-record', type=Path, help='optional BSD subr_prf.o.json; enables root-only userlog sysctl')
    p.add_argument('--soc', default='soc/Sm8550.h', help='module-relative profile')
    p.add_argument('--output', type=Path, required=True, help='new directory under tmp/ or .cache/')
    p.add_argument('--ar', help='llvm-ar executable')
    p.add_argument('--nm', help='llvm-nm executable for import/export audit')
    a = p.parse_args()
    profile = (ROOT / a.soc).resolve()
    if ROOT not in profile.parents or not profile.is_file():
        p.error('SoC profile must exist within the module')
    out = a.output.resolve()
    if not any(part in ('tmp', '.cache') for part in out.parts) or out == ROOT or ROOT in out.parents:
        p.error('output must be outside source and under tmp/ or .cache/')
    record_bytes = a.compile_record.read_bytes()
    record = json.loads(record_bytes.decode().rstrip().rstrip(','))
    flags, cwd = compile_flags(record)
    if not any('arm64' in f or 'aarch64' in f for f in flags):
        p.error('require ARM64 reference flags')
    ar = a.ar or shutil.which('llvm-ar') or str(Path(flags[0]).parent / 'llvm-ar')
    nm = a.nm or shutil.which('llvm-nm') or str(Path(ar).with_name('llvm-nm'))
    # Never overwrite an archive/evidence directory from a previous run.
    out.mkdir(parents=True, exist_ok=False)
    defines = ['-UXNU_SOCRATES_USB_LOG', '-UXNU_SOCRATES_FB_CAPTURE', '-I' + str(ROOT), '-I' + str(ROOT / 'kernel'), '-I' + str(ROOT / 'include'),
               '-DQUD_SOC_HEADER="' + profile.relative_to(ROOT).as_posix() + '"',
               '-Wno-reserved-identifier', '-Wno-pre-c11-compat']
    report = {'hardware_verified': False, 'kernel_linked': False,
              'compile_record_sha256': hashlib.sha256(record_bytes).hexdigest(),
              'soc': a.soc, 'commands': [], 'source_sha256': {},
              'userlog_enabled': bool(a.userlog_compile_record),
              'userlog_compile_record_sha256': (hashlib.sha256(a.userlog_compile_record.read_bytes()).hexdigest()
                                                 if a.userlog_compile_record else None)}
    inputs = [p for p in ROOT.rglob('*') if p.is_file() and p.suffix in ('.c', '.h')]
    before = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    objects = []
    try:
        for unit in UNITS + (('kernel/QcomUsbUserLog.c',) if a.userlog_compile_record else ()):
            obj = out / (Path(unit).stem + '.o')
            unit_flags, unit_cwd = (compile_flags(json.loads(a.userlog_compile_record.read_text().rstrip().rstrip(',')))
                                    if unit.endswith('QcomUsbUserLog.c') else (flags, cwd))
            command = unit_flags + defines + ['-Wno-switch-default', '-c', str(ROOT / unit), '-o', str(obj)]
            report['commands'].append(command)
            with (out / (obj.stem + '.log')).open('wb') as log:
                subprocess.run(command, cwd=unit_cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        for name, digest in before.items():
            if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
                raise RuntimeError('source changed during build: ' + name)
        command = [ar, 'rcs', str(out / 'libQcomXnuUsbDebug.a'), *objects]
        subprocess.run(command, check=True)
        report['commands'].append(command)
        archive = out / 'libQcomXnuUsbDebug.a'
        def symbols(mode):
            lines = subprocess.check_output([nm, mode, '--extern-only', str(archive)], text=True).splitlines()
            return {line.split()[-1] for line in lines if line.strip() and not line.endswith(':')}
        defined = symbols('--defined-only')
        external = symbols('--undefined-only') - defined
        allowed = {'_PE_halt_restart', '_PE_parse_boot_argn', '_cpu_number', '_master_cpu',
                   '_ml_io_map', '_ml_static_vtop', '___stack_chk_guard', '___stack_chk_fail',
                   '_boot_progress_stage', '_bzero', '_memcpy', '_memset', '_panic',
                   '_proc_suser', '_current_proc', '_snprintf',
                   '_sysctl_register_oid_early', '_sysctl__debug_children'}
        if external - allowed:
            raise RuntimeError('unexpected XNU imports: ' + str(sorted(external - allowed)))
        required = {'_qcom_usb_debug_' + s for s in
                    ('init', 'is_active', 'write', 'putc', 'poll', 'panic_prepare', 'panic_drain', 'stop', 'get_status',
                     'control_start', 'control_take_reboot', 'control_arm_reboot',
                     'control_reboot_failed', 'wait_drained', 'enqueue_putc', 'try_record',
                     'worker_ready', 'worker_stopped', 'register_screenshot', 'register_framebuffer')}
        if a.userlog_compile_record:
            required |= {'_qcom_usb_userlog_submit', '_sysctl__debug_qcom_usb_userlog'}
        if not required <= defined:
            raise RuntimeError('missing public API exports')
        # The native adapter must never select the historical identity path.
        native_imports = subprocess.check_output([nm, '--undefined-only', str(out / 'QcomXnuUsbDebug.o')], text=True)
        if any(s in native_imports for s in ('QcomUsbEnterIdentityDma', 'QcomUsbRestoreDma')):
            raise RuntimeError('identity DMA fallback in native backend')
        report['external_imports'] = sorted(external)
        report['public_exports'] = sorted(required)
        report['source_sha256'] = before
        report['archive_sha256'] = hashlib.sha256((out / 'libQcomXnuUsbDebug.a').read_bytes()).hexdigest()
        report['successful_archive'] = True
    finally:
        (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'libQcomXnuUsbDebug.a')


if __name__ == '__main__':
    main()
