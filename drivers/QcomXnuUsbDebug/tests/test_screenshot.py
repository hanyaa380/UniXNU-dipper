#!/usr/bin/env python3
"""Host PNG/protocol tests using replies from the real C control handler."""
import argparse
from dataclasses import replace
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import screenshot
import xnu_usb_control as codec


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--wire', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    replies = codec.ReplyExtractor().feed(a.wire.read_bytes())
    first, data = replies[:2]
    assert first.verb == 'SCREENSHOT' and data.verb == 'SHOTNEXT'
    assert replies[2].fields['state'] == 'capture-failed'
    assert replies[3].fields['state'] == 'noimage'
    assert replies[4].fields['state'] == 'nohook'
    assert replies[5].verb == 'REBOOT' and replies[5].fields['state'] == 'queued'
    png, metadata = screenshot.receive_snapshot(first, lambda: data, time.monotonic() + 5)
    expected = bytes(i * 7 for i in range(24))
    assert metadata['width'] == 4 and metadata['height'] == 2
    assert screenshot.decode_pixels(expected, 4, 2, 'rgb', zlib.crc32(expected)) == expected
    assert screenshot.decode_pixels(bytes([8, 1, 2, 3]), 4, 2, 'rle', zlib.crc32(bytes([1, 2, 3]) * 8)) == bytes([1, 2, 3]) * 8
    def refuses(call):
        try: call()
        except codec.ControlError: return
        raise AssertionError('invalid frame accepted')
    for change in ({'offset': '1'}, {'frame': '99'}, {'data': '00'}, {'data': 'xyz'}):
        broken = replace(data, fields=dict(data.fields, **change))
        refuses(lambda: screenshot.receive_snapshot(first, lambda: broken, time.monotonic() + 5))
    refuses(lambda: screenshot.receive_snapshot(first, lambda: data, time.monotonic() - 1))
    refuses(lambda: screenshot.decode_pixels(b'\0\1\2\3', 4, 2, 'rle', 0))
    refuses(lambda: screenshot.decode_pixels(b'\xff\1\2\3', 4, 2, 'rle', 0))
    refuses(lambda: screenshot.decode_pixels(expected, 4, 2, 'rgb', 0))
    # Exercise the real CLI and socket codec with fragmented native replies.
    server = socket.socket(socket.AF_UNIX)
    path = a.output / 'shot.sock'
    server.bind(str(path)); server.listen(4); server.settimeout(5)
    errors = []
    hello_body = b'SXNU1R 0000000000000007 00000000 HELLO OK session=0000000000000007 last_seq=00000000 control=1'
    hello = hello_body + f' {zlib.crc32(hello_body):08x}\n'.encode()
    def serve():
        try:
            for verb, reply in [('HELLO', hello), ('SCREENSHOT', first.raw), ('SHOTNEXT', data.raw)]:
                conn, _ = server.accept()
                with conn:
                    request = bytearray()
                    while not request.endswith(b'\n'):
                        request.extend(conn.recv(128))
                    assert codec.parse_request(request).verb == verb
                    for at in range(0, len(reply), 3): conn.sendall(reply[at:at + 3])
        except BaseException as exc: errors.append(str(exc))
    worker = threading.Thread(target=serve, daemon=True); worker.start()
    output = a.output / 'native-handler.png'
    result = subprocess.run([sys.executable, '-B', str(Path(screenshot.__file__)), '--socket', str(path),
        '--output', str(output), '--timeout', '5'], capture_output=True, text=True, timeout=10)
    worker.join(6); server.close(); path.unlink()
    assert not errors and not worker.is_alive(), errors
    assert result.returncode == 0, result.stderr
    assert output.read_bytes() == png
    offset = 8; scanlines = bytearray()
    while offset < len(png):
        size = struct.unpack('>I', png[offset:offset + 4])[0]
        kind = png[offset + 4:offset + 8]; payload = png[offset + 8:offset + 8 + size]
        assert zlib.crc32(kind + payload) & 0xffffffff == struct.unpack('>I', png[offset + 8 + size:offset + 12 + size])[0]
        if kind == b'IDAT':scanlines.extend(zlib.decompress(payload))
        offset += size + 12
    assert scanlines == b'\0' + expected[:12] + b'\0' + expected[12:]
    # Existing output is refused before opening the socket.
    again = subprocess.run([sys.executable, '-B', str(Path(screenshot.__file__)), '--socket', str(path),
        '--output', str(output)], capture_output=True, text=True)
    assert again.returncode and output.read_bytes() == png
    print('PASS native C replies -> fragmented socket -> screenshot CLI -> CRC-verified PNG; negative frames refused')


if __name__ == '__main__':main()
