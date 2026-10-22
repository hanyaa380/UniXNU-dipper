#!/usr/bin/env python3
"""Build the owner-signed userland pieces of the SSH-over-USB channel for arm64 iOS:
the fajita-reboot tool (package/ssh-tools/fajita-reboot.c) and, with --dropbear DIR,
a static dropbear SSH server. Output (ignored, never committed):
build/ssh-userland/{fajita-reboot,dropbear,...} plus trust.json with their CDHashes,
which make-owner-trust-bundle.py adds to an owner trust module.

Signing and the iOS SDK
-----------------------
Linking an iOS userland binary needs an iOS SDK sysroot (libSystem et al.) and the
binary must be code-signed (ad-hoc, 4 KiB pages) for AMFI to let it run, exactly as
scripts/build-adreno-metal-plugin.py signs the Adreno probe daemon. This repository's
Linux host has neither an iOS SDK nor a Mach-O signer, so this script is written to
run where they exist:

  * macOS with Xcode: xcrun supplies the SDK (clang/ld64) and codesign.
  * Linux with an iOS SDK sysroot in UNIXNU_IOS_SDK and a signer (ldid/rcodesign):
    the cross toolchain at ~/unixnu-toolchain (clang + ld64) links against the SDK.

With neither, the script stops with a clear message instead of producing an
unsignable or unlinkable binary. It never downloads anything.
"""
import argparse
import json
import os
import plistlib
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/ssh-userland'


def find_sdk():
    sdk = os.environ.get('UNIXNU_IOS_SDK')
    if sdk and Path(sdk).is_dir():
        return Path(sdk)
    if shutil.which('xcrun'):
        try:
            return Path(subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip())
        except subprocess.CalledProcessError:
            pass
    return None


def find_signer():
    if shutil.which('codesign'):
        return 'codesign'
    for name in ('ldid', 'rcodesign'):
        if shutil.which(name):
            return name
    return None


def compiler(sdk):
    # macOS: xcrun clang. Linux: the cross clang of ~/unixnu-toolchain with the SDK.
    if shutil.which('xcrun') and not os.environ.get('UNIXNU_IOS_SDK'):
        return ['xcrun', 'clang']
    clang = os.environ.get('UNIXNU_XNU_CLANG', str(Path.home() / 'unixnu-toolchain/clang/bin/clang'))
    ld64 = os.environ.get('UNIXNU_LD64', str(Path.home() / 'unixnu-toolchain/ld64/bin/arm64-apple-darwin-ld'))
    return [clang, f'-fuse-ld={ld64}']


def sign(signer, path, identifier, entitlements=None):
    if signer == 'codesign':
        cmd = ['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', identifier]
        if entitlements:
            cmd += ['--entitlements', str(entitlements)]
        subprocess.run(cmd + [str(path)], check=True)
    elif signer == 'ldid':
        cmd = ['ldid', '-S' + (str(entitlements) if entitlements else ''), '-I' + identifier]
        subprocess.run(cmd + [str(path)], check=True)
    else:  # rcodesign
        subprocess.run(['rcodesign', 'sign', str(path)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dropbear', metavar='DIR', help='a dropbear source tree (untrusted; built, not run from)')
    args = parser.parse_args()

    sdk = find_sdk()
    signer = find_signer()
    if not sdk:
        raise SystemExit('No iOS SDK: set UNIXNU_IOS_SDK=/path/to/iPhoneOS.sdk or run on macOS with Xcode. '
                         'This Linux host has only the kernel stand-in SDK, which cannot link a userland binary.')
    if not signer:
        raise SystemExit('No Mach-O signer (codesign/ldid/rcodesign) found; AMFI will refuse an unsigned binary.')

    OUT.mkdir(parents=True, exist_ok=True)
    base = compiler(sdk) + ['-target', 'arm64-apple-ios17.0', '-isysroot', str(sdk), '-O2',
                            '-Wall', '-Wextra', '-Werror', '-fstack-protector-strong']
    cdhashes = {}

    # fajita-reboot: no entitlements needed (reboot(2) is allowed for root).
    reboot_bin = OUT / 'fajita-reboot'
    subprocess.run(base + [str(ROOT / 'package/ssh-tools/fajita-reboot.c'), '-o', str(reboot_bin)], check=True)
    sign(signer, reboot_bin, 'org.blu-sharky.fajita-reboot')
    cdhashes['fajita-reboot'] = cdhash(reboot_bin)

    if args.dropbear:
        src = Path(args.dropbear).resolve()
        if not src.is_dir():
            raise SystemExit(f'--dropbear: {src} is not a directory')
        # dropbear ships an autotools build; cross-configure against the SDK, then
        # build a single static multi-call binary (dropbearmulti: dropbear + dbclient
        # + dropbearkey + scp). Entitlements: a daemon outside every sandbox profile
        # needs no-container, like the Adreno probe daemon.
        ent = OUT / 'dropbear-entitlements.plist'
        ent.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True,
                                        'platform-application': True}))
        env = dict(os.environ, CC=' '.join(base), CFLAGS='-DHAVE_GETGROUPLIST')
        subprocess.run(['./configure', '--host=arm-apple-darwin', '--disable-zlib',
                        '--enable-static'], cwd=src, env=env, check=True)
        subprocess.run(['make', 'PROGRAMS=dropbear dbclient dropbearkey scp', 'MULTI=1', 'dropbearmulti'],
                       cwd=src, env=env, check=True)
        multi = OUT / 'dropbearmulti'
        shutil.copyfile(src / 'dropbearmulti', multi)
        sign(signer, multi, 'org.blu-sharky.dropbear', ent)
        cdhashes['dropbear'] = cdhash(multi)

    (OUT / 'trust.json').write_text(json.dumps(dict(cdhashes=cdhashes,
        purpose='owner-built SSH-over-USB userland; trusted only through an extra owner trust module'), indent=2) + '\n')
    print('PASS: SSH userland built and signed, CDHashes', cdhashes)


def cdhash(path):
    out = subprocess.run(['codesign', '-d', '--verbose=4', str(path)], capture_output=True, text=True).stderr \
        if shutil.which('codesign') else ''
    import re
    m = re.search(r'^CDHash=([a-f0-9]{40})$', out, re.M)
    return m.group(1) if m else 'unknown (compute with the signer in use)'


if __name__ == '__main__':
    main()
