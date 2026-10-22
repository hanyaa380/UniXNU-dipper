#!/usr/bin/env python3
"""Build and run the host tests of the fajita touch stack: GENI I2C sequencing, the RMI4 core, the board helpers and the whole
RMI-over-GENI register path, each against register models (no hardware). C sources as C11, tests as C++17, with the address and
undefined-behaviour sanitizers when the compiler has them.

  scripts/check-fajita-touch-host.py [--cc clang] [--cxx clang++] [--out DIR]
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
I2C = ROOT / 'drivers/NativeGeniI2c'
RMI = ROOT / 'drivers/NativeRmi4Touch'
PKG = ROOT / 'package'
TESTS = [
    ('geni-i2c-test', [I2C / 'tests/geni-i2c-test.cpp', I2C / 'tests/FakeGeniI2c.cpp'], ['GeniI2c']),
    ('rmi4-test', [RMI / 'tests/rmi4-test.cpp'], ['Rmi4']),
    ('fajita-touch-board-test', [PKG / 'tests/fajita-touch-board-test.cpp'], ['GeniI2c', 'Rmi4']),
    ('fajita-touch-chain-test', [PKG / 'tests/fajita-touch-chain-test.cpp', I2C / 'tests/FakeGeniI2c.cpp'], ['GeniI2c', 'Rmi4']),
]
SOURCES = {'GeniI2c': I2C / 'GeniI2c.c', 'Rmi4': RMI / 'Rmi4.c'}


def sanitizers(cxx, out):
    probe = out / 'probe.cpp'
    probe.write_text('int main() { return 0; }\n')
    flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    ok = subprocess.run([cxx, *flags, str(probe), '-o', str(out / 'probe')], capture_output=True).returncode == 0
    return flags if ok else []


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cc', default=shutil.which('clang') or 'cc')
    parser.add_argument('--cxx', default=shutil.which('clang++') or 'c++')
    parser.add_argument('--out')
    args = parser.parse_args()
    out = Path(args.out or tempfile.mkdtemp(prefix='fajita-touch-host-'))
    out.mkdir(parents=True, exist_ok=True)
    san = sanitizers(args.cxx, out)
    common = ['-g', '-O1', '-Wall', '-Wextra', '-Werror', *san, '-I', str(I2C), '-I', str(RMI)]
    objects = {}
    for name, source in SOURCES.items():
        objects[name] = out / f'{name}.o'
        subprocess.run([args.cc, '-std=c11', *common, '-c', str(source), '-o', str(objects[name])], check=True)
    for name, sources, libs in TESTS:
        binary = out / name
        subprocess.run([args.cxx, '-std=c++17', *common, *map(str, sources), *(str(objects[lib]) for lib in libs), '-o', str(binary)],
                       check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.returncode:
            sys.stderr.write(result.stderr)
            raise SystemExit(f'FAIL: {name} ({result.returncode})')
    print(f'PASS: fajita touch host tests ({"sanitized" if san else "without sanitizers"})')


if __name__ == '__main__':
    main()
