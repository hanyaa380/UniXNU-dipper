#!/usr/bin/env python3
"""Build a hardware-only DarwinBoot handoff fixture for dipper.

The fixture is a synthetic ARM64 Mach-O with a minimal Apple-format ADT. It
contains no Apple code. DarwinBoot validates it, exits boot services and jumps
to it at EL1 with the MMU off. The fixture fills the framebuffer green when the
boot_args ABI, MMU state and GOP geometry are usable, red otherwise.
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = 0xFFFF_FFF0_0700_4000
ENTRY = BASE + 0x1000


def run(command):
    subprocess.run([str(value) for value in command], check=True)


def adt_node(name, properties=(), children=()):
    values = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(values), len(children))
    for key, value in values:
        result += struct.pack('<32sI', key.encode(), len(value))
        result += value + bytes(-len(value) % 4)
    return result + b''.join(children)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    tools = {name: shutil.which(name) for name in ('clang', 'ld.lld', 'llvm-objcopy')}
    missing = [name for name, path in tools.items() if path is None]
    if missing:
        parser.error(f'missing tools: {", ".join(missing)}')

    obj = output / 'kernel.o'
    elf = output / 'kernel.elf'
    code_path = output / 'kernel-code.bin'
    run([tools['clang'], '--target=aarch64-none-elf', '-c', ROOT / 'darwinboot-fixture/Kernel.S', '-o', obj])
    run([tools['ld.lld'], f'-Ttext={ENTRY:#x}', '-e', '_start', obj, '-o', elf])
    run([tools['llvm-objcopy'], '-O', 'binary', elf, code_path])
    code = code_path.read_bytes()
    if len(code) > 0x1000:
        raise SystemExit(f'fixture code is {len(code)} bytes; the synthetic text segment allows 4096')

    def segment(name, address, size, offset, length, protection):
        return struct.pack('<II16sQQQQiiII', 0x19, 72, name, address, size, offset, length,
                           protection, protection, 0, 0)

    state = bytearray(272)
    struct.pack_into('<Q', state, 256, ENTRY)
    commands = (
        segment(b'__TEXT', BASE, 0x2000, 0, 0x2000, 5)
        + segment(b'__DATA', BASE + 0x4000, 0x1000, 0, 0, 3)
        + struct.pack('<IIII', 5, 288, 6, 68)
        + state
    )
    image = bytearray(0x2000)
    image[:32] = struct.pack('<8I', 0xFEEDFACF, 0x0100000C, 0, 2, 3, len(commands), 0, 0)
    image[32:32 + len(commands)] = commands
    image[0x1000:0x1000 + len(code)] = code

    kernel = output / 'kernel.macho'
    kernel.write_bytes(image)
    # No ramdisk, trust cache or GPU addon is supplied, so only the names needed
    # to prove a complete ADT are present.
    kernel.with_suffix('.macho.adt').write_bytes(
        adt_node('device-tree', children=[adt_node('chosen', children=[adt_node('memory-map')])])
    )
    print(f'DarwinBoot fixture: {kernel} ({len(image)} bytes), entry {ENTRY:#x}')


if __name__ == '__main__':
    main()
