# QcomXnuUsbDebug

QcomXnuUsbDebug 为高通 ARM64 平台的 XNU 提供 USB 日志、Mac 双向控制、正常重启请求和按需截图。日志通过 DWC3 CDC ACM 传给主机，启动 Android 或挂载根文件系统之前也可使用，无需 ADB。

当前模块包含固件已准备好的 SM8550 配置，以及 Xiaomi Mi 8（SDM845 /
`dipper`）的 Darwin23 接入配置。SDM845 使用独立 QSCRATCH 页映射、
USB30_PRIM GCC 资源和 apps SMMU SID 0x740；PSCI reset 尚未验收并保持关闭。
模块上游的 Darwin22 补丁保留在 `patches/darwin22`，dipper 仓库通过
Darwin23 设备补丁接入 `qcom-usb-debug=1`。

| 入口 | 用途 |
| --- | --- |
| [源码目录与架构](docs/architecture.md) | 文件职责、控制器交接、DMA 和输出路径 |
| [完整接入步骤](docs/integration.md) | 准备内核、检查/应用补丁、构建链接、启用、采集及回退 |
| [职责和支持边界](docs/boundaries.md) | SoC / 设备 / XNU / 主机 / 显示驱动分工及限制 |
| [内核与用户态日志](docs/logging.md) | 早期补发、消息缓冲、独立采集器、分页解码和完整性验收 |
| [双向控制与截图](docs/control-and-screenshot.md) | 命令、协议、显示 provider、PNG 与错误处理 |
| [使用和验收](docs/usage-and-validation.md) | 主机操作、检查命令、成功标准与排障 |
| [平台配置](soc/Sm8550.h) | MMIO、时钟、SID、IOVA、USB 标识及复位能力 |
| [SDM845 配置](soc/Sm845.h) | Xiaomi Mi 8 QSCRATCH、USB30_PRIM、SID 0x740 与独立 IOVA |
| [公共 API](include/QcomXnuUsbDebug.h) | 初始化、写入、轮询、panic、状态与控制接口 |
| [补丁集](patches/darwin22) | 控制台 tee 与正常线程轮询；只调用通用 API |
| [其他 SoC 适配](docs/porting-other-socs.md) | agent 读取顺序、适配步骤和已遇到的问题 |
| [测试](docs/usage-and-validation.md) | 测试源码、检查入口与运行条件 |
| [来源记录](docs/sources-and-evidence.md) | 提取前原件身份、许可证及历史证据 |

```c
#include <QcomXnuUsbDebug.h>
/* 启动 CPU，在 arm_vm_init / ExitBootServices 之后；独占控制器。 */
qcom_usb_debug_init();
qcom_usb_debug_write("hello XNU\n", 10);
qcom_usb_debug_poll(); /* 定期调用，或应用随附 worker 补丁。 */
```

编译开关 `XNU_QCOM_USB_DEBUG=1`，运行启用参数 `qcom-usb-debug=1`；双向控制另加 `qcom-usb-ctl=1`。应用控制台补丁后自动捕获早期阶段和 `printf/IOLog/kprintf/panic`，消息缓冲补丁同时覆盖内核 `os_log`；用户态统一日志按 [日志指南](docs/logging.md) 额外构建并部署采集器。默认 USB serial 为 `QcomXnuUsbDebug`，默认采集结束标记为 `QCOM PANIC END`。

主机在采集器已连接并启用 `--control-socket` 后使用：

```sh
python3 scripts/xnu_usb_control.py --socket <capture>/usb-control.sock status
python3 scripts/xnu_usb_control.py --socket <capture>/usb-control.sock reboot
python3 scripts/screenshot.py --socket <capture>/usb-control.sock --output <new-image>.png
```

以上脚本路径相对模块根目录；尖括号参数需替换为实际路径。截图仅在显示驱动已注册 provider 或固定 CPU 扫描映射时可用，默认不自动读任何显存地址。

启用前，固件需要完成 USB 供电、时钟和 PHY 初始化。模块按单例轮询运行，使用有限的静态缓冲区；断连和深度休眠的支持范围见边界文档。控制协议限于文档列出的命令，不提供 GDB/KDP、远程 shell 或任意内存访问。

驱动独立维护，设备通过 `drivers/QcomXnuUsbDebug` 子模块引用，远程仓库使用 `unixnu` topic。设备维护者负责 catalog、锁定提交和归档接入。当前代码尚未接入活动设备；原始采集证据保留在来源工作区。

完整日志开发与验收使用 `tests/check_all.py`；采集时加 `--verify-logs` 可在结束后统一解码并核对原始捕获身份。参数、输出和源覆盖门槛见 [日志指南](docs/logging.md)。
