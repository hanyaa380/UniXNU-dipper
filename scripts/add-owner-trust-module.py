#!/usr/bin/env python3
"""Prepend one 4-KiB trust module to a DarwinBoot trust-cache bundle."""
import argparse
import struct
from pathlib import Path


def modules(bundle):
    count = struct.unpack_from('<I', bundle)[0]
    if not 1 <= count <= 128:
        raise SystemExit('invalid trust bundle module count')
    starts = struct.unpack_from(f'<{count}I', bundle, 4)
    return [bundle[start:(starts[i + 1] if i + 1 < count else len(bundle))]
            for i, start in enumerate(starts)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('owner', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    base = args.base.read_bytes()
    owner = args.owner.read_bytes()
    if len(owner) < 24 or struct.unpack_from('<I', owner)[0] != 2:
        raise SystemExit('owner module is malformed')
    pieces = [owner, *modules(base)]
    count = len(pieces)
    cursor, starts = 4 + 4 * count, []
    for piece in pieces:
        starts.append(cursor)
        cursor += len(piece)
    result = struct.pack(f'<{count + 1}I', count, *starts) + b''.join(pieces)
    args.output.write_bytes(result)
    print(f'{args.output}: {count} modules, {len(result)} bytes')


if __name__ == '__main__':
    main()
