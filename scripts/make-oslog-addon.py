#!/usr/bin/env python3
"""Pack the signed oslogprobe into the kernel's owner NGPUADD2 sidecar."""
import argparse
import struct
import zlib
from pathlib import Path


def padded(data):
    return data + bytes(-len(data) % 8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    path = b'bin/oslogprobe\0'
    data = args.probe.read_bytes()
    packed = zlib.compress(data, 9)
    body = (struct.pack('<IIQQ', len(path), 0o755, len(data), len(packed)) +
            padded(path) + padded(packed))
    blob = b'NGPUADD2' + struct.pack('<IIQ', 1, 0, 24 + len(body)) + body
    args.output.write_bytes(blob)
    print(f'{args.output}: bin/oslogprobe {len(data)} bytes, addon {len(blob)} bytes')


if __name__ == '__main__':
    main()
