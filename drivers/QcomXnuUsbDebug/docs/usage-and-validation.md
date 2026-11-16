# 使用与验收记录

完整接入步骤见 [integration.md](integration.md)，控制与截图见 [control-and-screenshot.md](control-and-screenshot.md)，适用范围见 [boundaries.md](boundaries.md)。

## 软件检查

| 检查 | 覆盖 | 不代表 |
| --- | --- | --- |
| `tests/check_all.py` | 按固定顺序运行基础/日志测试、完整归档、采集器和 XNU 检查，并绑定源码/归档身份 | 完整内核链接及设备部署 |
| `tests/check.py` | 28 项：实际 C 传输/私有域/缓冲/控制/截图、ASan/UBSan、重定位资源、ELF/COFF、C 应答到分片 socket 和 PNG CLI | 真实硬件枚举/截图 |
| `scripts/build.py` | 5 个基础 ARM64 XNU 对象，可选第六个 BSD sysctl 对象、静态归档、来源 SHA、导入导出及禁止 identity fallback 审计 | 完整内核链接 |
| `tests/check_logging.py` | 15 项：真实消息缓冲/早期 formatter、独立 sysctl、原子 ring、分页 exporter、系统源 ABI 和源覆盖门槛 | 设备部署及完整快照实测 |
| `tests/check_xnu.py` | 28 项：补丁应用/撤销/重复/冲突/版本、八个真实 C 单元开启/关闭编译及冷入口指令片段、归档 hook 解析 | 设备启动或 reboot 成功 |

宿主测试需允许创建本地 Unix socket。所有测试输出使用新 `tmp/` 目录；脚本拒绝覆盖既有报告。报告明确标记 `hardware_verified=false`。来源设备的历史结果按其当时的镜像和配置解释；本模块的硬件接入需另行验收。

## 设备接入后的验收

1. 保存 SoC 来源、XNU revision、模块源码和归档 SHA、最终镜像 SHA 与启动参数。
2. 使用消费者已有的临时启动和恢复流程，采集原始 USB 数据及命令/恢复元数据。
3. 验证 serial、CDC full/high-speed、早期日志、正常 worker、长消息、压力和断线计数。
4. 发送 HELLO/PING/STATUS；检查 session、控制开关及回压计数。
5. 显示驱动注册真实扫描来源，保存 PNG 与元数据；与设备可见画面独立对照。
6. 明确发送 REBOOT，确认正常关机日志、复位和恢复；`queued` 本身不足以验收。
7. 如测试 panic，只有 `QCOM PANIC END` 表示模块确认两级发送队列排空；无标记不能认定采集完整。

截图成功与 USB 服务发布均不能代替可见桌面、面板或 Metal shader 执行验收。所有原始 USB 采集及元数据按 workspace 规则保护，不作为普通旧输出清理。

## 测试文件

| 文件 | 验证内容 |
| --- | --- |
| `test_dwc3.c` | DWC3 控制传输、端点、TRB 与收发模型 |
| `test_domain.c` | 私有 SMMUv2 页表、安装、读回、故障与恢复 |
| `test_qualify.c` | 历史 identity qualifier 的寄存器访问和所有权检查，固定 SM8550 夹具 |
| `test_inspect.c` | 从所选 profile 读取时钟与 DWC3 状态，验证资源重定位 |
| `test_usb_buffer.c` | CPU 积压、争用与容量限制 |
| `test_usb_control.c` | CRC、分片解析、会话、序号、长度及请求预算 |
| `test_usb_screenshot.c` | provider 注册、固定像素格式、RGB/RLE、快照游标和失败处理 |
| `test_usb_screenshot_control.c` | 实际控制处理器的回压、重复序号、截图失败和重启回调要求 |
| `test_screenshot.py` | C 应答经分片本地 socket 到 Mac CLI，验证 PNG 内容与 CRC |
| `check.py` | 宿主模型、ASan/UBSan、SM8550/人工重定位矩阵、ELF/COFF 交叉编译 |
| `check_xnu.py` | 版本锁定、补丁往返与冲突、八个 XNU C 单元和冷入口指令片段开关编译和归档 hook 解析 |

## 宿主测试运行方法

从 workspace 根目录执行，下面的变量替换为本地真实路径。输出目录必须是尚不存在的 `tmp/` 或 `.cache/` 子目录，并位于模块源码之外。

```sh
MODULE="$PWD/QcomXnuUsbDebug"
TARGET_CLANG="/absolute/path/to/clang"
python3 -B "$MODULE/tests/check.py" --target-clang "$TARGET_CLANG" --output tmp/qud-model-check
```

宿主需要 clang 和可用的 ASan/UBSan 运行库；目标 clang 旁需有 `llvm-nm`。截图 CLI 测试只创建本地 Unix socket，不连接设备，需要执行环境允许创建该 socket。Python 导入使用脚本自身的位置定位 `scripts/`，无需设置 `PYTHONPATH`。

默认共 28 项检查。共享 DWC3 模型和 ELF/COFF 编译不选择任何 SoC 配置，覆盖独立描述符实例和调用方字符串复制。它们各运行一次，避免在两个平台 profile 下重复执行相同检查。人工重定位配置沿用同一寄存器布局，用于检查地址、SID、IOVA 和时钟偏移参数化；它不代表新增支持的 SoC。历史 identity qualifier 只在 SM8550 夹具上运行，原生后端仍只使用私有转换域。

XNU 检查需要按 [integration.md](integration.md) 生成模块归档，并提供该版本内核和编译记录：

```sh
python3 -B "$MODULE/tests/check_xnu.py" \
  --xnu "$XNU_SRC" --records "$XNU_RECORDS" \
  --archive tmp/qud-module-build/libQcomXnuUsbDebug.a --nm "$LLVM_NM" \
  --output tmp/qud-integration-check
```

这组共 28 项检查。脚本从传入内核读取锁定版本的源码，在独立 fixture 中应用和撤销补丁，不修改传入树。生成的对象、日志和报告保留在指定输出目录；整个过程不启动、刷写或重启设备。

历史平台记录见 [来源与证据](sources-and-evidence.md)，当前运行命令以本页为准。

## 新增测试要求

C 模型通过模块根目录的 include 路径引用实际实现。需要寄存器模型时限制可访问地址，并断言失败时的写入及恢复行为；不要复制一份驱动算法来充当被测实现。测试只产生的夹具放在本目录，运行时生成的文件进入输出目录。

新增 SoC 要扩展测试矩阵和对应夹具。仅传 `scripts/build.py --soc` 会选择构建 profile，不会自动运行该 SoC 的模型。查看 [适配经验](porting-other-socs.md) 中的具体要求。

新增的完整日志接入、15 项专用主机测试、用户态构建及验收流程见 [logging.md](logging.md)。完整冷启动汇编仍需消费者平台前置补丁，指令片段编译不等于最终入口链接审计。
