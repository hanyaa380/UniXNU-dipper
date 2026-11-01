#!/usr/bin/env python3
"""Check/apply/reverse the complete versioned patch set in an explicit XNU tree.

No reset, stash, clean, build or device commands. git apply is atomic by default;
--reject and --3way are deliberately not used. Existing edits are never erased.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# OS compatibility lives with the tool that enforces it. Patch filenames
# define order: 0001-*.patch, 0002-*.patch, and so on.
XNU_PROFILES = {
    'darwin22': {
        'xnu_version': '8796.141.3',
        'xnu_revision': '1b191cb58250d0705d8a51287127505aa4bc0789',
        'translation_units': (
            'pexpert/arm/pe_kprintf.c',
            'osfmk/kern/debug.c',
            'osfmk/kern/printf.c',
            'osfmk/console/serial_console.c',
            'osfmk/console/serial_general.c',
            'bsd/kern/bsd_init.c',
            'bsd/kern/subr_prf.c',
            'osfmk/arm/arm_init.c',
            'osfmk/arm64/start.s',
        ),
    },
}


def command(argv, cwd):
    return subprocess.run([str(v) for v in argv], cwd=cwd, capture_output=True, text=True)


def patch_set(profile):
    if profile not in XNU_PROFILES:
        raise ValueError('unsupported XNU profile: ' + profile)
    directory = ROOT / 'patches' / profile
    config = XNU_PROFILES[profile]
    series = sorted(p.name for p in directory.glob('*.patch'))
    if not series:
        raise ValueError('no patches for XNU profile: ' + profile)
    patches = []
    for index, name in enumerate(series, 1):
        if not name.startswith(f'{index:04d}-'):
            raise ValueError('patch filenames must have consecutive four-digit numbers')
        path = (directory / name).resolve()
        if path.parent != directory.resolve() or path.suffix != '.patch' or not path.is_file():
            raise ValueError('invalid patch path')
        patches.append(path)
    return config, patches


def operate(tree, profile, action):
    tree = tree.resolve()
    config, patches = patch_set(profile)
    top = command(['git', 'rev-parse', '--show-toplevel'], tree)
    if top.returncode or Path(top.stdout.strip()).resolve() != tree:
        raise ValueError('--xnu must be the root of an explicit Git checkout/worktree')
    revision = command(['git', 'rev-parse', 'HEAD'], tree)
    if revision.returncode or revision.stdout.strip() != config['xnu_revision']:
        raise ValueError('unsupported XNU revision; expected ' + config['xnu_revision'])
    paths = [str(p) for p in patches]
    forward = command(['git', 'apply', '--check', *paths], tree)
    reverse = command(['git', 'apply', '--reverse', '--check', *reversed(paths)], tree)
    state = 'ready' if forward.returncode == 0 else 'applied' if reverse.returncode == 0 else 'conflict-or-mixed'
    report = {'profile': profile, 'xnu': str(tree), 'xnu_revision': revision.stdout.strip(),
              'action': action, 'state_before': state,
              'patch_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in patches}}
    if action == 'check':
        if state != 'ready':
            raise ValueError('patch set is not ready: ' + state + '\n' + forward.stderr)
    elif action == 'apply':
        if state != 'ready':
            raise ValueError('refusing apply: ' + state + '\n' + forward.stderr)
        result = command(['git', 'apply', *paths], tree)
        if result.returncode:
            raise ValueError(result.stderr)
        report['state_after'] = 'applied'
    elif action == 'reverse':
        if state != 'applied':
            raise ValueError('refusing reverse: ' + state + '\n' + reverse.stderr)
        result = command(['git', 'apply', '--reverse', *reversed(paths)], tree)
        if result.returncode:
            raise ValueError(result.stderr)
        report['state_after'] = 'ready'
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('status', 'check', 'apply', 'reverse'))
    p.add_argument('--xnu', required=True, type=Path)
    p.add_argument('--profile', choices=tuple(XNU_PROFILES), default='darwin22')
    a = p.parse_args()
    try:
        result = operate(a.xnu, a.profile, a.action)
    except (ValueError, OSError) as exc:
        p.exit(1, str(exc) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
