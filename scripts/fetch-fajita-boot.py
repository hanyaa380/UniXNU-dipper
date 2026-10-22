#!/usr/bin/env python3
"""Extract a partition image of the stock OnePlus 6T firmware from its full OTA zip, read with range requests.

The full OTA holds an A/B payload.bin. Only the zip directory, the payload manifest and the data of the selected
partition are read; the image is checked against the SHA-256 in the manifest. --info prints the boot image header,
the values [bootimg.image] of board.toml is checked against.

Usage: fetch-fajita-boot.py [--url URL | --zip FILE] [--partition boot] [-o FILE] [--info]
"""
import argparse
import bz2
import hashlib
import lzma
from pathlib import Path
import struct
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# OxygenOS 11.1.2.2 (34.J.62), the last release for fajita, from the OnePlus update server.
URL = ('https://otafsg1.h2os.com/patch/amazone2/GLO/OnePlus6TOxygen/OnePlus6TOxygen_34.J.62_GLO_0620_2111252336/'
       'OnePlus6TOxygen_34.J.62_OTA_0620_all_2111252336_339a2fa8335f21.zip')
REPLACE, REPLACE_BZ, ZERO, REPLACE_XZ = 0, 1, 6, 8


class HttpFile:
    """A read-only seekable file over HTTP range requests."""

    def __init__(self, url):
        self.url, self.pos = url, 0
        with urllib.request.urlopen(urllib.request.Request(url, method='HEAD')) as reply:
            if reply.headers.get('Accept-Ranges') != 'bytes':
                raise SystemExit(f'{url}: the server does not take range requests')
            self.size = int(reply.headers['Content-Length'])

    def seek(self, offset, whence=0):
        self.pos = (offset, self.pos + offset, self.size + offset)[whence]
        return self.pos

    def tell(self):
        return self.pos

    def seekable(self):
        return True

    def read(self, count=-1):
        end = self.size if count < 0 else min(self.size, self.pos + count)
        if end <= self.pos:
            return b''
        request = urllib.request.Request(self.url, headers={'Range': f'bytes={self.pos}-{end - 1}'})
        with urllib.request.urlopen(request) as reply:
            data = reply.read()
        if len(data) != end - self.pos:
            raise SystemExit(f'short read at {self.pos}')
        self.pos = end
        return data


def varint(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if byte < 0x80:
            return value, pos


def fields(data):
    """Protobuf fields as (number, value): an int for varints, bytes for length-delimited fields."""
    pos = 0
    while pos < len(data):
        key, pos = varint(data, pos)
        number, kind = key >> 3, key & 7
        if kind == 0:
            value, pos = varint(data, pos)
        elif kind == 2:
            length, pos = varint(data, pos)
            value, pos = data[pos:pos + length], pos + length
        elif kind == 1:
            value, pos = data[pos:pos + 8], pos + 8
        elif kind == 5:
            value, pos = data[pos:pos + 4], pos + 4
        else:
            raise SystemExit(f'unsupported protobuf wire type {kind}')
        yield number, value


def payload_offset(source):
    """Offset of the stored payload.bin in the zip."""
    archive = zipfile.ZipFile(source)
    info = archive.getinfo('payload.bin')
    if info.compress_type != zipfile.ZIP_STORED:
        raise SystemExit('payload.bin is compressed in the zip')
    source.seek(info.header_offset)
    header = source.read(30)
    name_length, extra_length = struct.unpack_from('<HH', header, 26)
    return info.header_offset + 30 + name_length + extra_length


def partition(source, base, name):
    source.seek(base)
    magic, version, manifest_size = struct.unpack('>4sQQ', source.read(20))
    if magic != b'CrAU' or version != 2:
        raise SystemExit(f'not a version 2 payload: {magic!r} {version}')
    signature_size, = struct.unpack('>I', source.read(4))
    manifest = source.read(manifest_size)
    data_start = base + 24 + manifest_size + signature_size
    block_size = 4096
    for number, value in fields(manifest):
        if number == 3:
            block_size = value
    for number, value in fields(manifest):
        if number != 13:
            continue
        entry = list(fields(value))
        if dict(entry).get(1) != name.encode():
            continue
        info = dict(fields(dict(entry)[7]))
        operations = [dict(fields(op)) | {'extents': [dict(fields(e)) for n, e in fields(op) if n == 6]}
                      for n, op in entry if n == 8]
        return data_start, block_size, info[1], info[2], operations
    raise SystemExit(f'no partition {name} in the payload')


def extract(source, data_start, block_size, size, operations):
    image = bytearray(size)
    for op in operations:
        kind = op.get(1, REPLACE)
        if kind == ZERO:
            continue
        source.seek(data_start + op.get(2, 0))
        data = source.read(op[3])
        data = {REPLACE: bytes, REPLACE_BZ: bz2.decompress, REPLACE_XZ: lzma.decompress}.get(kind, None)(data) \
            if kind in (REPLACE, REPLACE_BZ, REPLACE_XZ) else None
        if data is None:
            raise SystemExit(f'operation type {kind} is not in a full OTA')
        pos = 0
        for extent in op['extents']:
            start, length = extent.get(1, 0) * block_size, extent.get(2, 0) * block_size
            image[start:start + length] = data[pos:pos + length].ljust(length, b'\0')[:max(0, min(length, size - start))]
            pos += length
    return bytes(image)


def describe(image):
    if image[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags_addr, pagesize,
     header_version) = struct.unpack_from('<9I', image, 8)
    cmdline = image[64:64 + 512].split(b'\0')[0].decode(errors='replace')
    base = kernel_addr - 0x8000
    for key, value in [('header_version', header_version), ('pagesize', pagesize), ('base', hex(base)),
                       ('kernel_offset', hex(kernel_addr - base)), ('ramdisk_offset', hex(ramdisk_addr - base)),
                       ('second_offset', hex(second_addr - base)), ('tags_offset', hex(tags_addr - base)),
                       ('kernel_size', kernel_size), ('ramdisk_size', ramdisk_size), ('cmdline', cmdline)]:
        print(f'{key:16} {value}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--url', default=URL, help='full OTA zip URL (default: OxygenOS 11.1.2.2)')
    parser.add_argument('--zip', type=Path, help='local full OTA zip instead of the URL')
    parser.add_argument('--partition', default='boot')
    parser.add_argument('-o', '--output', type=Path)
    parser.add_argument('--info', action='store_true', help='print the boot image header')
    args = parser.parse_args()

    source = args.zip.open('rb') if args.zip else HttpFile(args.url)
    data_start, block_size, size, digest, operations = partition(source, payload_offset(source), args.partition)
    image = extract(source, data_start, block_size, size, operations)
    if hashlib.sha256(image).digest() != digest:
        raise SystemExit(f'{args.partition}: SHA-256 differs from the payload manifest')
    output = args.output or ROOT / f'.cache/firmware/fajita/{args.partition}.img'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(image)
    print(f'{args.partition}: {output} ({size} bytes, sha256 {digest.hex()})')
    if args.info and args.partition == 'boot':
        describe(image)


if __name__ == '__main__':
    main()
