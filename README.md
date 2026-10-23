# UniXNU-dipper

Bring-up repository for the standard Xiaomi Mi 8 (`dipper`, Snapdragon 845).

This repository starts from UniXNU-fajita because both devices use SDM845. The first milestone is deliberately
narrow: build a Mu-Silicium firmware volume containing UniXNU and DarwinBoot, boot it with `fastboot boot`, and
reach the UniXNU menu with an empty ramdisk.

## State

| Stage | State |
| --- | --- |
| Device repository | Scaffolded from fajita |
| Mu-Silicium dipper package | Generated from `equuleusPkg` by `scripts/make-dipper-uefi.py` |
| UEFI firmware | Builds successfully on Linux with CLANGPDB |
| Boot image | Generated, round-trip verified and accepted by dipper ABL |
| UniXNU menu | Works on dipper hardware with an empty ramdisk |
| Payload FAT | Works from the existing FAT `logfs` partition on hardware |
| EFI payload | `HelloPayload.efi` runs and returns through UniXNU on hardware |
| DarwinBoot | Synthetic ARM64 Mach-O/ADT handoff reaches EL1 and paints the framebuffer green |
| Real XNU kernel | Public fajita kernel reaches BSD init; the stock artifact has no `NativeUfs`, so `_IOFindBSDRoot` waits forever for a root device |
| UFS root, touch, charging, GPU | Not implemented or verified for dipper |

The standard Mi 8 is `dipper`. The screen-fingerprint/UD model is normally `equuleus`; this repository targets
`dipper`, although it uses equuleus vendor DXE binaries as the closest available SDM845 donor set.

## Firmware

Generate the Mu-Silicium package:

```sh
python3 scripts/make-dipper-uefi.py ../Mu-Silicium
```

The generator:

- copies `Platforms/Xiaomi/equuleusPkg` to `Platforms/Xiaomi/dipperPkg`;
- keeps the equuleus Xiaomi SDM845 vendor binaries and raw files;
- selects `SimpleFbDxe` instead of the equuleus panel-specific display driver;
- adds UniXNU, UniXNUDxe, DarwinBoot and DipperBootMemoryDxe;
- patches the shared SEC and BootShim for DTB handoff and cache-coherent firmware entry;
- creates `Resources/Configs/dipper.toml` and `Resources/DTBs/dipper.dts`.

Only one dipper-specific static DTB source is generated at a time. At runtime the ABL passes the phone's own DT in
`x0`; the appended DTB is a fallback and a boot-image construction requirement.

## First build

The base UniXNU repository still needs its submodules initialized. A device build also needs Mu-Silicium
submodules, EDK II and the host build tools.

```sh
cd ../UniXNU
./unixnu defconfig dipper_defconfig -D ../UniXNU-dipper
./unixnu fetch
./unixnu build package firmware --uefi ../Mu-Silicium
./unixnu bootimg --uefi ../Mu-Silicium \
  --dtb ../Mu-Silicium/Resources/DTBs/dipper.dtb \
  --ramdisk none
```

Then start the image without writing a partition:

```sh
fastboot devices
fastboot boot out/dipper/boot/boot.img
```

Success for this milestone means visible display output or serial `UNIXNU:` lines and the UniXNU menu.

The current build produced:

```text
../Mu-Silicium/Build/dipperPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
out/dipper/boot/boot.img
```

The verification image report initially records `hardware_tested: false`; its successful on-device boot is recorded
here because the build driver cannot observe the panel or absence of a UART.

## Hardware safety

- Confirm `adb shell getprop ro.product.device` prints `dipper`.
- Back up the stock boot image, recovery, GPT and any data that matters before experimenting.
- Use `fastboot boot`; do not flash the firmware to a partition during menu bring-up.
- Never reuse fajita's GPT LBA values on dipper.
- Do not assume the equuleus PMIC, battery, panel or touch configuration is correct for dipper.
- Nothing in this repository charges the battery while UEFI or XNU is running. Keep the phone charged and test
  in short sessions.

During bring-up the existing `cache` partition was backed up and formatted as FAT, then its GPT type was changed
to EFI System Partition. The FAT `logfs` partition later proved to be the usable payload volume. Host backups from
this device are kept outside both repositories under `/root/mi8uefi/backups/`; restore them before returning the
phone to a normal Android configuration.

## Next stages

1. Boot the UEFI firmware and UniXNU menu on hardware.
2. Confirm `DipperBootMemoryDxe`, FAT payload parsing and EFI payload startup.
3. Confirm DarwinBoot's Mach-O, ADT, boot-services exit and EL1 handoff. Complete.
4. Build and prelink an SDM845 XNU kernel containing `NativeUfs`, `NativeLegacyDisplay` and boot progress.
5. Measure dipper's `userdata` GPT geometry and DRAM size, then build and validate the target userdata.
6. Verify dipper's UFS/SMMU, display, touch and power paths on hardware.
7. Validate Data, launchd and userland before enabling optional services.

The bring-up firmware currently uses a 32 MiB DarwinBoot window because the
dipper UEFI allocator stalls on a single 258 MiB allocation. A real XNU image
needs a larger window and therefore an allocator-side follow-up.

## Relation to fajita

The inherited scripts still contain `fajita-*` boot arguments and probe names. These are implementation identifiers,
not dipper hardware claims. They are kept initially so the platform-independent SDM845 code remains reproducible;
rename them only while adding and testing the corresponding dipper driver logic.
