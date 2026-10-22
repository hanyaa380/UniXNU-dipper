#!/usr/bin/env python3
"""Build a wallpaper-only GPU addon for fajita: the default iPadOS 17 wallpaper with PNG images in place of its HEIC ones.

  make-fajita-wallpaper-addon.py [--edge PIXELS] OS_DMG OUTPUT

ImageIO cannot decode the default wallpaper's HEIC images without a hardware HEVC decoder, so PosterBoard's wallpaper stays black.
UniXNU's scripts/make-wallpaper-overlay.py (macOS: a mounted OS volume and sips) builds the PNG overlay, and make-gpu-addon.py --wallpaper
packs it, but only together with the Adreno Metal plugin, which fajita does not have. This script does both on Linux, without the plugin:

- reads the animation straight from the OS disk image of the IPSW (OS_DMG, 044-76040-038.dmg of 21H461, in the ipsw/21H461 directory
  of the unixnu output root) with 7-Zip (7z 21 or later reads DMG and APFS);
- decodes the two HEIC layers with libheif's heif-dec and scales them with ImageMagick to EDGE pixels square (default 2340, the long
  edge of the 6T panel; the layers stretch their contents to their bounds, sheng uses 2732 for its 12.9-inch class panel);
- rewrites main.caml to point at the PNG images, as make-wallpaper-overlay.py does;
- writes the NGPUADD2 file of make-gpu-addon.py with only wallpaper/* entries.

Boot with it: "unixnu build payload --gpu-addon OUTPUT" and the boot argument sheng-gpu-addon=1. The kernel
(patches/xnu/zzy-native-gpu-addon-mount.patch) writes the files to /private/var/ngpu/wallpaper on the Data volume and mounts them over
the sealed animation; its other two mounts (plugin and launchd job) find no source directory and only record that.
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ANIMATION = ('Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection/Wallpapers/7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper/'
             '7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca')
IMAGES = ('J120', 'J120-Dark')   # the iPad7,1 (J120) variants, the ones PosterBoard picks for fajita's iPad identity


def pad(data):
    return data + bytes(-len(data) % 8)


def tool(name):
    path = shutil.which(name)
    if not path:
        sys.exit(f'{name} not found (7z: 7-Zip 21+, heif-dec: libheif, magick: ImageMagick 7)')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--edge', type=int, default=2340, help='edge of the square PNG images in pixels (default 2340)')
    parser.add_argument('dmg', type=Path, help='the OS disk image of the IPSW (044-76040-038.dmg)')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        sys.exit(f'Refuse to overwrite {args.output}')
    if not args.dmg.is_file():
        sys.exit(f'{args.dmg} not found')
    if not 256 <= args.edge <= 3416:
        sys.exit('--edge takes 256 to 3416 (the HEIC images are 3416 pixels square)')
    sevenzip, heif, magick = tool('7z'), tool('heif-dec'), tool('magick')
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        subprocess.run([sevenzip, 'x', '-y', f'-o{work}', str(args.dmg), f'{ANIMATION}/*', '-r'], check=True, stdout=subprocess.DEVNULL)
        source = work / ANIMATION
        if not (source / 'main.caml').is_file():
            sys.exit(f'No wallpaper animation at {ANIMATION} in {args.dmg}')
        files = []
        for name in ('index.xml', 'assetManifest.caml'):
            files.append((name, (source / name).read_bytes()))
        main_caml = (source / 'main.caml').read_text()
        for image in IMAGES:
            main_caml = main_caml.replace(f'"/assets/{image}.HEIC"', f'"/{image}.png"').replace(f'"assets/{image}.HEIC"', f'"{image}.png"')
            decoded, scaled = work / f'{image}-full.png', work / f'{image}.png'
            subprocess.run([heif, str(source / 'assets' / f'{image}.HEIC'), str(decoded)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run([magick, str(decoded), '-resize', f'{args.edge}x{args.edge}', '-strip', f'PNG32:{scaled}'], check=True)
            files.append((f'{image}.png', scaled.read_bytes()))
        if '.HEIC"/>' in main_caml:
            sys.exit('main.caml still points at a HEIC image')
        files.append(('main.caml', main_caml.encode()))
    # The addon format of UniXNU scripts/make-gpu-addon.py: "NGPUADD2", u32 count, u32 0, u64 total bytes; per file u32 path bytes
    # (with its NUL), u32 mode, u64 size, u64 compressed size, the path padded to 8 bytes, the zlib stream padded to 8 bytes.
    body = b''
    for name, data in sorted(files):
        encoded = f'wallpaper/{name}'.encode() + b'\0'
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), 0o644, len(data), len(packed)) + pad(encoded) + pad(packed)
    blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    args.output.write_bytes(blob)
    for name, data in sorted(files):
        print(f'644 {len(data):>10} wallpaper/{name}')
    print(f'{args.output}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
