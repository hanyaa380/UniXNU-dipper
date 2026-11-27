#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Inject device-local A630 GMU, SQE and ZAP images into a placeholder kernel.

Public cloud builds contain only A630-GMU-PLACEHOLDER in
native_a630_gmu_firmware_blob, A630-SQE-PLACEHOLDER in
native_a630_sqe_firmware_blob, and the two A630-ZAP placeholders. Vendor
firmware must stay private, so this tool creates a new kernel file and
replaces those zero-filled arrays. The source kernel is unchanged.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


FIRMWARE_BYTES = 0x8000
EXPECTED_FNV1A = 0xC7FE81D5
MARKER = b"A630-GMU-PLACEHOLDER"
SYMBOL = "_native_a630_gmu_firmware_blob"
SQE_FILE_BYTES = 29988
SQE_EXPECTED_FNV1A = 0xA408DA75
SQE_MARKER = b"A630-SQE-PLACEHOLDER"
SQE_SYMBOL = "_native_a630_sqe_firmware_blob"
ZAP_MDT_FILE_BYTES = 6684
ZAP_MDT_EXPECTED_FNV1A = 0x48C8FC22
ZAP_MDT_SLOT_BYTES = 0x2000
ZAP_MDT_MARKER = b"A630-ZAP-MDT-PLACEHOLDER"
ZAP_MDT_SYMBOL = "_native_a630_zap_mdt_blob"
ZAP_B02_FILE_BYTES = 1820
ZAP_B02_EXPECTED_FNV1A = 0xFBA23A9F
ZAP_B02_SLOT_BYTES = 0x1000
ZAP_B02_MARKER = b"A630-ZAP-B02-PLACEHOLDER"
ZAP_B02_SYMBOL = "_native_a630_zap_b02_blob"
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19


def fail(message):
    raise SystemExit(f"patch-a630-firmware: {message}")


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def symbol_address(kernel, symbol):
    nm = os.environ.get("LLVM_NM") or shutil.which("llvm-nm")
    if not nm:
        fail("llvm-nm is not on PATH and LLVM_NM is unset")
    result = subprocess.run(
        [nm, "-n", str(kernel)],
        check=True,
        capture_output=True,
        text=True,
    )
    matches = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[-1] == symbol:
            try:
                matches.append(int(fields[0], 16))
            except ValueError:
                pass
    if len(matches) != 1:
        fail(f"expected one {symbol}, found {len(matches)}")
    return matches[0]


def file_offset(data, address, size):
    if len(data) < 32:
        fail("kernel is shorter than a 64-bit Mach-O header")
    magic, _, _, _, command_count = struct.unpack_from("<IiiII", data, 0)
    if magic != MH_MAGIC_64:
        fail("kernel is not a 64-bit Mach-O")
    offset = 32
    for _ in range(command_count):
        command, command_size = struct.unpack_from("<II", data, offset)
        if command_size < 8 or offset + command_size > len(data):
            fail("invalid Mach-O load command")
        if command == LC_SEGMENT_64:
            _, vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                "<16sQQQQ", data, offset + 8
            )
            backed = min(vmsize, filesize)
            if vmaddr <= address and address + size <= vmaddr + backed:
                return fileoff + address - vmaddr
        offset += command_size
    fail(f"{address:#x}..{address + size:#x} is not file-backed")


def patch_slot(data, kernel, symbol, marker, firmware, slot_bytes):
    address = symbol_address(kernel, symbol)
    offset = file_offset(data, address, slot_bytes)
    slot = data[offset : offset + slot_bytes]
    if not slot.startswith(marker):
        fail(f"{symbol} does not start with the public placeholder marker")
    if any(slot[len(marker) :]):
        fail(f"{symbol} placeholder is not zero-filled or has already been patched")
    data[offset : offset + slot_bytes] = firmware.ljust(slot_bytes, b"\0")
    return address, offset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--sqe-firmware", type=Path)
    parser.add_argument("--zap-mdt", type=Path)
    parser.add_argument("--zap-b02", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--expect-fnv1a",
        default=f"{EXPECTED_FNV1A:08x}",
        help="required firmware FNV-1a (default: the current Mi 8 vendor image)",
    )
    parser.add_argument(
        "--expect-sqe-fnv1a",
        default=f"{SQE_EXPECTED_FNV1A:08x}",
        help="required SQE FNV-1a (default: the current Mi 8 vendor image)",
    )
    parser.add_argument(
        "--expect-zap-mdt-fnv1a",
        default=f"{ZAP_MDT_EXPECTED_FNV1A:08x}",
        help="required ZAP MDT FNV-1a (default: the current Mi 8 vendor image)",
    )
    parser.add_argument(
        "--expect-zap-b02-fnv1a",
        default=f"{ZAP_B02_EXPECTED_FNV1A:08x}",
        help="required ZAP B02 FNV-1a (default: the current Mi 8 vendor image)",
    )
    args = parser.parse_args()

    if args.kernel.resolve() == args.output.resolve():
        fail("--output must differ from --kernel; keep the downloaded artifact intact")
    if bool(args.zap_mdt) != bool(args.zap_b02):
        fail("--zap-mdt and --zap-b02 must be supplied together")
    firmware = args.firmware.read_bytes()
    if len(firmware) != FIRMWARE_BYTES:
        fail(f"firmware is {len(firmware)} bytes; this A630 stage needs {FIRMWARE_BYTES}")
    wanted_hash = int(args.expect_fnv1a.removeprefix("0x"), 16)
    actual_hash = fnv1a(firmware)
    if actual_hash != wanted_hash:
        fail(
            f"firmware FNV-1a is {actual_hash:08x}, expected {wanted_hash:08x}"
        )

    sqe = None
    if args.sqe_firmware:
        sqe = args.sqe_firmware.read_bytes()
        if len(sqe) != SQE_FILE_BYTES:
            fail(
                f"SQE firmware is {len(sqe)} bytes; this A630 stage needs "
                f"{SQE_FILE_BYTES}"
            )
        wanted_sqe_hash = int(args.expect_sqe_fnv1a.removeprefix("0x"), 16)
        actual_sqe_hash = fnv1a(sqe)
        if actual_sqe_hash != wanted_sqe_hash:
            fail(
                f"SQE FNV-1a is {actual_sqe_hash:08x}, expected "
                f"{wanted_sqe_hash:08x}"
            )

    zap_mdt = None
    if args.zap_mdt:
        zap_mdt = args.zap_mdt.read_bytes()
        if len(zap_mdt) != ZAP_MDT_FILE_BYTES:
            fail(
                f"ZAP MDT is {len(zap_mdt)} bytes; this A630 stage needs "
                f"{ZAP_MDT_FILE_BYTES}"
            )
        wanted_zap_mdt_hash = int(
            args.expect_zap_mdt_fnv1a.removeprefix("0x"), 16
        )
        actual_zap_mdt_hash = fnv1a(zap_mdt)
        if actual_zap_mdt_hash != wanted_zap_mdt_hash:
            fail(
                f"ZAP MDT FNV-1a is {actual_zap_mdt_hash:08x}, expected "
                f"{wanted_zap_mdt_hash:08x}"
            )

    zap_b02 = None
    if args.zap_b02:
        zap_b02 = args.zap_b02.read_bytes()
        if len(zap_b02) != ZAP_B02_FILE_BYTES:
            fail(
                f"ZAP B02 is {len(zap_b02)} bytes; this A630 stage needs "
                f"{ZAP_B02_FILE_BYTES}"
            )
        wanted_zap_b02_hash = int(
            args.expect_zap_b02_fnv1a.removeprefix("0x"), 16
        )
        actual_zap_b02_hash = fnv1a(zap_b02)
        if actual_zap_b02_hash != wanted_zap_b02_hash:
            fail(
                f"ZAP B02 FNV-1a is {actual_zap_b02_hash:08x}, expected "
                f"{wanted_zap_b02_hash:08x}"
            )

    original = args.kernel.read_bytes()
    data = bytearray(original)
    address, offset = patch_slot(
        data, args.kernel, SYMBOL, MARKER, firmware, FIRMWARE_BYTES
    )
    sqe_address = None
    sqe_offset = None
    if sqe is not None:
        sqe_address, sqe_offset = patch_slot(
            data, args.kernel, SQE_SYMBOL, SQE_MARKER, sqe, FIRMWARE_BYTES
        )
    zap_mdt_address = zap_mdt_offset = None
    if zap_mdt is not None:
        zap_mdt_address, zap_mdt_offset = patch_slot(
            data, args.kernel, ZAP_MDT_SYMBOL, ZAP_MDT_MARKER, zap_mdt,
            ZAP_MDT_SLOT_BYTES
        )
    zap_b02_address = zap_b02_offset = None
    if zap_b02 is not None:
        zap_b02_address, zap_b02_offset = patch_slot(
            data, args.kernel, ZAP_B02_SYMBOL, ZAP_B02_MARKER, zap_b02,
            ZAP_B02_SLOT_BYTES
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(
        f"patched {args.output}: {SYMBOL} at VA {address:#x}, file {offset:#x}, "
        f"{len(firmware)} bytes, fnv1a {actual_hash:08x}, "
        f"sha256 {hashlib.sha256(firmware).hexdigest()}"
    )
    if sqe is not None:
        print(
            f"patched {args.output}: {SQE_SYMBOL} at VA {sqe_address:#x}, "
            f"file {sqe_offset:#x}, {len(sqe)} bytes, "
            f"fnv1a {actual_sqe_hash:08x}, "
            f"sha256 {hashlib.sha256(sqe).hexdigest()}"
        )
    if zap_mdt is not None:
        print(
            f"patched {args.output}: {ZAP_MDT_SYMBOL} at VA "
            f"{zap_mdt_address:#x}, file {zap_mdt_offset:#x}, "
            f"{len(zap_mdt)} bytes, fnv1a {actual_zap_mdt_hash:08x}, "
            f"sha256 {hashlib.sha256(zap_mdt).hexdigest()}"
        )
    if zap_b02 is not None:
        print(
            f"patched {args.output}: {ZAP_B02_SYMBOL} at VA "
            f"{zap_b02_address:#x}, file {zap_b02_offset:#x}, "
            f"{len(zap_b02)} bytes, fnv1a {actual_zap_b02_hash:08x}, "
            f"sha256 {hashlib.sha256(zap_b02).hexdigest()}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
