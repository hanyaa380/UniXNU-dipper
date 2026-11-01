# 为其他 Qualcomm SoC 适配

先确认目标平台能否沿用当前后端，再填写资源配置。现有实现依赖 Kalama 风格 GCC、Qualcomm QSCRATCH、非安全 QSMMUv500/SMMUv2，以及固件准备好的 DEVICE 模式 USB。SM8550 的基址、SID 和复位方法都有来源记录；其他平台需要自己的记录。

## Agent 从哪里开始

先读模块根目录的 [AGENTS.md](../AGENTS.md)，然后读 [支持边界](boundaries.md) 和本页。按下面的入口查看实现，避免从历史设备文件名推断公共接口。

| 要判断的问题 | 入口 |
| --- | --- |
| 哪些资源可以通过配置改变 | [Sm8550.h](../soc/Sm8550.h)、[QcomUsbSoc.h](../include/QcomUsbSoc.h) |
| 固件状态是否满足接管条件 | [QcomUsbQualify.c](../soc/QcomUsbQualify.c) |
| stream、context bank、页表及恢复如何管理 | [QcomUsbDmaDomain.c](../soc/QcomUsbDmaDomain.c) |
| XNU 映射、缓存、计时及 CPU 约束 | [QcomXnuUsbDebug.c](../kernel/QcomXnuUsbDebug.c) |
| EP0、TRB、CDC 与收发队列 | [Dwc3.c](../usb/Dwc3.c) |
| XNU 输出和线程接入 | [XNU 版本配置](../scripts/xnu_patches.py) 及其补丁集 |
| 正常重启与显示来源 | [公共接口](../include/QcomXnuUsbDebug.h)、[截图接口](../include/QcomUsbScreenshot.h) |
| 现有检查覆盖到哪里 | [docs/usage-and-validation.md](usage-and-validation.md) |

[来源与证据](sources-and-evidence.md) 记录原件身份，Git 提交记录追踪当前源码。共享控制器使用 `dwc3_*`，高通 XNU 接口使用 `qcom_usb_debug_*`。迁移时应查看调用关系，避免只改名称后留下两套后端。来源设备的历史记录见 [sources-and-evidence.md](sources-and-evidence.md)。

## 先留下目标平台的资源记录

新增配置前，在目标设备或 SoC 文档中记录以下内容。每个硬件值都附 DTS/live DT、固件或锁定版本参考驱动的文件与位置；尚未确认的值保持待确认。

| 项目 | 需要确认的内容 |
| --- | --- |
| USB 控制器 | DWC3 基址、窗口、版本、DEVICE role、寄存器布局 |
| 供电与时钟 | GCC 基址、CBCR/BCR/GDSCR/CFG_GDSCR、父时钟与稳压器所有者 |
| PHY 与连接 | PHY/repeater 初始化、QSCRATCH、VBUS 策略、Type-C 路由 |
| DMA | SMMU 架构与非安全窗口、USB SID、地址位宽、可用 context bank/stream slot/ASID |
| 启动交接 | 固件何时停止 USB，旧 host/OTG 是否仍可 DMA，ExitBootServices 后哪些状态保留 |
| 复位 | PSCI conduit、SMC SYSTEM_RESET 是否已在该板验证、现有 PE 回调所有者 |
| OS 与构建 | XNU revision、目标架构、生成头文件、实际编译记录与内核归档接入 |
| 显示 | 扫描对象、CPU 映射、格式、stride、缓存同步及生命周期 |

同一 SoC 的不同设备也可能使用不同 PHY、供电和 Type-C 路由。Android Linux 驱动及 DTS 可用于核对资源和初始化依赖；其中的 Linux clock、regulator、PHY 和 gadget 框架调用需要由当前启动链或 XNU 平台实现承担。

## 确定改动放在哪一层

寄存器布局和初始化前提一致，只是基址、偏移、SID、IOVA 或描述符不同，可新增 `soc/<Name>.h`，通过构建器的 `--soc` 选择。保留现有 SM8550 配置供回归使用。尚未确认复位能力时，把新配置的 `QUD_PSCI_SMC_RESET` 设为 0，先验证日志与状态通道。

若时钟控制语义、PHY 初始化或 SMMU 架构发生变化，在 `soc/` 增加对应平台实现，再接入原生后端。特别是 SMMUv3，不能仅更换 SMMU 基址后调用现有 SMMUv2 代码。SoC 地址放在 `soc/`；板级供电和连接策略由消费者维护。跨平台变化不应散落到 CDC 描述符、TRB 算法或 XNU 控制台补丁中。

更换 XNU 时，在 `patches/` 增加独立版本目录，并在补丁工具中增加版本配置，核查该版本真实输出路径、初始化时序和线程 ABI。现有补丁锁定的 revision 应继续保留。[integration.md](integration.md) 给出了构建、应用、撤销和内核链接步骤。

## 已遇到的问题及处理方法

下列经验来自来源实现和本模块的软件验收。只有 [来源记录](sources-and-evidence.md) 中列出的轮次具有对应设备证据；新 SoC 仍需逐项验证。

### 控制器地址正确，访问仍可能失败

原生路径检查的是固件已就绪的供电、五个 USB 时钟、复位、DEVICE role 和 PHY 状态。分支时钟恢复助手没有实现父时钟、稳压器和 PHY/repeater 的完整冷启动。先确认电源和时钟窗口可访问，再接触 DWC3。对不可访问总线的读操作，软件预检查也可能遇到异步异常。

接管时要排除固件回调和仍在运行的 host/OTG。确认 device 侧 `DSTS.DEVCTRLHLT` 后，还需核对其他角色的 DMA 所有权。旧 DMA 未停止前，旧缓冲区必须继续有效。

### CPU 地址与设备地址混用

CPU VA 用于访问内存，PA 用于建立物理映射，TRB 和事件缓冲区使用设备可见 IOVA。原生实现分别检查 DMA arena 和页表每页首尾的物理连续性，并校验 SMMU 输入、物理输出和 DWC3 地址位宽。CPU 指针不能直接写入 TRB，PA 也可能高于 4GiB。

当前页表构造器使用三页页表，要求 2MiB 对齐的 32 位 IOVA，只映射八页自有 DMA。移植时检查新配置是否满足这些约束；需要扩大映射时同步修改构造器、窗口白名单、容量检查和测试。CPU 日志积压与截图快照不映射给 USB DMA。

### 把 SMMU 格式状态当成故障

此前排查遇到过 `FSR=0x400`：这是 AArch64 FORMAT 状态，本身不表示 fault。现有 `fsr_idle` 和 `fsr_a64` 分别检查 fault/reserved 位和格式，测试覆盖了相应状态。判断失败时保存原始读数与阶段，按位解释，避免用整寄存器非零作为故障条件，也不要清寄存器来绕过资格检查。

私有域的安装顺序包括选择空闲资源、写前重读、清页表缓存、配置、局部 TLB 同步及读回，最后提交 stream 有效位。恢复前需确认 DMA 已停止、租约仍属于本模块。停止或所有权不确定时保留租约和静态内存至复位。历史 identity/BYPASS qualifier 单独保留测试，原生后端没有这条失败降级路径。

### 源码中的汇编没有全部进入目标对象

历史 Darwin ARM64 内联汇编使用分号拼接，曾使计时或 DAIF 指令缺失。当前模板用换行分隔。修改后查看目标对象反汇编，确认计时、保存/恢复中断状态、缓存维护及屏障指令实际存在。编译成功无法检查这些语义。

计时当前使用 `CNTVCT` 配合 `CNTFRQ`。DMA 缓存维护和 MMIO 映射要与目标 XNU 一致；现有 TRB 布局拒绝大于 64 字节的缓存维护粒度。遇到拒绝时先核对缓存布局和隔离条件，再调整实现。

### 能收到 kprintf，却缺少其他输出

Darwin22 的 `printf` 和 `IOLog` 经过中央控制台路径。只接 `kprintf` 会漏掉它们；PE 回退又可能与中央挂钩重复输出。补丁在原控制台拒收前复制原始字节，同时覆盖 unbuffered、debug 和 panic 路径。超过单次 16KiB 的写入要分块，无换行消息也要持续刷新格式缓冲。

早期 kprintf 先经 CPU-only sink 缓存，初始化后再恢复常规锁与输出开关，避免早期空指针、CPU data 或 MMIO 访问。检查全部九个挂钩单元的宏和 include，并验证实际格式化路径及最终内核链接。现有 `tests/check_xnu.py` 检查八个 C 单元及冷入口指令片段的开关编译及 hook 解析；它不执行真实 XNU 格式化路径。

### 把早期日志写入和正常线程工作混在一起

早期及多 CPU 生产者可以提交 CPU 日志积压，不能依赖 CPU data 已就绪。硬件服务限制在启动 CPU。正常 worker 等调度器可用后才启动，绑定 `master_cpu` 并确认迁移完成；不要把它写死为 CPU0。

短临界区保存中断状态，争用或递归时及时返回并计数，内部不睡眠。正常 worker 每轮经调度器等待约 5ms。重启在独立正常线程执行，截图 provider 在 USB guard 外执行。panic 使用预分配内存和独立同步 drain，不能等 worker 继续运行。

### 有应答，却没有完成操作

控制应答入队后才提交事务序号和截图游标，回压时不消耗数据。主机不自动重试有副作用的命令，超时后先查询状态。`REBOOT state=queued` 表示请求已接收，随后还要排空日志、走正常 sync/unmount 并确认设备复位与恢复。已有 PE 回调不会被覆盖，未取得回调所有权时返回 `nohook`。

截图由显示驱动注册有效的 CPU 扫描映射或自定义 provider。模块固定整帧后再传输，Mac 校验会话、frame、偏移、长度及整帧 CRC 后写 PNG。固定映射可能被显示写入撕裂，动态扫描与缓存同步由 provider 处理。PNG 只证明读取了注册图像；验证面板可见画面、桌面和 Metal 执行仍需各自证据。

### 有日志文件，却无法确认完整性

4MiB CPU 积压和 16KiB 传输队列都有限。核对 pending、overflow、contention 及 transport loss；主机不读或长期供大于求都会丢字节。panic 最终 drain 预算约两秒，只有两级队列排空后才发送 `QCOM PANIC END`。采集器退出或文件不再增长不能代替这个检查。

## 为新配置补齐验证

1. 把目标 profile 加入 `tests/check.py` 的测试矩阵，检查目标基址和偏移确实被访问。现有默认矩阵只有 SM8550 和人工重定位配置，`scripts/build.py --soc` 不会自动扩展它。
2. 对新增寄存器语义补模型，覆盖写前状态变化、写失败、读回不一致、TLB 超时和恢复所有权改变。地址白名单需与配置一致。`tests/test_qualify.c` 含 SM8550 固定 SID/偏移夹具，当前只在 SM8550 下运行。
3. 按 `docs/usage-and-validation.md` 运行 ASan/UBSan、交叉编译、控制及 PNG 检查。用目标 XNU 的真实编译记录生成归档，再验证对应版本补丁及 hook。
4. 完整链接内核，保存源码 revision、profile、归档和镜像哈希。通过设备已有的临时启动与恢复流程，先验证固定日志和枚举，再扩展到正常 worker、压力、控制、重启及截图。
5. 保存原始 USB 采集、命令和恢复元数据。生成物放在 workspace 的 `tmp/` 或 `.cache/`；清理仅针对确认不再使用的可再生产物，原始采集及其元数据始终保留。

Agent 完成适配时，在设备仓库记录改动文件、资源依据、运行过的检查、对应报告，以及仍未验证的冷启动、休眠或显示能力。将模型通过、内核编译和设备结果分别列出，便于后续维护者判断下一步。

新增的完整日志接入、15 项专用主机测试、用户态构建及验收流程见 [logging.md](logging.md)。完整冷启动汇编仍需消费者平台前置补丁，指令片段编译不等于最终入口链接审计。
