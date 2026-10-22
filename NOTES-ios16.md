# iOS 16 for the iPhone X: planned phone userland

The board targets iPadOS 17.7.11 (iPad7,1) for bring-up. iOS 16.7.16 for the iPhone X is the planned userland once fajita boots. This note records why, and the work it needs.

## Why iOS 16

| Image | Device | Chip | Userland | fajita |
| --- | --- | --- | --- | --- |
| iPadOS 17.7.11 (21H461) | iPad7,1 | A10X, `0x8011` | `arm64` | runs; current target |
| iOS 16.7.16 (20H392) | iPhone10,6 | A11, `0x8015` | `arm64` | runs; planned |
| iOS 17 and later | iPhone11,2 and newer | A12 and newer | `arm64e` | does not run |

SDM845 (Cortex-A75 and A55) is ARMv8.2 and has no pointer authentication. `arm64e` code uses PAC instructions outside the hint space, such as `retab` and `braa`, which are undefined on ARMv8.2. iOS 17 dropped the iPhone 8 and iPhone X, so every iOS 17 iPhone image is `arm64e`, and iOS 16 on the A11 is the newest iPhone userland fajita can run.

The iPhone X panel is 1125x2436; fajita is 1080x2340. The aspect ratios are close, so the iPhone layout fits without the iPad width problem: at 2x scale the iPadOS image sees a 540-point-wide screen, narrower than any iPad.

## Precedent

[OnePlus 7 Pro (guacamole)](https://unixnu.com/devices/guacamole) reached iOS 16.7.16 userland on hardware. It uses the kernel and root file system of the ios-sweet work, with 16 KiB pages and HFS+, not the UniXNU source kernel and 4 KiB route.

## Work

1. **XNU source.** `feeds.toml` of UniXNU pins `xnu-10063.141.1` (iOS 17). iOS 16.7 needs the matching `xnu-8796` series tag, which the prelinked kexts of the 20H392 kernelcache are built against.
2. **Patches.** Rebase the UniXNU XNU patches onto that tag: the 4 KiB page patches, `non-apple-pmap-field`, the native timer, PAN and boot progress patches, and the late `zzy-` and `zzz-` patches.
3. **Drivers and kits.** Rebuild ArmDTPlatform, ArmCpu, NativeUfs, NativeAES and the other built-in drivers against the iOS 16 headers and recorded compile flags (`*.cpo.json`). corecrypto and libpthread archives must match the iOS 16 kernel ABI.
4. **Prelink.** New `kernelcache_sha256`, slide, `early_security_init` address and roots for the 20H392 kernelcache.
5. **Identity.** `ipad_identity` writes iPad properties; an iPhone identity (`iPhone10,6`, `d221ap`, chip `0x8015`) needs the matching ADT properties in the board script.
6. **Userdata and setup.** New `[userdata] windows`. Check whether 20H392 for the A11 has a `Cryptex1,SystemOS` component with `./unixnu ipsw info`; without one, the OS image alone. Update the build and version in the skip-setup seed (`docs/guide/skip-setup.md`).
7. **Trust caches.** The static trust cache of 20H392 and new `trust_cache_sha256` pins.

Look up the image before starting:

```sh
curl -s "https://api.ipsw.me/v4/device/iPhone10,6?type=ipsw"
```

Items 1 to 3 are stack-wide; they belong in UniXNU, not in this repository.
