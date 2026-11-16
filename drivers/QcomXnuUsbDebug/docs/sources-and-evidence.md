# 来源与证据

记录日期：2026-10-26，Asia/Shanghai。下列硬件结果来自来源设备的历史记录，独立模块尚未重新做硬件验证。

模块内包含实现、SoC 配置、补丁和主机工具，提取前的文件哈希见本页末尾。独立构建及模型检查已通过；实机结果仍以各轮镜像和原始采集为准。

## Xiaomi Mi 8 / SDM845 目标资源

目标设备为标准版 Xiaomi Mi 8（`dipper`，6 GiB RAM、64 GiB UFS 2.1）。
以下 USB 资源只描述 Mi 8 的 primary USB controller：

| 项目 | 值 | 依据 |
| --- | --- | --- |
| DWC3 core | `0x0a600000`，`0xcd00` | Linux `arch/arm64/boot/dts/qcom/sdm845.dtsi` 的 `usb_1_dwc3` |
| QSCRATCH wrapper | `0x0a6f8800`，`0x400` | 同一 DTS 的 `usb_1`；`dwc3-qcom.c` 的 `SDM845_QSCRATCH_*` |
| QSCRATCH HS control | wrapper `+0x10`，VBUS bit20、SW session-valid bit28 | Linux `drivers/usb/dwc3/dwc3-qcom.c` |
| Apps SMMU | `0x15000000`，`0x80000`，stream 0x740 mask 0 | DTS `iommus = <&apps_smmu 0x740 0>` |
| USB30_PRIM BCR/GDSCR/CFG_GDSCR | `0xf000` / `0xf004` / `0xf008` | Linux `drivers/clk/qcom/gcc-sdm845.c` |
| USB clocks | core `0xf00c`, cfg_noc `0x502c`, aggre `0x8201c`, mock_utmi `0xf014`, sleep `0xf010` | 同一 GCC 驱动 |
| 实测继承状态 | DWC3、GCC、SMMU 可由当前 UEFI/XNU 交接读取；USB SMMU SID 0x740 | 本仓库 `drivers/NativeDwc3` 的只读探针 |

`Sm845.h` 把 QSCRATCH 从 `0x0a6f8000` 页基址映射 `0x1000` 字节，
因此 HS PHY control 的 profile 偏移是 `0x810`，实际物理地址仍为
`0x0a6f8810`。SDM845 的 USB30_PRIM GDSC 使用
`CFG_GDSCR.POWER_UP_COMPLETE`；本配置要求该位已由固件建立，不做冷启动
电源域或 PHY 初始化。PSCI reset 尚未在本板独立验收，故设置为 0。

## 会话与结构依据

- [按上游调整 UniXNU 结构](codex://threads/01a172fc-b622-7013-a5a2-775ebafd8bed)：会话要求保留 iPhone X 20H392 / Darwin22，按 ARM64 → SoC common → OS → 设备分层，共享驱动独立仓库，保护原始 USB 及恢复证据。记录时 GPU 批次仍在进行。
- [本地仓库规范](../../UniXNU/docs/contributing/repository.md)、[驱动接入规范](../../UniXNU/docs/porting/drivers.md)、[日志采集规范](../../UniXNU/docs/porting/log-capture.md)。上游网页入口为 [repository](https://unixnu.com/zh/contributing/repository)，整理时网页无法读取，结构规则采用本地文档及 workspace AGENTS.md。
- [UniXNU 组织仓库](https://github.com/orgs/UniXNU/repositories)：整理时查看的组织列表和本地 `components.toml` 中，未找到可直接复用的原生 USB 调试组件。组织列表有分页，发布仓库前仍需完整查重。

## 实现位置

原件链接相对本文件所在目录。当前模块接口见 README，下面保留提取前的实现位置。

| 内容 | 来源 |
| --- | --- |
| 总体链路与最初实机结果 | [direct-usb-logging.md](../../UniXNU-socrates/docs/direct-usb-logging.md) |
| XNU 初始化、缓存、worker 与输出路径说明 | [README-usb.md](../../UniXNU-socrates/kernel/README-usb.md) |
| 原生后端 / CPU 缓冲 | [SocratesUsbLog.c](../../UniXNU-socrates/kernel/SocratesUsbLog.c)、[SocratesUsbBuffer.h](../../UniXNU-socrates/kernel/SocratesUsbBuffer.h) |
| CDC 与 DWC3 | [SocratesDwc3.c](../../UniXNU-socrates/usb/SocratesDwc3.c)、[接口](../../UniXNU-socrates/usb/SocratesDwc3.h)、[说明](../../UniXNU-socrates/usb/README.md) |
| 平台检查与私有转换域 | [平台说明](../../UniXNU-socrates/usb-platform/README.md)、[translated-domain.md](../../UniXNU-socrates/usb-platform/translated-domain.md) |
| Darwin22 控制台与 worker 补丁 | [native USB patch](../../UniXNU-socrates/patches/xnu-ios16-native-usb.patch)、[worker patch](../../UniXNU-socrates/patches/xnu-ios16-usb-log-worker.patch) |
| 有限控制协议 | [SocratesUsbControl.h](../../UniXNU-socrates/kernel/SocratesUsbControl.h)、[主机客户端](../../UniXNU-socrates/scripts/xnu_usb_control.py) |
| 原始采集与控制桥 | [collect-xnu-usb.py](../../UniXNU-socrates/scripts/collect-xnu-usb.py) |
| 无串口时显式诊断 | [usb-diagnostics.md](../../UniXNU-socrates/kernel/usb-diagnostics.md) |
| 当前迁移/系统观察 | [format2 migration](../../UniXNU-socrates/docs/format2-migration.zh-CN.md)、[current blockers](../../UniXNU-socrates/docs/ios16-current-blockers.md) |

## 证据层级及时间差异

1. 最小 USB：历史文档记录独立 UEFI 探针收到 3909 字节，随后独立 XNU v13 收到 3803 字节，包含真实 root mount panic 和 `SOCRATES PANIC END`。这建立原生启动/panic 通道，不建立冷启动 PHY 或其他设备支持。详见 [usb-direct-progress.json](../../UniXNU-socrates/reports/usb-direct-progress.json)。
2. 缓冲与输出覆盖：2026-10-24 文档记录 4 MiB CPU backlog、主机不再隐式限制 16 MiB、13 项主机检查，以及 v58 至 v61 panic 汇总零 pending/loss 的具体轮次。有限缓冲仍可能丢弃，不能推广成永久零丢失保证。
3. 格式 2 后续运行：原会话及迁移记录使用 USB 完成多轮用户态观察、扫描帧传输和系统恢复。s27 正常卸载/重启证据见 [tmp/s27/shutdown-qualification.json](../../tmp/s27/shutdown-qualification.json)；该资格文件记录 USB 521381 字节、SHA256 `1fec2bc9e0fa62a2e5d3688c80e090ce87bb2a34621dda4b9cadaa1a789b2227`，14 项检查均为 true。资格文件和原始采集仍是后续复核依据。
4. 可见内容：s23 的 [scanout-3.png](../../tmp/s23/framebuffer/scanout-3.png) 是通过 USB 传输并校验的扫描缓冲主屏内容。它证明采样内容；物理面板、触控或 Metal 完成仍须其他证据。
5. 当时的支持边界：读取时的 current-blockers 记载 s42/s43 小负载 GPU 和正常重启，但完整 GPU 桌面、音频、调频、深度休眠未验收。USB 日志只承载观察，不能凭服务发布或一条成功消息代替对应硬件结果。

早期 USB 文档中的“没有根文件系统/用户态”描述特定 v13 轮次，不能覆盖后续 iOS16 进展。相反，后续运行成功也不能倒推最初镜像具有这些能力。旧平台 README 的 BYPASS 路径属于历史诊断，当前原生路径以私有 translated-domain 实现为准。当前 DWC3 头文件及代码已有 bulk OUT 接收能力，早期 README 的 OUT 丢弃描述已过时。

## 来源与许可证

DWC3 控制序列和控制传输参考 AsahiLinux/m1n1 `v1.4.17`，提交 `6b83e98e30657f7b98cad9b0a6949e6569f10044`；[现存 LICENSE.m1n1](../../UniXNU-socrates/usb/LICENSE.m1n1) 保留 MIT。`Dwc3Regs.h` 保留 Texas Instruments BSD 声明，参考目录说明采用 BSD 选项；不能将所有文件统一改标成新模块原创许可证。

平台资料锁定的 LineageOS SM8550 提交为 `093e3da978e7b954d041a261d40975957c7fe6f1`，来源清单见 [qualification-source.json](../../UniXNU-socrates/usb-platform/qualification-source.json) 和 [translated-domain-source.json](../../UniXNU-socrates/usb-platform/translated-domain-source.json)。复用的是 DWC3 设计与明确的寄存器事实，Apple DART/PMGR/PHY 不能代替高通实现。提取源码时逐文件保留出处、许可证和原有提交身份。

## 提取前文件身份

以下 SHA256 标识提取前原件。当前源码的改动由 Git 提交记录追踪。来源路径相对原工作区根目录，历史平台 JSON 原件仍保留在来源设备仓库。

| 当前文件 | 来源文件 | 原件 SHA256 |
| --- | --- | --- |
| `usb/Dwc3Regs.h` | `UniXNU-socrates/usb/Dwc3Regs.h` | `5e9a362cfa6385f55a793b48965963559fc2c1f167cab9d3afa3845603328e4b` |
| `usb/LICENSE.m1n1` | `UniXNU-socrates/usb/LICENSE.m1n1` | `11ca423a564b52d3b7f260398baa20d016c25ae180fbdd0067c27b7d1ed7b420` |
| `kernel/QcomXnuUsbDebug.c` | `UniXNU-socrates/kernel/SocratesUsbLog.c` | `de43c548c6989329f866c56a919ba9b8fd99bf480870feb8776d5d54b4169e0e` |
| `include/QcomXnuUsbDebug.h` | `UniXNU-socrates/kernel/SocratesUsbLog.h` | `bb56283f62898f12017ad05b4cad025408fc50ef570c03a8ad2ecfd8ece00397` |
| `kernel/QcomUsbBuffer.h` | `UniXNU-socrates/kernel/SocratesUsbBuffer.h` | `857ba276de22d2f1b297a72bd6db4c14afc1a1442a493640e0c0d666e27cabc4` |
| `kernel/QcomUsbControl.h` | `UniXNU-socrates/kernel/SocratesUsbControl.h` | `366cb431619c43288085c4904b6d741487ffc82f7d715f0ef156a4c00869c373` |
| `scripts/collect-xnu-usb.py` | `UniXNU-socrates/scripts/collect-xnu-usb.py` | `94c6d1b7acc8561d8738a393a248e5cceef199762e40d6e3516529f902058ba7` |
| `scripts/xnu_usb_control.py` | `UniXNU-socrates/scripts/xnu_usb_control.py` | `77b78cd19f5179712a6ce2fd37d61c7463b48b2cca04d8f23ebbea25a89687fd` |
| `patches/darwin22/0001-qcom-usb-debug-console.patch` | `UniXNU-socrates/patches/xnu-ios16-native-usb.patch` | `8eacd71948b99e45b9345b54b5dee7540c6f2e4a668db63266b589c81368ddcb` |
| `patches/darwin22/0002-qcom-usb-debug-worker.patch` | `UniXNU-socrates/patches/xnu-ios16-usb-log-worker.patch` | `bf54eab5510e0c3f780b0ae9c004ab6d0beeafde24bca1d7db44643dee7ac325` |
| `tests/test_dwc3.c` | `UniXNU-socrates/usb/test_dwc3.c` | `65d67994f5dcad5b3f4c1d4f8d5ceb9f026f35dc310d4caa140585747b82f570` |
| `tests/test_usb_buffer.c` | `UniXNU-socrates/kernel/test_usb_buffer.c` | `24640080b3d343909ed1c6cdff80db2a93bc18ac8afbdf5a9eb45af38533b423` |
| `tests/test_usb_control.c` | `UniXNU-socrates/kernel/test_usb_control.c` | `17ea9aa87fb44f6cac6bf39ef647bc1bf6668e02841fb6bb1cb934e83558df06` |
| `tests/test_domain.c` | `UniXNU-socrates/usb-platform/test_domain.c` | `2b45bfd6121d8445df173b73c809341b137c25bdc5bfb07522853f8a917ce263` |
| `tests/test_qualify.c` | `UniXNU-socrates/usb-platform/test_qualify.c` | `f9640a7bbe40c0f38b32a5c7eb96aee93d1ca9d742ca0a81890ecc16241c33a9` |
| `usb/Dwc3.c` | `UniXNU-socrates/usb/SocratesDwc3.c` | `3493a845f70d2bb6103dc289c10e136246c373d739e719902df2a353ed6857ed` |
| `usb/Dwc3.h` | `UniXNU-socrates/usb/SocratesDwc3.h` | `3124b2e7004e923a6650d77590693cae3be533b607da5b4ebd864123341a7349` |
| `soc/QcomUsbQualify.c` | `UniXNU-socrates/usb-platform/QcomUsbQualify.c` | `e7d0171fca4ee38eded7f6394b6a5f78285c3e93bb7cdd2cf34cb2885f48d6e7` |
| `soc/QcomUsbQualify.h` | `UniXNU-socrates/usb-platform/QcomUsbQualify.h` | `24669a7ae85cc942de6a4c8b92720c0e143b4d93547fd018512549893f6d61a0` |
| `soc/QcomUsbDmaDomain.c` | `UniXNU-socrates/usb-platform/QcomUsbDmaDomain.c` | `e665569b7d371e4d6884c1df86cc4b9537c536046d3e6efdcc37fc40432b8699` |
| `soc/QcomUsbDmaDomain.h` | `UniXNU-socrates/usb-platform/QcomUsbDmaDomain.h` | `b8cfbdadd3144cef187cc4488596f5b878b358252d5fa38a4587caea35ede229` |
| 历史记录原件 | `UniXNU-socrates/usb-platform/qualification-source.json` | `3307925ae0fedf31532a76ec81874f4011ae47e2ca9809dc9b2b019c7373b6ff` |
| 历史记录原件 | `UniXNU-socrates/usb-platform/translated-domain-source.json` | `424eaa22c631595425c3f640aa13ba2776d7a547c880aa0d71741e7cf8a92f46` |

## 最新日志实现的移入

本次对照会话“按上游调整 UniXNU 结构”的 s77/s78/s79 日志批次。来源设备的早期日志检查点为 `92c556b5854b893edaaf3e3f3ca2315450b84da1`；上游采集器源为 AdrenoMetal `88849ec7942124c0454127ca80fc67de8407714c`。保留其 MIT 通知于 `userland/LICENSE.NativeMetal`。

复用 CPU 队列、枚举分页、原生系统源与内存额度检查、解码和验收算法；将名称和帧前缀改为 Qcom/QCOM，并以独立 BSD sysctl 取代 GPU tunnel。来源 hash 是本次读取的原文件身份，不表示改名后的文件与之逐字相同。

| 来源工作区路径（相对 UniXNU-socrates） | 移入时 SHA256 |
| --- | --- |
| `kernel/SocratesUsbLog.c` | `905cfbec86a3ad5e86abc5c20d7813a116fecd95160150f42faa55a54c082857` |
| `kernel/SocratesUsbBuffer.h` | `4a55473f7b850e83e297e41ad5d2c47dca7d34f086c6c05477cadc12b9b752e7` |
| `kernel/SocratesEarlyLog.h` | `e3a4ad7418d22f4d39578cb174d7cd0d2c18515a060a422061037340130191fb` |
| `kernel/SocratesUserLogSink.cpp` | `fcaf43ce504637e228440ce8012188e214953c4e46749e438dcc5af5534a648c` |
| `include/SocratesUserLogWire.h` | `fada2695d56ef31fa88b2e2265345fe03a477ac11f32fbdfe1708e51f4f88169` |
| `userland/SocratesUserLogTransport.h` | `df9198ec8a551d69dce1f2e7ce001f0bb0cbc36e7510624bebb523fec1cc20a2` |
| `userland/SocratesIos16LogStore.h` | `18cd84a6ba48abf80a5593c930ff420cd123f2ad0e45296cab1148daa0a4f6cd` |
| `userland/SocratesLogMemory.h` | `8fd4061f564af46cfa70d718276043c0aac9054c4ae06cc088f3e3bc15925156` |
| `userland/SocratesOsLogPages.inc` | `922c15b9b6c83f163f4fd69f8975081cf1b84cf6d23b79045f776b2eb2dcc6d7` |
| `scripts/socrates_user_logs.py` | `ac9b3359c68904e3b4fa159a3a7176e89c5fddc75414d1a84d550f1f66fc16b0` |
| `scripts/decode-ios16-user-logs.py` | `690933a97199890b7ed900c2660a60d47c5ec3a38b7b1839138f554181e4535b` |
| `scripts/qualify-ios16-early-log.py` | `8ea995255932b9f17006fdf51d68b5b09ab02e993e89392802964a34e645f296` |
| `patches/xnu-ios16-usb-msgbuf.patch` | `51e5ca00f454c36ba652f65d419609027598170a21c816a1e5dfbf691cc81451` |
| `patches/xnu-ios16-early-log.patch` | `c2f2f70bae0eee1af63b00cdb33d239d922342e129710fa0664411dada9eb83b` |
| `drivers/AdrenoMetal/NativeOsLogExport.mm` | `fcfb808fd7ddf742f626dcddbc2733b0335727b6c9b607f287fe2e46e7d659e4` |

来源原始证据保留于 workspace `tmp/s77`、`tmp/s78`、`tmp/s79`，未复制进模块。s77 原始捕获 SHA256 `e75bf90efaf0bdc8a0accec6249ea43d8db711557a9f608f6736a1c093d0f811`，s78 为 `c165c028ff48338b99c287b19ced57d3c01bfeb6f5725f479d5084b09b68d042`，s79 为 `b24693f07d7b74a838bce1871518cadaa92576b57df7a79d979c4b5442b94863`。这些证据对应原设备代码；独立 sysctl 入口和模块最终内核另验。使用方法和边界见 [logging.md](logging.md)。
