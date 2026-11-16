#!/usr/bin/env python3
"""Strict bounded codec, Unix-socket client and CLI for the Qualcomm USB control protocol."""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import socket
import sys
import zlib

FRAME_BYTES = 128
REPLY_BYTES = 512
WORK_BYTES = 256
VERBS = ('HELLO', 'PING', 'STATUS', 'REBOOT', 'SCREENSHOT', 'SHOTNEXT')
FIELD_KEYS = frozenset((
    'session', 'last_seq', 'control', 'rx', 'rxo', 'tx', 'poll', 'usb',
    'er', 'bp', 'bo', 'ev', 'gen', 'rej', 'state', 'result',
    'width', 'height', 'bytes', 'encoded', 'crc32', 'frame', 'encoding', 'offset', 'data',
))
HEX16 = re.compile(rb'[0-9a-fA-F]{16}')
HEX8 = re.compile(rb'[0-9a-fA-F]{8}')
FIELD_NAME = re.compile(rb'[a-z][a-z0-9_]*')
FIELD_VALUE = re.compile(rb'[A-Za-z0-9_.:+-]+')


class ControlError(ValueError):
    """A frame, reply or socket exchange violates the fixed control contract."""


@dataclass(frozen=True)
class Request:
    session: int
    seq: int
    verb: str
    crc: int
    raw: bytes


@dataclass(frozen=True)
class Reply:
    session: int
    seq: int
    verb: str
    status: str
    fields: dict
    crc: int
    raw: bytes


def crc32(data):
    return zlib.crc32(data) & 0xffffffff


def _integer(value, maximum, name, allow_zero=True):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ControlError(f'{name} is outside its fixed integer range')
    if not allow_zero and value == 0:
        raise ControlError(f'{name} must be nonzero')
    return value


def _line(frame, limit, kind):
    if not isinstance(frame, (bytes, bytearray, memoryview)):
        raise ControlError(f'{kind} must be bytes')
    frame = bytes(frame)
    if not 1 <= len(frame) <= limit or not frame.endswith(b'\n'):
        raise ControlError(f'{kind} must be one complete line within {limit} bytes')
    if b'\r' in frame or b'\x00' in frame or not frame.isascii():
        raise ControlError(f'{kind} must be finite printable ASCII')
    return frame[:-1]


def _crc_parts(line, prefix):
    split = line.rfind(b' ')
    if split < 0 or len(line) - split - 1 != 8 or not HEX8.fullmatch(line[split + 1:]):
        raise ControlError(f'{prefix} requires an eight-digit CRC')
    expected = int(line[split + 1:], 16)
    if expected != crc32(line[:split]):
        raise ControlError(f'{prefix} CRC mismatch')
    return line[:split], expected


def encode_request(verb, session=0, seq=0):
    if verb not in VERBS:
        raise ControlError('unknown control verb')
    session = _integer(session, (1 << 64) - 1, 'session')
    seq = _integer(seq, (1 << 32) - 1, 'sequence')
    if verb == 'HELLO':
        if session or seq:
            raise ControlError('HELLO requires session zero and sequence zero')
    elif not session or not seq:
        raise ControlError('non-HELLO commands require nonzero session and sequence')
    prefix = f'SXNU1 {session:016x} {seq:08x} {verb}'.encode('ascii')
    frame = prefix + f' {crc32(prefix):08x}\n'.encode('ascii')
    if len(frame) > FRAME_BYTES:
        raise ControlError('encoded request exceeds the fixed frame limit')
    return frame


def parse_request(frame):
    line = _line(frame, FRAME_BYTES, 'request frame')
    body, expected_crc = _crc_parts(line, 'request')
    parts = body.split(b' ')
    if len(parts) != 4 or any(not part for part in parts) or parts[0] != b'SXNU1':
        raise ControlError('request grammar differs')
    if not HEX16.fullmatch(parts[1]) or not HEX8.fullmatch(parts[2]):
        raise ControlError('request session or sequence is not fixed hex')
    try:
        verb = parts[3].decode('ascii')
    except UnicodeDecodeError as exc:
        raise ControlError('request verb is not ASCII') from exc
    if verb not in VERBS:
        raise ControlError('unknown control verb')
    session, seq = int(parts[1], 16), int(parts[2], 16)
    if verb == 'HELLO':
        if session or seq:
            raise ControlError('HELLO requires session zero and sequence zero')
    elif not session or not seq:
        raise ControlError('non-HELLO commands require nonzero session and sequence')
    return Request(session, seq, verb, expected_crc, bytes(frame))


def parse_reply(frame):
    line = _line(frame, REPLY_BYTES, 'reply frame')
    body, expected_crc = _crc_parts(line, 'reply')
    parts = body.split(b' ')
    if len(parts) < 6 or any(not part for part in parts) or parts[0] != b'SXNU1R':
        raise ControlError('reply grammar differs')
    if not HEX16.fullmatch(parts[1]) or not HEX8.fullmatch(parts[2]):
        raise ControlError('reply session or sequence is not fixed hex')
    try:
        verb = parts[3].decode('ascii')
        status = parts[4].decode('ascii')
    except UnicodeDecodeError as exc:
        raise ControlError('reply verb or status is not ASCII') from exc
    if verb not in VERBS or status not in ('OK', 'ERR'):
        raise ControlError('reply verb or status is outside the fixed set')
    fields = {}
    for token in parts[5:]:
        if token.count(b'=') != 1:
            raise ControlError('reply field is not exactly key=value')
        name, value = token.split(b'=', 1)
        if (not FIELD_NAME.fullmatch(name) or not FIELD_VALUE.fullmatch(value) or
                name.decode('ascii') not in FIELD_KEYS):
            raise ControlError('reply field is outside the fixed bounded set')
        key = name.decode('ascii')
        if key in fields:
            raise ControlError('duplicate reply field')
        fields[key] = value.decode('ascii')
    return Reply(int(parts[1], 16), int(parts[2], 16), verb, status, fields,
                 expected_crc, bytes(frame))


def validate_reply(reply):
    required = {
        'HELLO': {'session', 'last_seq', 'control'},
        'PING': {'rx', 'rxo', 'tx', 'poll'},
        'STATUS': {'usb', 'er', 'rx', 'rxo', 'tx', 'bp', 'bo', 'poll', 'ev', 'gen', 'rej'},
    }
    if reply.verb == 'HELLO':
        if reply.status != 'OK' or reply.session == 0:
            raise ControlError('HELLO reply is not a successful kernel session announcement')
    elif reply.verb in ('REBOOT', 'SCREENSHOT', 'SHOTNEXT'):
        if 'state' not in reply.fields:
            raise ControlError('command reply lacks its state')
    elif reply.status != 'OK' or not required[reply.verb] <= set(reply.fields):
        raise ControlError(f'{reply.verb} reply lacks its fixed status fields')
    if reply.verb == 'HELLO' and not required[reply.verb] <= set(reply.fields):
        raise ControlError('HELLO reply lacks its fixed session fields')
    return reply


class ReplyExtractor:
    """Bounded scanner that finds valid replies even mid-console or fragmented."""

    MAGIC = b'SXNU1R '

    def __init__(self):
        self._scan = bytearray()
        self._candidate = bytearray()

    def _scan_byte(self, byte):
        self._scan.append(byte)
        if self._scan.endswith(self.MAGIC):
            self._candidate = bytearray(self._scan)
            self._scan.clear()
        elif byte == 10:
            self._scan.clear()
        elif len(self._scan) >= len(self.MAGIC):
            del self._scan[:len(self._scan) - (len(self.MAGIC) - 1)]

    def feed(self, data):
        replies = []
        for byte in data:
            if not self._candidate:
                self._scan_byte(byte)
                continue
            self._candidate.append(byte)
            if byte == 10:
                if len(self._candidate) <= REPLY_BYTES:
                    try:
                        replies.append(validate_reply(parse_reply(bytes(self._candidate))))
                    except ControlError:
                        pass
                self._candidate.clear()
            elif len(self._candidate) >= REPLY_BYTES:
                tail = bytes(self._candidate[-6:])
                self._candidate.clear()
                self._scan.extend(tail)
        return replies


class UnixControlClient:
    """One-request-per-connection client. It never retries a request."""

    def __init__(self, path, timeout=5.0):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        try:
            self.socket.connect(str(path))
        except BaseException:
            self.socket.close()
            raise

    def request_frame(self, frame):
        request = parse_request(frame)
        self.socket.sendall(request.raw)
        received = bytearray()
        while len(received) <= REPLY_BYTES:
            chunk = self.socket.recv(REPLY_BYTES + 1 - len(received))
            if not chunk:
                raise ControlError('control socket closed before a complete reply')
            received.extend(chunk)
            newline = received.find(b'\n')
            if newline >= 0:
                if newline != len(received) - 1:
                    raise ControlError('control socket returned trailing bytes')
                return validate_reply(parse_reply(bytes(received)))
        raise ControlError('control socket reply exceeds the fixed limit')

    def request(self, verb, session=0, seq=0):
        return self.request_frame(encode_request(verb, session, seq))

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


def _hello_fields(reply):
    try:
        kernel_session = int(reply.fields['session'], 16)
        last_seq = int(reply.fields['last_seq'], 16)
        control = int(reply.fields['control'], 10)
    except (KeyError, ValueError) as exc:
        raise ControlError('HELLO fields are not fixed hexadecimal/decimal values') from exc
    if (kernel_session != reply.session or kernel_session == 0 or
            not 0 <= last_seq <= 0xffffffff or control != 1):
        raise ControlError('HELLO reply fields disagree with the kernel session')
    return kernel_session, last_seq


def execute_cli_request(socket_path, verb, timeout=5.0):
    if verb not in VERBS:
        raise ControlError('unknown control verb')
    with UnixControlClient(socket_path, timeout) as client:
        hello = client.request('HELLO')
        kernel_session, last_seq = _hello_fields(hello)
    if verb == 'HELLO':
        return hello
    if last_seq == 0xffffffff:
        raise ControlError('sequence exhausted; a new transport session is required')
    # The collector accepts exactly one request per connection. The HELLO
    # connection has been closed; issue the sole state-changing frame on a new
    # one and never retry it.
    with UnixControlClient(socket_path, timeout) as client:
        reply = client.request(verb, kernel_session, last_seq + 1)
    if (reply.session != kernel_session or reply.seq != last_seq + 1 or
            reply.verb != verb):
        raise ControlError('reply does not match the issued command')
    return reply


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True, type=Path,
                        help='collector-created private Unix control socket')
    parser.add_argument('--timeout', type=float, default=5.0)
    parser.add_argument('verb', choices=tuple(v.lower() for v in VERBS))
    args = parser.parse_args(argv)
    if not args.timeout > 0:
        parser.error('--timeout must be positive')
    try:
        reply = execute_cli_request(args.socket, args.verb.upper(), args.timeout)
    except (ControlError, OSError) as exc:
        print(json.dumps({'ok': False, 'verb': args.verb.upper(), 'error': str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps({
        'ok': reply.status == 'OK', 'verb': reply.verb, 'status': reply.status,
        'session': f'{reply.session:016x}', 'seq': f'{reply.seq:08x}',
        'fields': reply.fields,
    }, sort_keys=True))
    return 0 if reply.status == 'OK' else 1


if __name__ == '__main__':
    raise SystemExit(main())
