# UniXNU-dipper

Bring-up repository for the standard Xiaomi Mi 8 (`dipper`, Snapdragon 845).

## 中文说明

本仓库是 Xiaomi Mi 8（代号 `dipper`，Qualcomm Snapdragon 845）的
UniXNU 设备移植与真机启动链工程。项目从 SDM845 平台的
UniXNU-fajita 工程起步，逐步补齐 Mu-Silicium UEFI、DarwinBoot、XNU、
UFS/APFS、显示、HID、触摸以及 GPU 资格检查。

当前真机已经完成：

- Mu-Silicium UEFI、UniXNU 菜单和 DarwinBoot 启动链；
- 8 核 CPU、GIC、UFS、SMMU、APFS 和 Data 挂载；
- launchd、backboardd、SpringBoard 和 iOS 用户态启动；
- ST FTS 触摸，可操作 SpringBoard、初始引导和 Settings；
- CPU framebuffer 显示、Apple Logo、进度条和用户界面呈现；
- iOS 17.7.11（21H461）用户态下可进入 Settings。

当前仍在进行：

- Adreno 630 / GMU / HFI / SMMU 和 Metal 用户态 GPU 接入；
- 首次设置状态修复与持久化 Data 分区；
- Wi-Fi、蓝牙、音频、充电、相机等剩余外设。

本项目只发布源码、补丁、构建脚本和诊断工具，不包含 Apple
kernelcache、IPSW、ADT、iOS 根卷、私有密钥或用户数据。使用者必须从
自己合法取得的系统镜像中提取输入文件。

完整的仓库、人员及社区参考致谢见 [CREDITS.md](CREDITS.md)。

This repository starts from UniXNU-fajita because both devices use SDM845. The first milestone is deliberately
narrow: build a Mu-Silicium firmware volume containing UniXNU and DarwinBoot, boot it with `fastboot boot`, and
reach the UniXNU menu with an empty ramdisk.

从源码构建 XNU、生成自己的 prelink candidate，以及使用私有 CI 的完整步骤见
[BUILDING-KERNEL.md](BUILDING-KERNEL.md)。

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
| Real XNU kernel | Source kernel builds and prelinks; UFS, APFS, SystemOS/Data, launchd and userland boot on hardware |
| SMP | Eight logical CPUs have reached Running through PSCI/SMC |
| Display and userland transition | CPU framebuffer, Logo, progress bar, SpringBoard and Settings work |
| Touch | ST FTS works through NativeHidInput, IOHIDEventDriver and backboardd |
| GPU | A630 read-only qualification probe is integrated; GMU/Metal acceleration is in progress |
| Charging and other peripherals | Partial or not yet implemented |

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

1. Complete and validate the Adreno 630 clock, power, GMU/HFI, SMMU and
   command-submission backend.
2. Integrate the official UniXNU AdrenoMetal/Turnip path and render a
   known image before enabling the desktop renderer.
3. Resolve the first-setup `WFBuddyViewController` duplicate-push failure
   and move from the temporary 2 GiB Data seed to persistent storage.
4. Add charging, Wi-Fi, Bluetooth, audio, camera and other missing
   peripherals.

## Acknowledgements

This project builds directly on UniXNU, Mu-Silicium, the SDM845 fajita
bring-up, Xiaomi's released kernel sources and several community hardware
references. Repository-level and person-level credits are maintained in
[CREDITS.md](CREDITS.md).

The bring-up firmware currently uses a 32 MiB DarwinBoot window because the
dipper UEFI allocator stalls on a single 258 MiB allocation. A real XNU image
needs a larger window and therefore an allocator-side follow-up.

## Relation to fajita

The inherited scripts still contain `fajita-*` boot arguments and probe names. These are implementation identifiers,
not dipper hardware claims. They are kept initially so the platform-independent SDM845 code remains reproducible;
rename them only while adding and testing the corresponding dipper driver logic.
