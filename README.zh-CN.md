# UniXNU-dipper 中文说明

[English README](README.md) | [致谢与开源来源](CREDITS.md)

本仓库是 Xiaomi Mi 8（代号 `dipper`，Qualcomm Snapdragon 845）的
UniXNU 设备移植与真机启动链工程。项目从 SDM845 平台的
UniXNU-fajita 工程起步，逐步补齐 Mu-Silicium UEFI、DarwinBoot、XNU、
UFS/APFS、显示、HID、触摸和 GPU 资格检查。

## 当前状态

默认 `main` 分支保留早期稳定说明。当前真机开发与完整源码位于
[`diag/io-media-registry`](https://github.com/hanyaa380/UniXNU-dipper/tree/diag/io-media-registry)
分支，该分支已经完成：

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

## 主要目录

| 路径 | 用途 |
| --- | --- |
| `board.toml` | Mi 8 的镜像、内核参数、payload 和设备集成配置 |
| `package/` | 板级探针、BootProgress、HID、显示和按键支持 |
| `drivers/` | Mi 8 专用的 UFS、触摸、GPU 和 USB 调试驱动 |
| `patches/` | XNU、NativeHidInput 和其他内核补丁 |
| `scripts/` | UEFI、ADT、Data seed、内核和诊断镜像构建脚本 |
| `BUILDING-KERNEL.md` | 完整的内核与云构建教程 |
| `NOTES-touch.md` | Mi 8 ST FTS 触摸硬件和驱动说明 |

## 构建

简要流程：

```sh
cd ../UniXNU
./unixnu defconfig dipper_defconfig -D ../UniXNU-dipper
./unixnu fetch
./unixnu build package firmware --uefi ../Mu-Silicium
./unixnu bootimg --uefi ../Mu-Silicium \
  --dtb ../Mu-Silicium/Resources/DTBs/dipper.dtb \
  --ramdisk none
```

详细步骤见 [BUILDING-KERNEL.md](BUILDING-KERNEL.md)。

## 真机安全

- 操作前确认设备代号为 `dipper`；
- 刷写前备份 boot、recovery、GPT 和重要数据；
- 早期调试优先使用 `fastboot boot` 或独立测试分区；
- 不要把 fajita 的 GPT LBA 直接用于 dipper；
- UEFI/XNU 运行期间当前不保证充电，请保持电量并短时测试。

## 许可与二进制边界

本项目只发布源码、补丁、构建脚本和诊断工具，不包含 Apple
kernelcache、IPSW、ADT、iOS 根卷、私有密钥或用户数据。使用者必须从
自己合法取得的系统镜像中提取输入文件。

仓库、人员和社区参考资料见 [CREDITS.md](CREDITS.md)。
