#!/usr/bin/env python3
"""Request a USB snapshot and save a CRC-verified RGB PNG using stdlib only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
import zlib
from xnu_usb_control import ControlError, UnixControlClient, execute_cli_request

MAX_BYTES = 16 * 1024 * 1024


def decode_pixels(data, width, height, encoding, crc):
    expected = width * height * 3
    if not 1 <= width <= 4096 or not 1 <= height <= 4096 or not 0 < expected <= MAX_BYTES:
        raise ControlError('invalid snapshot dimensions')
    if encoding == 'rgb':
        rgb = bytes(data)
    elif encoding == 'rle':
        if len(data) % 4:
            raise ControlError('truncated RLE tuple')
        rgb = bytearray()
        for at in range(0, len(data), 4):
            count = data[at]
            if not count or len(rgb) + count * 3 > expected:
                raise ControlError('invalid RLE run or expanded image overflow')
            rgb.extend(data[at + 1:at + 4] * count)
        rgb = bytes(rgb)
    else:
        raise ControlError('unsupported snapshot encoding')
    if len(rgb) != expected or zlib.crc32(rgb) & 0xffffffff != crc:
        raise ControlError('incomplete snapshot or frame CRC mismatch')
    return rgb


def png_bytes(rgb, width, height):
    def chunk(kind, content):
        return struct.pack('>I', len(content)) + kind + content + struct.pack('>I', zlib.crc32(kind + content) & 0xffffffff)
    scanlines = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b''))


def receive_snapshot(first, next_reply, deadline):
    if (first.verb != 'SCREENSHOT' or not first.session or not first.seq or
            first.status != 'OK' or first.fields.get('state') != 'ready'):
        raise ControlError('snapshot unavailable: ' + first.fields.get('state', first.status))
    f = first.fields
    try:
        width, height = int(f['width']), int(f['height'])
        raw_bytes, encoded = int(f['bytes']), int(f['encoded'])
        frame, crc = int(f['frame']), int(f['crc32'], 16)
        encoding = f['encoding']
    except (KeyError, ValueError) as exc:
        raise ControlError('invalid snapshot metadata') from exc
    if (not 1 <= width <= 4096 or not 1 <= height <= 4096 or
            raw_bytes != width * height * 3 or not 0 < raw_bytes <= MAX_BYTES or
            not 0 < encoded <= raw_bytes or frame != first.seq or not 0 <= crc <= 0xffffffff or
            encoding not in ('rgb', 'rle') or (encoding == 'rgb' and encoded != raw_bytes)):
        raise ControlError('snapshot metadata outside bounds')
    data = bytearray()
    while len(data) < encoded:
        if time.monotonic() >= deadline:
            raise ControlError('snapshot deadline expired; no command automatically retried')
        reply = next_reply()
        fields = reply.fields
        try:
            offset = int(fields['offset'])
            received_frame = int(fields['frame'])
            chunk = bytes.fromhex(fields['data'])
        except (KeyError, ValueError) as exc:
            raise ControlError('snapshot chunk missing or malformed') from exc
        if (reply.status != 'OK' or fields.get('state') != 'data' or
                reply.session != first.session or reply.verb != 'SHOTNEXT' or
                received_frame != frame or offset != len(data) or
                not 0 < len(chunk) <= 192 or len(data) + len(chunk) > encoded):
            raise ControlError('stale, missing, repeated or oversized snapshot chunk')
        data.extend(chunk)
    rgb = decode_pixels(data, width, height, encoding, crc)
    return png_bytes(rgb, width, height), dict(width=width, height=height, frame=frame,
        session=f'{first.session:016x}', rgb_bytes=raw_bytes, encoded_bytes=encoded,
        encoding=encoding, crc32=f'{crc:08x}', rgb_sha256=hashlib.sha256(rgb).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--socket', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timeout', type=float, default=1200, help='overall image transfer budget in seconds')
    a = p.parse_args()
    if not 0 < a.timeout < float('inf'):
        p.error('--timeout must be finite and positive')
    if a.output.exists() or a.output.is_symlink():
        p.error('refusing to overwrite an existing image')
    try:
        deadline = time.monotonic() + a.timeout
        first = execute_cli_request(a.socket, 'SCREENSHOT', min(5, a.timeout))
        seq = first.seq
        def next_reply():
            nonlocal seq
            if seq == 0xffffffff:
                raise ControlError('sequence exhausted')
            seq += 1
            with UnixControlClient(a.socket, min(5, max(0.001, deadline - time.monotonic()))) as client:
                reply = client.request('SHOTNEXT', first.session, seq)
            if reply.seq != seq or reply.session != first.session:
                raise ControlError('control transaction mismatch')
            return reply
        png, metadata = receive_snapshot(first, next_reply, deadline)
        a.output.parent.mkdir(parents=True, exist_ok=True)
        with a.output.open('xb') as stream:
            stream.write(png)
        metadata.update(output=str(a.output.resolve()), png_sha256=hashlib.sha256(png).hexdigest())
        print(json.dumps(metadata, indent=2))
    except (ControlError, OSError) as exc:
        p.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    main()
