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
| Payload, ADT, kernel | not started |
| Userdata | iPadOS 17.7.11 root image (8.74 GiB) built on Linux, block table CRCs checked, flashed (2026-10-21); not read by a kernel yet |

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

## Boot

```sh
fastboot boot out/fajita/boot/boot.img
```

`fastboot boot` is temporary and leaves Android and every partition unchanged.
