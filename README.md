# UniXNU-fajita

Device repository for the OnePlus 6T (fajita, SDM845). It holds the board file, kernel defconfig and device scripts.

- [Documentation](https://unixnu.com)
- [Porting guide](https://unixnu.com/porting/)
- [Bring-up order](https://unixnu.com/porting/bring-up)

Build with the `unixnu` driver of [UniXNU](https://github.com/UniXNU/UniXNU).

## State

| Stage | State |
| --- | --- |
| UEFI firmware and UniXNU menu | works on hardware with `fastboot boot` and an empty ramdisk (2026-10-21); built on Linux |
| Payload, ADT, kernel | XNU 23.6.0 (iPadOS 17.7.11 kernelcache, built by the macOS workflow) boots to the root mount on hardware (2026-10-21): VM, scheduler, IOKit, AMFI, Sandbox and APFS come up and the verbose console ends on `Waiting on ... IOMedia ... Apple_HFS`. No storage driver yet. The console needs DarwinBoot to pass `v_display` 0 with `-v` (pending in DarwinBoot) |
| Kernel built on Linux | the fajita-linux branch of UniXNU builds the same kernel on Fedora (patched clang 21, Apple ld64 and cctools, Apple iig outputs from the macOS workflow): identical exported symbols (22563) and segment order; it boots on the phone to the same `Waiting on ... IOMedia` as the macOS build (2026-10-22) |
| Userdata | iPadOS 17.7.11 root image (8.74 GiB) built on Linux, block table CRCs checked, flashed (2026-10-21); not read by a kernel yet |

## Bring-up on the phone

- Keep `boot_a` and `boot_b` free of boot images. With an image on the active slot's boot partition the ABL refuses `fastboot boot` ("Load Error"). Every reset also spends a retry of the active slot, and at 0 the ABL refuses as well, so run `fastboot set_active b` before each `fastboot boot`. With empty boot partitions every reset lands in fastboot.
- The ABL also refuses large ramdisks, so the payload volume is flashed to `system_a` (2.86 GiB, unused with slot b active) and the boot image carries no ramdisk: `fastboot flash system_a out/fajita/payload/payload.img`, then `unixnu bootimg ... --ramdisk none`. UFSDxe, PartitionDxe and Fat expose the volume and UniXNU finds `\EFI\UniXNU\unixnu.toml` on it.
- Nothing charges the battery while UEFI or XNU runs.
- `scripts/patch-xnu-entry-probe.py --reset-after SYMBOL --in FUNCTION` puts a timed PSCI reset after a call in the kernel; a reset back to fastboot means the kernel got past it. This bisected the first boot.

## Firmware

There is no Mu-Silicium port for fajita. `scripts/make-fajita-uefi.py` derives one in a Mu-Silicium checkout from the enchilada (OnePlus 6) port:

| Item | enchilada | fajita |
| --- | --- | --- |
| Panel | `samsung,sofef00`, 1080x2280 | `samsung,s6e3fc2x01`, 1080x2340 |
| ABL board ID | `<8 0 17819 22>` | `<8 0 18801 41>` |
| Display driver | Qualcomm DisplayDxe (OnePlus 6 binary) | SimpleFbDxe on the ABL splash at `0x9d400000` |
| Vendor DXE binaries | `Binaries/enchilada` | `Binaries/enchilada`, same SoC |
| Memory map, FD base and size | `0x9fc00000`, `0x300000` | unchanged |

The 6 and 6T share `sdm845-oneplus-common.dtsi` in mainline Linux, including the reserved memory and the splash region. The charger and battery configuration in `Binaries/enchilada/RawFiles` is the OnePlus 6 one; check it before charging under UEFI.

```sh
git clone --recurse-submodules https://github.com/Project-Silicium/Mu-Silicium ../Mu-Silicium
python3 scripts/make-fajita-uefi.py ../Mu-Silicium
dtc -I dts -O dtb -o ../Mu-Silicium/Resources/DTBs/fajita.dtb ../Mu-Silicium/Resources/DTBs/fajita.dts
```

The script writes `Platforms/OnePlus/fajitaPkg`, `Resources/Configs/fajita.toml` and `Resources/DTBs/fajita.dts`, and compiles the device tree when `dtc` is on `PATH`. It adds UniXNU, UniXNUDxe and DarwinBoot to the DSC and FDF, as in uefi-sheng. It also patches the shared `BootShim/BootShim.S` with the uefi-sheng fixes: the ARM64 header size covers the shim and the firmware volume, which `unixnu bootimg` checks, and the copied firmware volume is cleaned and the instruction cache invalidated before the jump.

## Stock boot image

`scripts/fetch-fajita-boot.py --info` reads the boot partition of OxygenOS 11.1.2.2 (34.J.62) from the OnePlus full OTA with range requests, about 64 MiB of the 2.1 GiB zip, checks it against the payload manifest and prints its header. `[bootimg.image]` in `board.toml` holds those values: header version 1, page size 4096, base 0, kernel offset `0x8000`, ramdisk offset `0x1000000`, tags offset `0x100`. The image goes to `.cache/firmware/fajita/boot.img`, which Git ignores. On a phone running 11.1.2.2 it is the stock boot partition, so it can restore one; `fastboot boot` of the UniXNU image never writes it.

## Build

Run these commands from the UniXNU checkout. The device repository is `../UniXNU-fajita`.

```sh
./unixnu defconfig fajita_defconfig -D ../UniXNU-fajita
./unixnu fetch
./unixnu build package firmware --uefi ../Mu-Silicium
./unixnu bootimg --uefi ../Mu-Silicium --dtb ../Mu-Silicium/Resources/DTBs/fajita.dtb
```

## Data volume seed

The writable Data volume (`/private/var`) comes from a RAM disk seed made to `data-seed.json`: one APFS container at offset 0 holding one case-sensitive, unencrypted volume named Data with no role, plus the listed directories with their owners and modes. On Linux, `scripts/make-data-seed-linux.py` builds it without root, FUSE or a kernel module. mkapfs from [apfsprogs](https://github.com/linux-apfs/apfsprogs) formats the image. The script then writes the directories into the volume's catalog itself and checks the result with apfsck and its own read-back. Last, it packs the image with UniXNU's `packed_ramdisk.py`. `--fetch-apfsprogs` clones a pinned apfsprogs commit to `.cache/apfsprogs` and builds it there; `--apfsprogs DIR` uses a build tree you already have.

```sh
python3 -I scripts/make-data-seed-linux.py \
  --packer ../UniXNU/drivers/DarwinBoot/Test/packed_ramdisk.py --fetch-apfsprogs \
  --volume-uuid 0E1D5C2A-7B3F-4C88-9A61-2F4D8E0B6C17 --container-uuid 5A9C3E71-D204-4B6F-8E15-C7A0B9F24D38 \
  --timestamp 1792540800
```

The script writes `.cache/data-seed/data-linux.dbr` and `data-linux.json`; `--keep-raw` keeps `data-linux.raw`. Without the UUID options it picks random UUIDs. Either way it prints the volume UUID, which goes to `CONFIG_XNU_OWNER_DATA_VOLUME_UUID`. With fixed UUIDs, a fixed `--timestamp` (or `SOURCE_DATE_EPOCH`) and the same apfsprogs commit, the image is bit-identical on every run. `--apfs-dump DIR` (a build directory with apfs-fuse's `apfsutil` and `apfs-dump-quick`) adds a read-back by a second APFS implementation. `scripts/make-data-seed-macos.py` is the macOS equivalent.

## Boot

```sh
fastboot boot out/fajita/boot/boot.img
```

`fastboot boot` is temporary and leaves Android and every partition unchanged.
