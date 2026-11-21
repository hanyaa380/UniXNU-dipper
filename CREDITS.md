# 致谢与开源来源

本项目是在 UniXNU 社区和多个公开 SDM845、APFS、UEFI、触摸及 GPU 工程
基础上完成的。以下名单以本仓库的实际引用、Git 历史和公开资料为依据。

## 项目维护与贡献者

按本仓库 Git 历史统计：

| 人员 | 贡献 |
| --- | --- |
| [hanyaa380](https://github.com/hanyaa380) | 设备移植维护、启动链调试、UFS/APFS、显示、触摸、A630 资格检查、真机测试与集成 |
| [John Panos](https://github.com/johnpanos) | `UniXNU-fajita` 基础设备工程和 SDM845 UniXNU 启动基础 |
| [bluseliu50](https://github.com/bluseliu50) | 独立 `unixnudata` 数据分区、UFS 分区识别和原生滚动 `vc-text` 控制台 |
| [youzjuzi](https://github.com/youzjuzi) | 独立维护 `dipper-xnu`，提供 A630 GMU/HFI/SMMU、HWCG、CP Protect、UBWC、GMEM、零拷贝扫描输出和批量 GPU 页表映射的真机参考与验证数据 |
| [manysuq](https://github.com/manysuq) | 维护 HakiOS dipper 分支，提供 GMU/HFI/SMMU 分阶段实现、Adreno 用户客户端和 AIR/Metal 边界的独立交叉参考 |

同时感谢所有通过 issue、讨论、测试报告、日志分析和代码审查间接帮助
本项目的人。名单如有遗漏，欢迎通过 issue 或 pull request 补充。

## 核心项目

| 仓库 | 用途 |
| --- | --- |
| [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) | 非 Apple ARM64 设备启动与 XNU 用户态框架 |
| [johnpanos/UniXNU-fajita](https://github.com/johnpanos/UniXNU-fajita) | 本设备仓库的 SDM845 起点 |
| [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | Mi 8 的 AArch64 UEFI、DXE 和 BDS |
| [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) | XNU 内核源码和移植参考 |

## 硬件与驱动参考

| 仓库 | 用途 |
| --- | --- |
| [MiCode/Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) | Xiaomi Mi 8 `dipper-q-oss` 内核、ST FTS/FTM5、时钟、供电和 SoC 定义 |
| [youzjuzi/dipper-xnu](https://github.com/youzjuzi/dipper-xnu) | iOS 16.5 的 Mi 8 独立移植；A630 驱动、180 项初始化序列、105 项 HWCG、32 项 CP Protect、GMU/HFI/SMMU 与最新 GMEM/零拷贝性能修复参考 |
| [manysuq/hakios-xnu-kernel-dipper](https://github.com/manysuq/hakios-xnu-kernel-dipper) | HakiOS 的 dipper 设备目录及 Adreno/GMU/SMMU 分阶段实现；用于寄存器顺序、超时和 Metal 边界的独立交叉核对 |
| [UniXNU/NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay) | NativeLegacyDisplay 和 framebuffer 呈现 |
| [UniXNU/NativeUfs](https://github.com/UniXNU/NativeUfs) | UFS 与 SDM845 apps-SMMU 接入 |
| [UniXNU/ArmCpu](https://github.com/UniXNU/ArmCpu) | ARM CPU、GIC、SMP 和 timer 支持 |

## APFS、构建和恢复工具

| 仓库 | 用途 |
| --- | --- |
| [linux-apfs/apfsprogs](https://github.com/linux-apfs/apfsprogs) | Linux 下检查和处理 APFS Data seed |
| [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) | ADT 字段和早期 iOS 非 Apple 硬件启动参数参考 |
| [hanyaa380/UniXNU-build](https://github.com/hanyaa380/UniXNU-build) | 本项目 macOS 云端内核构建与 artifact 流程 |
| [hanyaa380/dipper-unixnu-bringup](https://github.com/hanyaa380/dipper-unixnu-bringup) | 真机进度、日志、镜像哈希和失败记录 |

## GPU 官方依赖与社区参考

| 仓库 | 用途 |
| --- | --- |
| [UniXNU/Adreno740](https://github.com/UniXNU/Adreno740) | 当前官方 Adreno 内核驱动框架、GMU、SMMU、ring 和用户客户端接口 |
| [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) | Metal → metal2vulkan 用户态前端 |
| [UniXNU/AdrenoPM4](https://github.com/UniXNU/AdrenoPM4) | PM4 命令编码 |
| [UniXNU/AdrenoFirmware](https://github.com/UniXNU/AdrenoFirmware) | GMU firmware 与 HFI 协议 |
| [UniXNU/ArmSmmu4K](https://github.com/UniXNU/ArmSmmu4K) | 4 KiB GPU/设备 SMMU 页表 |
| [UniXNU/AirNir](https://github.com/UniXNU/AirNir) | Apple AIR 到 NIR/SPIR-V 的离线编译链 |
| [HuajiFruit/UniXNU-sirius](https://github.com/HuajiFruit/UniXNU-sirius) | 无 Metal 时 CoreImage/OpenGL 回退崩溃的 CoreUI fixup 参考 |

## 文档与 AI 协作工具

| 工具 | 用途 |
| --- | --- |
| OpenAI Codex | 源码审查、补丁实现、日志分析、构建协调和中文文档整理 |
| DeepSeek | 部分早期中文进度文档整理与问题分析 |

AI 工具只用于代码与文档协作；硬件真机结果、代码审查和最终决定以仓库
提交及可复现日志为准。

## 许可与二进制边界

本致谢不能替代各上游项目的许可证和版权声明。引用源码时请同时遵守对应
仓库的许可证。本仓库不包含或分发 Apple kernelcache、IPSW、ADT、iOS
根卷、私有 CI 输入或用户数据。
