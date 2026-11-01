# 架构与移植方法

## 源码目录

| 目录 | 内容 |
| --- | --- |
| `include/` | 公共 XNU、截图和 SoC 配置接口 |
| `soc/` | Qualcomm 状态检查、私有 DMA 域、具名 SoC 配置及来源记录 |
| `usb/` | 跨平台 DWC3/CDC、寄存器定义及许可证 |
| `kernel/` | XNU 映射、缓存、日志缓冲、控制和截图实现 |
| `patches/` | 按 XNU 版本维护的接入补丁 |
| `scripts/` | 构建、补丁管理和 Mac 运行工具 |
| `tests/` | C/Python 测试、检查入口和夹具 |
| `docs/` | 接入、平台适配和历史证据 |

`usb/` 通过回调访问控制器和 DMA，不引用 SoC 配置或 XNU 头文件。Qualcomm 状态检查与 DMA 域位于 `soc/`，其中 `Sm8550.h` 记录该 SoC 的资源值；XNU 映射、缓存和线程接入留在 `kernel/`。

## 实现分层

| 层 | 应承担的工作 | 现有实现参考 |
| --- | --- | --- |
| 通用传输 | DWC3 EP0、CDC 描述符、端点、TRB、事件、收发队列 | `usb/Dwc3.c/.h`、`Dwc3Regs.h` |
| Qualcomm 平台 | 电源/时钟资格检查、PHY 状态、QSCRATCH、私有 SMMU 域与资源所有权 | `soc/QcomUsbQualify.*`、`QcomUsbDmaDomain.*` |
| XNU/OS | MMIO 映射、物理地址、缓存同步、CPU 约束、控制台挂钩、调度和 panic | `kernel/QcomXnuUsbDebug.c`、`QcomUsbBuffer.h`、OS 对应补丁 |
| 设备与主机 | DTS 资源、启动交接、serial/VID/PID、启用策略；主机发现、采集和控制 | `soc/*.h`、消费者设备配方、`collect-xnu-usb.py`、`xnu_usb_control.py` |

代码路径相对模块根目录。`usb/Dwc3` 为共享控制器实现；高通平台代码和配置在 `soc/`，XNU 补丁与运行参数使用 Qualcomm 通用命名。显示驱动通过公共 provider 接口注册截图来源，USB 不定位显存。

主机工具可随组件发布。所有寄存器常量、流 ID、复位方法、固件状态、PMIC 和 Type-C 路由都应有对应设备/SoC 的来源；相同 SoC 不代表板级 PHY、供电和连接策略相同。

## 控制器交接

从目标 DTS、live DT、固件与锁定版本的参考驱动确定 DWC3、GCC、SMMU、时钟、复位和供电所有者。先读已知可访问的电源/时钟窗口，再接触 DWC3；软件读前检查不能兜住异步总线异常。

原生路径检查并继承已经就绪的供电、五个 USB 时钟、复位、DEVICE role 与 PHY 状态。可选的 branch gate 恢复助手只处理对应分支；父时钟、稳压器、PHY/eUSB2 repeater 和冷启动初始化仍由启动链负责。

接管前排除仍在运行的 host/OTG 和固件回调，停止旧控制器并确认 `DSTS.DEVCTRLHLT`。只看到 device 侧停止，不足以证明另一个角色没有 DMA。继承的缓冲区在旧 DMA 真正停止前仍须有效。

## 地址与 DMA

以下数值来自 SM8550/socrates。其他 SoC 需按其硬件资料单独配置。

| 资源 | 当前参考 |
| --- | --- |
| GCC / DWC3 / apps SMMU 基址 | `0x00100000` / `0x0a600000` / `0x15000000` |
| USB stream | SID `0x40` |
| 自有 DMA arena | 32 KiB、4 KiB 对齐、物理连续 |
| 自有页表 | 三页，每页 4 KiB，物理连续 |
| 设备可见 IOVA | `0x90000000..0x90007fff` |
| CPU 日志积压 | 4 MiB，不映射给 USB DMA |

通过真实 XNU 地址转换检查 DMA 和页表每页首尾的物理连续性；分别核对 DWC3 的 IOVA 位宽、SMMU 输入及物理输出能力。PA 可以高于 4 GiB，不能把 CPU 指针直接写进 TRB。

当前运行路径使用 `QcomUsbDmaDomain` 私有 S1 转换域：仅映射八页自有 DMA，其他叶子无效，页项不可执行。必须找到未被使用、无冲突的 stream slot/context bank 和 ASID；页表清缓存、配置、局部 TLB 同步及读回完成之后，最后提交 stream 有效位。

历史 identity/BYPASS qualifier 是独立诊断代码，不能作为当前私有转换域失败后的自动降级方案；不能关闭全局 SMMU 或改动其他设备的 stream。

非一致性 DMA 要正确 clean/invalidate，并执行屏障；现有 TRB 布局拒绝大于 64 字节的缓存维护粒度。MMIO 映射为设备内存，64 位 TTBR 使用对齐、完整访问。写回调按地址、阶段和值白名单限制副作用。

## CDC 与 XNU 接入

通用 DWC3 core 采用无分配轮询模型、一个 CDC ACM 功能，当前参考端点为 notification IN `0x81`、bulk OUT `0x02`、bulk IN `0x82`。提供 full/high-speed 描述符，主动禁用 SuperSpeed，降低首轮 PHY 和链路范围。

先在独立 UEFI 候选接收固定测试行，再验证 ExitBootServices 与 MMU 切换后的内存/控制器所有权。XNU 阶段通过原生映射、缓存和计时回调重建自己的上下文。保留图形控制台，将 USB 作为额外日志出口。

当前默认 serial 是 `QcomXnuUsbDebug`。参考 VID/PID `1209:316d` 沿用 m1n1 的隔离开发测试标识；发布组件/设备前应落实适当产品标识，不能把它当作新模块已获分配的 ID。

## 输出路径

只接 `kprintf` 不足以收全 `printf` 和 `IOLog`。Darwin22 的两者通过 `console_printbuf_* → console_write`；需要在原控制台拒收之前 tee 原始字节，并覆盖 `console_write_unbuffered`、PE/debug 回退及 panic 路径。

大于单次 16 KiB 上限的写入须分块；无换行长消息须持续刷新格式缓冲。PE 回退到中央控制台时去除重复 tee，保留原有图形/串口行为。验证输出覆盖时，还需测试实际格式化函数的行为。

当前 opt-in 为 `qcom-usb-debug=1`。详细 kprintf 开关须在输出 sink 就绪后才打开，避免早期空指针。编译宏与 include 要覆盖 PE/debug、格式输出、`serial_console.o`、`serial_general.o` 和 BSD worker；新 OS 版本应按其真实源码及编译记录重新核对。

## 日志生产与 USB 服务

所有 CPU 可以向 4 MiB CPU 缓冲提交已挂钩字节；通过非自旋、保存中断状态的短临界区保护，冲突/递归时及时返回并计数。早期生产者不能依赖 CPU data 就绪或访问 MMIO。

只有启动 CPU 服务控制器；worker 在调度器可用后启动一次，绑定 `master_cpu` 对应 processor 并验证迁移完成，不能假设它永远是 CPU0。当前每 5 ms 经调度器等待后轮询，临界区内不睡眠。每次服务将有限积压移入 16 KiB 传输队列，并处理有限事件。IN 完成后才释放对应传输字节。

pending/high-water、overflow、contention 和 transport loss 等计数分别记录积压与损失。主机不读、断线、轮询停止或长期供大于求都会使有限队列丢失数据，因此输出挂钩的覆盖范围与日志是否完整需要分别检查。

## panic 与失败恢复

panic 使用预分配内存和同步轮询，不依赖调度线程继续运行。当前最终 drain 预算约两秒，单个有界硬件操作可能延长它。只有两级队列真正排空才发送 `QCOM PANIC END`；缺少标记表示未证明完成，不能凭采集器退出推断日志完整。

停止/失败恢复必须先证明 DMA 停止，才能撤销 stream、禁用私有域、同步 TLB、恢复借用寄存器和释放内存。所有权改变、域故障、TLB 超时或停止状态不确定时，保留租约及静态内存至复位。不要清故障寄存器来使检查通过。

SMMU FSR 同时包含 FORMAT 状态和 fault 位：AArch64 `0x400` 本身不等于故障。Darwin ARM64 内联汇编的多条指令使用换行分隔；历史分号模板曾导致计时/DAIF 指令缺失，必须检查目标对象实际指令。

深度休眠前后的 USB 停止、资源恢复和原会话续接要单独设计并实测；当前常开 USB 调试成功不建立休眠恢复能力。

## 共享 DWC3 调用契约

`dwc3_platform` 包含寄存器 offset 读写、延迟、DMA 同步、屏障和可选 trace 回调。回调通过 cookie 获得所属平台状态。初始化前，调用方需完成供电、时钟、PHY、DEVICE role、MMIO 映射和 DMA 域建立，并取得控制器独占权。

提供至少 `DWC3_CONTEXT_BYTES` 字节、按 `DWC3_CONTEXT_ALIGNMENT` 对齐的上下文，以及至少 `DWC3_DMA_BYTES` 字节、按 `DWC3_DMA_ALIGNMENT` 对齐的连续 DMA 映射。CPU 地址与设备 DMA 地址分别传入，它们可以不同。上下文和映射需保持有效，直到 `dwc3_stop()` 成功；停止失败时继续保留资源。

```c
#include "Dwc3.h"

/* io 由调用方填入 MMIO、延迟、DMA 同步和屏障回调。 */
struct dwc3_identity identity = {
    .vid = product_vid, .pid = product_pid,
    .manufacturer = manufacturer_name,
    .product = product_name, .serial = device_serial,
};
int result = dwc3_init(context, context_bytes, &io, &identity,
                       dma_cpu, dma_device_address, dma_bytes);
```

VID/PID 必须非零，每个字符串为 1 至 31 个可打印 ASCII 字符。初始化在 MMIO 操作前验证标识，并将其复制为实例自己的描述符；返回后不再保留字符串指针。不同实例可以使用不同标识，查看描述符时需向 `dwc3_descriptor()` 传入所属实例。实例独立存储不代表可以并发操作同一个控制器或 DMA arena。

初始化后调用 `dwc3_start()`，由所属 CPU 定期执行有界 `dwc3_poll()`。`dwc3_queue()` 和 `dwc3_read()` 提供非阻塞收发，调用方负责与 poll 串行化。断线或总线复位会清空接收缓冲并递增 generation，上层协议据此废弃旧会话。

### 共享核心范围与验证

当前描述符提供 full/high speed，禁用 SuperSpeed；实现不负责 PHY 冷启动、板级电源策略或休眠恢复。缓存同步回调必须覆盖非一致 DMA 的 clean/invalidate 和屏障要求，调用方还需验证映射与 TRB 缓存行隔离。

[tests/test_dwc3.c](../tests/test_dwc3.c) 在没有 `QUD_SOC_HEADER` 的情况下运行，检查描述符、不同标识的实例、字符串复制、非法标识的 MMIO 前拒绝及控制传输。`tests/check.py` 还以相同条件编译 ARM64 ELF/COFF 对象，并核查无外部运行库导入。高通平台和 XNU 接口另有各自测试。

代码派生自 Asahi Linux m1n1，许可证见 [LICENSE.m1n1](../usb/LICENSE.m1n1)；`Dwc3Regs.h` 保留原有 BSD 声明。

## 扩展日志链路

`include/QcomEarlyLog.h` 与 Darwin22 入口/初始化补丁记录十五个早期阶段，初始化前只写 CPU backlog。`kernel/QcomUsbUserLog.c` 是可选 BSD sysctl 入口，按 `include/QcomUserLogWire.h` 校验并原子入队；`userland/` 保存独立 root 采集器、分页与 OS 版本适配。主机的 decode/qualify 脚本校验传输和源覆盖。它们均不依赖 `soc/` 地址或显示/GPU 驱动，具体边界见 [logging.md](logging.md)。
