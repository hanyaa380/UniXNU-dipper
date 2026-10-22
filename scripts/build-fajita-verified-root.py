#!/usr/bin/env python3
"""Verified root policies and archive of the fajita kernel.

Runs as a [kernel] archives builder. Every run makes its own owner key pair, so no private key exists outside the build:
on macOS kits/VerifiedRoot/tests/check.py makes it and builds the C verifier as a dylib (Homebrew OpenSSL); elsewhere
the same key pair comes from openssl genpkey and the verifier is a shared object built with the host cc and -lcrypto. For the OS and the SystemOS image of the
IPSW it signs a policy over the expanded image (kits/VerifiedRoot/tools/manifest.py), then verifies every block of it
again with the C verifier, as build-owner-os-policy.py does for the OS image, and writes the verification report
build-verified-root-probe.py requires. That script then builds build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a
with both policies. The images are read from the IPSW cache (fetch "OS" and "Cryptex1,SystemOS" first); UDIF images
are expanded from their block table, as the userdata step does, so no image is attached or written out.
"""
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
IPSW = ROOT.parents[1] / 'ipsw/21H461'
sys.path.insert(0, str(ROOT.parents[2] / 'scripts'))   # the UniXNU checkout (out/<board>/tree is two levels down)
from unixnu import userdata  # noqa: E402  the UDIF reader of the userdata step

# (component, member, volume UUID read from the image's APFS volume superblock, expanded SHA-256, 4096-byte blocks)
IMAGES = [
    ('os', '044-76040-038.dmg', '04769D26-2CA0-4825-B443-E2E5F88300F2',
     'e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2', 1483785),
    ('systemos', '044-76034-038.dmg', '8846FE78-2910-4297-AF97-D59BB6F8FC16',
     'b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f', 806400),
]


class ImageStream:
    """A read() over the expanded bytes of a disk image, for manifest.make()."""

    def __init__(self, dmg, size):
        self.pieces = userdata.read_image(dmg, size)
        self.buffer = b''

    def read(self, count):
        while len(self.buffer) < count:
            piece = next(self.pieces, None)
            if piece is None:
                break
            self.buffer += piece
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data


class View(ctypes.Structure):
    _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16), ('image_sha256', ctypes.c_uint8 * 32)]


def policy(manifest, native, keys, component, member, volume, expected_sha256, expected_blocks):
    dmg = IPSW / member
    if not dmg.is_file():
        raise SystemExit(f'{dmg} missing: run "unixnu ipsw fetch OS Cryptex1,SystemOS" first')
    size = userdata.image_size(dmg)
    out = ROOT / f'build/verified-root-{component}'
    shutil.rmtree(out, ignore_errors=True)
    print(f'{component}: signing {size} bytes of {member}', flush=True)
    record = manifest.make(ImageStream(dmg, size), size, keys / 'owner.pem', keys / 'owner-public.pem',
                           uuid.UUID(volume), 1, out)
    if record['image_sha256'] != expected_sha256 or record['block_count'] != expected_blocks:
        raise SystemExit(f'{component}: expanded image {record["image_sha256"]} ({record["block_count"]} blocks) is not the pinned original')
    encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
    anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
    view = View()
    if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or \
            bytes(view.volume_uuid) != uuid.UUID(volume).bytes:
        raise SystemExit(f'{component}: C signature/volume authentication failed')
    indices = [0, 1, view.block_count // 2, view.block_count - 1]
    complete, samples, lengths = hashlib.sha256(), [], []
    stream = ImageStream(dmg, size)
    for index in range(view.block_count):
        block = stream.read(min(4096, view.image_bytes - index * 4096))
        if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
            raise SystemExit(f'{component}: C block verification failed at {index}')
        complete.update(block)
        if index in indices:
            samples.append(block.ljust(4096, b'\0'))
            lengths.append(len(block))
    if stream.read(1) or complete.digest() != bytes(view.image_sha256):
        raise SystemExit(f'{component}: complete image digest differs')
    (out / 'original-samples.bin').write_bytes(b''.join(samples))
    report = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
                  volume_uuid=volume, generation=view.generation, source_member=member,
                  source_format='UDIF' if userdata.udif(dmg) else 'raw',
                  trusted_key_sha256=record['trusted_key_sha256'], actual_sample_indices=indices,
                  actual_sample_lengths=lengths, real_c_signature_and_block_verification=True,
                  original_image_modified=False, host_only=True, boot_root_authenticated=False,
                  qemu_tested=False, hardware_tested=False)
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {component} signature and every one of {view.block_count} blocks verified by the C verifier', flush=True)


def host_verifier(host):
    """The owner key pair and the C verifier of the host; returns the verifier library."""
    if platform.system() == 'Darwin':
        openssl = subprocess.check_output(['brew', '--prefix', 'openssl@3'], text=True).strip()
        subprocess.run(['python3', ROOT / 'kits/VerifiedRoot/tests/check.py', '--output', host, '--openssl-prefix', openssl],
                       cwd=ROOT, check=True)
        return host / 'verified-root.dylib'
    # As check.py does it, without its macOS and iOS test builds.
    keys = host / 'keys'
    keys.mkdir(parents=True, mode=0o700, exist_ok=True)
    os.chmod(keys, 0o700)
    for name in ('owner', 'other'):
        private, public = keys / (name + '.pem'), keys / (name + '-public.pem')
        private.unlink(missing_ok=True)
        os.close(os.open(private, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600))
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:3072', '-out', private],
                       check=True, capture_output=True)
        subprocess.run(['openssl', 'pkey', '-in', private, '-pubout', '-out', public], check=True, capture_output=True)
    library = host / 'verified-root.so'
    kit = ROOT / 'kits/VerifiedRoot'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I', kit / 'include',
                    kit / 'src/verified_root.c', kit / 'tests/openssl_provider.c', '-lcrypto', '-o', library], check=True)
    return library


def main():
    host = ROOT / 'build/verified-root-host'
    library = host_verifier(host)
    spec = importlib.util.spec_from_file_location('verified_manifest', ROOT / 'kits/VerifiedRoot/tools/manifest.py')
    manifest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest)
    native = ctypes.CDLL(str(library))
    native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(View)]
    native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
    for image in IMAGES:
        policy(manifest, native, host / 'keys', *image)
    subprocess.run(['python3', ROOT / 'scripts/build-verified-root-probe.py', '--component', 'os', '--include-systemos'],
                   cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
