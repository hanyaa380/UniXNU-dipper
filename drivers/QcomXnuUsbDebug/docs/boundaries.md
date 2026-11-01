# 支持边界与责任分工

## 通用化范围

SoC 资源放在配置头文件中，DWC3、私有 DMA 域、XNU 输出挂钩和主机控制分别维护。配置头文件适用于寄存器布局兼容的平台；布局变化时需要调整对应实现。当前功能范围为 CDC 调试，不包含完整 Linux USB gadget 子系统。

| 层 | 模块提供 | 消费者必须提供或验证 |
| --- | --- | --- |
| SoC 资源 | 显式基址、窗口、时钟偏移、SID、IOVA 配置与静态检查 | DTS/芯片资料、真实寄存器布局、资源可访问性 |
| 板级交接 | 就绪状态资格检查和受限接管 | 供电、父时钟、PHY/repeater、Type-C DEVICE 路由、退出固件与独占控制器 |
| USB | 无分配 DWC3 轮询、CDC ACM full/high speed | USB 线缆与主机、枚举、吞吐及断线恢复验收 |
| DMA | Qualcomm SMMUv2 私有 S1 域、精确 stream 租约及恢复 | 此实现匹配的 SMMU 布局、空闲 bank/slot/ASID、连续物理内存 |
| OS | Darwin22 版本化补丁、公共接口、worker/panic 接口 | 对应版本的构建、内建链接、启动参数与镜像 |
| 显示 | 一次注册 provider、固定 RGB 快照、分块传输和 PNG 工具 | CPU 映射、缓存同步、实际扫描对象、锁及生命周期 |
| 重启 | 命令排队、ACK drain、独立线程正常 `reboot_kernel` | 已确认 PSCI SMC reset 能力及正常系统关机链 |

SM8550 配置来自已有 socrates 实现。测试中的 relocated 配置仅证明地址/SID/偏移参数化，不证明另一款 SoC 可用。SMMUv3、不同 GCC/PHY 寄存器布局、SuperSpeed、完整冷启动及休眠恢复均需独立实现和验收。

## XNU 与硬件资源

补丁目前仅支持 补丁工具锁定的 XNU 8796.141.3 revision。升级 XNU 要添加独立 profile，核查真实调用路径和编译记录；模块接口不承诺跨版本内核 ABI。日志是额外出口，保留原图形/串口路径。

所有 CPU 可提交日志，只有启动 CPU 服务硬件；正常 worker 每次调度等待约 5ms。当前静态 CPU 积压 4MiB、传输队列 16KiB、DMA arena 32KiB、私有页表 12KiB，另有默认 16MiB 截图快照，须计入内核常驻内存预算。无主机读取、持续超量输出和生产者争用均可丢字节，状态计数用于识别损失。

原生后端只使用私有转换域，不自动降级 identity/BYPASS，不关闭全局 SMMU。地址检查无法防止不可访问总线的异步异常。停止、TLB 同步或所有权无法证明安全时，保留静态内存与租约直到复位；不能强行释放来继续运行。

## 双向控制、重启和截图

控制必须同时开启 `qcom-usb-debug=1 qcom-usb-ctl=1`。CRC32 检测传输损坏，session/sequence 防止会话串线和命令重放；它们不提供身份认证或加密。控制适用于明确开启的开发设备和可信物理 USB 主机。Mac 采集器单独拥有串口，控制客户端通过本地私有 Unix socket 通信。

重启仅在模块持有最终重启回调时接受。`QUD_PSCI_SMC_RESET=0` 或已有 `PE_halt_restart` 被其他实现占用时返回 `nohook`；不会覆盖已有回调。`queued` 只表示命令已接收，随后经过独立线程、日志排空及正常关机链，实际复位与恢复必须观察设备确认。命令不从 IRQ、panic 或 USB 锁内部执行。

截图默认没有来源，返回 `unavailable`。固定 framebuffer 适配器只接受显示驱动提供的已有 CPU 映射和明确格式，不接收主机传来的任意内存地址。映射/cookie 必须有效至复位；动态扫描缓冲、非一致内存或需要锁的驱动使用自定义 provider。provider 必须有界、无分配、复制完整 RGB888，返回前释放显示引用/锁，不能递归调用日志/USB。

快照上限默认 16MiB，每边不超过 4096，尺寸还须满足 `width*height*3` 上限。BGRA/RGBA 固定适配器丢弃 alpha，不处理旋转、HDR、压缩/tiled buffer 或合成器对象。并发显示写入可能撕裂；要求一致帧时由 provider 固定扫描源并同步。快照证明读取了注册的图像，不证明面板可见、桌面恢复、触控或 Metal shader 执行。

## 验收层级

模型与交叉编译证明算法及编译契约；归档审计证明模块符号；八个 XNU C 单元和冷入口指令片段的编译检查补丁接口。完整内核链接、真实枚举、日志、正常重启与真实扫描截图另行验收。独立模块尚未接入活动设备或刷机，现有检查没有新增硬件验证结论。来源设备的历史证据见 [sources-and-evidence.md](sources-and-evidence.md)。

## 日志覆盖

消息缓冲与早期挂钩只记录已到达这些入口的字节。可选用户态采集器经 root-only BSD sysctl 回传有限分页快照，不依赖 GPU。Darwin22 原生额度 ABI、20H392 私有系统源 ABI 与 SoC 配置分别维护；原厂权限和签名检查保留。固件进入内核前的日志、任意 stdout/stderr、已丢失/关闭/脱敏内容不在覆盖内。完整性和部署方法见 [logging.md](logging.md)。
