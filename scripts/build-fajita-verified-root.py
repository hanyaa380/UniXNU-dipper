#!/usr/bin/env python3
"""Verified root policies and archive of the fajita kernel.

Runs as a [kernel] archives builder. Every run makes its own owner key pair, so no private key exists outside the build:
on macOS kits/VerifiedRoot/tests/check.py makes it and builds the C verifier as a dylib (Homebrew OpenSSL); elsewhere
the same key pair comes from openssl genpkey and the verifier is a shared object built with the host cc and -lcrypto. For the OS and the SystemOS image of the
IPSW it signs a policy over the expanded image (kits/VerifiedRoot/tools/manifest.py), then verifies every block of it
again with the C verifier, as build-owner-os-policy.py does for the OS image, and writes the verification report
build-verified-root-probe.py requires. That script then builds build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a
with both policies. Hashing the images is done once: the block-hash table, the image record and the sample blocks are
cached (FAJITA_VERIFIED_ROOT_CACHE, default .cache/verified-root of the build tree), and later builds sign a new header
over the cached table, then check the signature and the cached samples with the C verifier. The images are read from
the IPSW cache (fetch "OS" and "Cryptex1,SystemOS" first); UDIF images
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
import struct
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
IPSW = ROOT.parents[1] / 'ipsw/21H461'
CACHE = Path(os.environ.get('FAJITA_VERIFIED_ROOT_CACHE') or ROOT / '.cache/verified-root')
# The UniXNU checkout: out/<board>/tree is two levels down from it with the default output root; with UNIXNU_OUT elsewhere, the
# tree's module links (kits/NativePrelink is a link into the checkout) lead back to it.
UNIXNU = next((base for base in (ROOT.parents[2], (ROOT / 'kits/NativePrelink').resolve().parents[1])
               if (base / 'scripts/unixnu/userdata.py').is_file()), ROOT.parents[2])
sys.path.insert(0, str(UNIXNU / 'scripts'))
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


def sign_cached(manifest, keys, cached, volume, out):
    """manifest.make() over a cached block-hash table: the same header, signature and envelope, without the image."""
    meta = json.loads((cached / 'image.json').read_text())
    table = (cached / 'hashes.bin').read_bytes()
    if hashlib.sha256(table).hexdigest() != meta['hash_table_sha256'] or len(table) != 32 * meta['block_count']:
        return None
    out.mkdir(parents=True, exist_ok=True)
    anchor = manifest.public_anchor(keys / 'owner-public.pem')
    header = struct.pack('<8s4I2Q16s32s32s32sQ', b'VROOT01\0', 1, 160, 4096, 0, meta['image_bytes'], meta['block_count'],
                         uuid.UUID(volume).bytes, bytes.fromhex(meta['image_sha256']), bytes.fromhex(meta['hash_table_sha256']),
                         hashlib.sha256(anchor).digest(), 1)
    (out / 'header.bin').write_bytes(header)
    (out / 'trusted-key.bin').write_bytes(anchor)
    signature = out / 'signature.bin'
    subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(keys / 'owner.pem'), '-out', str(signature), str(out / 'header.bin')],
                   check=True)
    (out / 'manifest.bin').write_bytes(header + signature.read_bytes() + table)
    shutil.copyfile(cached / 'hashes.bin', out / 'hashes.bin')
    return dict(meta, trusted_key_sha256=hashlib.sha256(anchor).hexdigest())


def policy(manifest, native, keys, component, member, volume, expected_sha256, expected_blocks):
    out = ROOT / f'build/verified-root-{component}'
    cached = CACHE / component
    if (cached / 'image.json').is_file():
        meta = json.loads((cached / 'image.json').read_text())
        if meta.get('image_sha256') == expected_sha256 and meta.get('block_count') == expected_blocks:
            shutil.rmtree(out, ignore_errors=True)
            record = sign_cached(manifest, keys, cached, volume, out)
            if record:
                encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
                anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
                view = View()
                if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or \
                        bytes(view.volume_uuid) != uuid.UUID(volume).bytes:
                    raise SystemExit(f'{component}: C signature/volume authentication of the cached table failed')
                samples = (cached / 'original-samples.bin').read_bytes()
                for slot, (index, length) in enumerate(zip(meta['sample_indices'], meta['sample_lengths'])):
                    block = samples[slot * 4096:slot * 4096 + length]
                    if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
                        raise SystemExit(f'{component}: cached sample block {index} fails the C verifier')
                shutil.copyfile(cached / 'original-samples.bin', out / 'original-samples.bin')
                report = dict(image_bytes=meta['image_bytes'], blocks_verified=meta['block_count'], image_sha256=meta['image_sha256'],
                              volume_uuid=volume, generation=1, source_member=member, source_format=meta['source_format'],
                              trusted_key_sha256=record['trusted_key_sha256'], actual_sample_indices=meta['sample_indices'],
                              actual_sample_lengths=meta['sample_lengths'], real_c_signature_and_block_verification=True,
                              blocks_verified_when_cached=meta['verified_at'], signed_from_cached_table=True,
                              original_image_modified=False, host_only=True, boot_root_authenticated=False,
                              qemu_tested=False, hardware_tested=False)
                (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
                print(f'PASS: {component} signed over the cached table; signature and {len(meta["sample_indices"])} sample blocks '
                      f'verified by the C verifier (every block was at {meta["verified_at"]})', flush=True)
                return
    dmg = IPSW / member
    if not dmg.is_file():
        raise SystemExit(f'{dmg} missing: run "unixnu ipsw fetch OS Cryptex1,SystemOS" first')
    size = userdata.image_size(dmg)
    out = ROOT / f'build/verified-root-{component}'
    shutil.rmtree(out, ignore_errors=True)
    print(f'{component}: hashing and signing {size} bytes of {member}', flush=True)
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
    cached.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(out / 'hashes.bin', cached / 'hashes.bin')
    shutil.copyfile(out / 'original-samples.bin', cached / 'original-samples.bin')
    (cached / 'image.json').write_text(json.dumps(dict(
        image_bytes=view.image_bytes, block_count=view.block_count, image_sha256=complete.hexdigest(),
        hash_table_sha256=record['hash_table_sha256'], source_format=report['source_format'],
        sample_indices=indices, sample_lengths=lengths, verified_at=f'first build, {member}'), indent=2) + '\n')


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
    # The probe's compile flags come from the bsd_init.o record, which names EXPORT_HDRS/libsa; a Linux kernel build has no such
    # directory, and upstream clang treats a missing include directory as an error under -Werror.
    (ROOT / 'build/xnu/ios-obj/EXPORT_HDRS/libsa').mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', ROOT / 'scripts/build-verified-root-probe.py', '--component', 'os', '--include-systemos'],
                   cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
