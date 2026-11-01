# 完整构建与接入方法

接入目标为 Darwin22 / ARM64 / XNU 8796.141.3。更换 OS 版本时，先增加对应补丁 profile，再完成同样的检查。支持边界见 [boundaries.md](boundaries.md)，截图及协议见 [control-and-screenshot.md](control-and-screenshot.md)。

## 1. 准备输入

需要本模块源码、目标 SoC 的真实硬件资料、XNU 独立构建树、生成头文件和各输出单元的编译记录，以及目标工具链。建议设备以 `drivers/QcomXnuUsbDebug` 子模块引用本模块。

Darwin22 补丁锁定 XNU Git revision `1b191cb58250d0705d8a51287127505aa4bc0789`，版本和编译单元在 [xnu_patches.py](../scripts/xnu_patches.py) 中定义，补丁顺序由 [补丁目录](../patches/darwin22) 中的四位数字文件名前缀决定。准备该版本正常 XNU 构建环境，先生成头文件和 `pe_kprintf.o.json`。编译记录是 JSON 对象，`arguments` 必须为 argv 数组，引用的源码/头文件/编译目录必须仍有效。

所有构建生成物放在 workspace 的 `tmp/` 或 `.cache/`，不进入模块源码。下面以 workspace 根目录为起点；变量中 `/absolute/...` 必须改成真实路径：

```sh
MODULE="$PWD/QcomXnuUsbDebug"
XNU_SRC="/absolute/path/to/dedicated/xnu-worktree"
XNU_RECORDS="/absolute/path/to/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
LLVM_AR="/absolute/path/to/llvm-ar"
LLVM_NM="/absolute/path/to/llvm-nm"
TARGET_CLANG="/absolute/path/to/clang"
```

`XNU_SRC` 应为构建器使用的明确 Git checkout/worktree 根目录；工具不会自动选择活动设备树，也不会重置、清理或覆盖已有设备补丁。若当前树带旧 socrates USB 改动，先在独立树完成迁移，不能将两套 USB 补丁叠加。

## 2. 选择并核对 SoC 配置

适配新平台时，先按 [其他 SoC 适配](porting-other-socs.md) 核对寄存器语义和启动交接，再填写配置。

[soc/Sm8550.h](../soc/Sm8550.h) 包含 GCC/DWC3/SMMU 基址及长度、五个 CBCR、BCR/GDSCR/CFG_GDSCR、QSCRATCH、USB SID、私有 IOVA、serial/product/VID/PID 和 `QUD_PSCI_SMC_RESET`。

寄存器布局兼容时，新建 `soc/<Name>.h`，通过 `--soc` 选择。供电、PHY 或 SMMU 架构有差异时，还需实现对应平台逻辑。编译必须显式选择配置。没有确认 PSCI SMC 复位能力的设备将 `QUD_PSCI_SMC_RESET` 置 0；此时控制通道仍能提供状态/截图，但不会安装模块复位回调，重启请求返回 `nohook`。

## 3. 检查并应用模块内补丁

```sh
python3 -B "$MODULE/scripts/xnu_patches.py" status --xnu "$XNU_SRC"
python3 -B "$MODULE/scripts/xnu_patches.py" check --xnu "$XNU_SRC"
python3 -B "$MODULE/scripts/xnu_patches.py" apply --xnu "$XNU_SRC"
```

工具校验 Git revision、连续的补丁编号和文件路径，整组预检查通过后才使用一次 `git apply` 应用四份补丁。状态为 `ready`、`applied` 或 `conflict-or-mixed`。重复应用、错误版本和冲突均拒绝；不会执行 `reset/stash/clean`，也不使用 `--reject` 或三方合并。

| 补丁 | 修改范围 |
| --- | --- |
| [0001 console](../patches/darwin22/0001-qcom-usb-debug-console.patch) | PE 初始化、kprintf、printf/debug、中央控制台、长消息及 panic |
| [0003 msgbuf](../patches/darwin22/0003-qcom-usb-debug-msgbuf.patch) | TOLOG/TOLOGLOCKED、内核 os_log 的 CPU 入队 |
| [0004 early](../patches/darwin22/0004-qcom-usb-debug-early-log.patch) | 冷入口计数器和早期初始化阶段 |
| [0002 worker](../patches/darwin22/0002-qcom-usb-debug-worker.patch) | kminit 后启动 CPU 轮询线程、可选正常重启线程 |

补丁仅引用 `QcomXnuUsbDebug.h`、`qcom_usb_debug_*` 和 `XNU_QCOM_USB_DEBUG`，不含板级地址、旧设备宏或显示驱动调用。截图 provider 由显示驱动注册，USB worker 无需修改。

## 4. 构建模块静态库

```sh
python3 -B "$MODULE/scripts/build.py" \
  --compile-record "$XNU_RECORDS/pexpert/DEVELOPMENT/pe_kprintf.o.json" \
  --soc soc/Sm8550.h --ar "$LLVM_AR" --nm "$LLVM_NM" \
  --output tmp/qud-module-build
```

输出目录必须是新路径。输出包括 `libQcomXnuUsbDebug.a`、五个基础对象（可选用户态入口增加第六个，见 [日志接入](logging.md)）、日志和带源码 SHA256、外部导入及公共导出审计的 `build-report.json`。脚本不改 XNU 源码，不链接完整内核，不操作设备。

SoC 配置通过编译宏 `QUD_SOC_HEADER` 选取，供高通平台代码与 XNU 适配层使用。共享 DWC3 不引用该配置，由适配层传入 USB 标识和平台回调；XNU 的九个挂钩编译单元只需要公共接口头文件。

## 5. 加入 XNU 编译与链接

在目标内核构建配方给下列九个单元增加：

```text
-DXNU_QCOM_USB_DEBUG=1
-I<absolute-module-path>/include
```

单元为 `pexpert/arm/pe_kprintf.c`、`osfmk/kern/debug.c`、`osfmk/kern/printf.c`、`osfmk/console/serial_console.c`、`osfmk/console/serial_general.c`、`bsd/kern/bsd_init.c`、`bsd/kern/subr_prf.c`、`osfmk/arm/arm_init.c`、`osfmk/arm64/start.s`。最后一项将宏加入汇编预处理参数；其余为 C 单元。`XNU_SOCRATES_USB_LOG` 和 `XNU_SOCRATES_FB_CAPTURE` 不参与新补丁；不需要接显示截图私有头文件。

把新归档加入目标内核的内建 archive/prelink 输入，保留所选构建器的依赖和顺序规则。启用 sysctl 时用 `-force_load` 或等效逐对象规则保留注册对象；完整用户态日志另见 [logging.md](logging.md)。不能同时链接旧设备 USB 归档。完整链接后检查：所有 `qcom_usb_debug_*` 调用均由新归档提供，没有未解析 hook、重复单例或 EFI imports。

在设备 catalog 中声明源码提交、SoC profile、构建命令、归档输出和 OS 依赖，再由设备的 kernel/payload/bootimg 配方消费。board.toml、构建器和归档路径由设备仓库维护。

编译关闭时对九个单元设 `XNU_QCOM_USB_DEBUG=0`，它们不应导入任何 USB hook。不要编译开启但漏链归档。

## 6. 启动参数与能力选择

| 配置 | 效果 |
| --- | --- |
| `qcom-usb-debug=1` | 启动日志后端及正常 worker |
| 另加 `qcom-usb-ctl=1` | 接收状态、重启、截图控制命令；默认关闭 |
| `-enable_kprintf_spam` | 按 Darwin22 策略请求详细 kprintf，早期 CPU 缓存先补发，sink 就绪后恢复正常锁路径 |
| `qcom-usb-diag=<mode>` | 显式诊断模式，可能按原诊断规则复位；日常采集不要添加 |

没有 `qcom-usb-debug=1` 时不初始化硬件。缺少 `qcom-usb-ctl=1` 时正常 worker 不开启控制接口，也不安装重启回调。`serial=3`、`-v` 等由 OS 策略决定，不是硬件驱动的替代品。

## 7. 注册截图来源（可选）

在显示驱动已经建立并验证 CPU 扫描映射后调用 `qcom_usb_debug_register_framebuffer()`，或注册自定义 `qcom_usb_debug_register_screenshot()`。映射、缓存一致性、像素格式及生命周期由显示驱动负责，完整示例见 [截图接入](control-and-screenshot.md)。未注册时返回 `SCREENSHOT ERR state=unavailable`，不影响日志和重启。

## 8. 采集与 Mac 控制

先启动采集器，再通过设备已验证方式临时启动新镜像：

```sh
python3 -B "$MODULE/scripts/collect-xnu-usb.py" \
  --serial QcomXnuUsbDebug --output tmp/qud-run/capture \
  --wait 120 --seconds 0 --no-marker --control-socket
```

另一个终端：

```sh
python3 -B "$MODULE/scripts/xnu_usb_control.py" --socket tmp/qud-run/capture/usb-control.sock hello
python3 -B "$MODULE/scripts/xnu_usb_control.py" --socket tmp/qud-run/capture/usb-control.sock status
python3 -B "$MODULE/scripts/screenshot.py" --socket tmp/qud-run/capture/usb-control.sock --output tmp/qud-run/screen.png
python3 -B "$MODULE/scripts/xnu_usb_control.py" --socket tmp/qud-run/capture/usb-control.sock reboot
```

只有采集器打开串口；其他工具通过私有 Unix socket 交换命令。路径使用短 workspace 子目录，避免 macOS socket 长度限制。每轮使用新采集目录，PNG 拒绝覆盖。重启后继续检查正常 sync/unmount、系统恢复与设备身份，不能仅凭 `queued` 判断成功。

## 9. 模块与补丁自检

完整日志配置优先使用 [logging.md 的统一检查入口](logging.md#一次运行完整软件检查)，一次构建并验证基础驱动、BSD 入口、用户态采集器与 XNU 补丁。以下保留各组件的独立检查方法。

```sh
python3 -B "$MODULE/tests/check.py" --target-clang "$TARGET_CLANG" --output tmp/qud-model-check
python3 -B "$MODULE/tests/check_xnu.py" \
  --xnu "$XNU_SRC" --records "$XNU_RECORDS" \
  --archive tmp/qud-module-build/libQcomXnuUsbDebug.a --nm "$LLVM_NM" \
  --output tmp/qud-integration-check
```

第一组共 28 项，运行实际 C 传输/私有域/缓冲/控制/截图模型及 SM8550 历史 qualifier，真实 C 应答到 Mac PNG CLI 的分片 socket 测试，以及 ELF/COFF 交叉编译。第二组共 28 项，从 `git show` 读取锁定源码，在独立 fixture 检验应用/撤销/冲突，编译八个 C 输出/初始化/worker 单元及冷入口指令片段的开启/关闭版本，核对 hook 导入与归档导出。两者不修改传入的 XNU 树。

完成这些检查后，还需链接完整内核，并在设备上验证 USB、重启和显示。保存镜像、源码 revision、配置、完整原始采集和恢复记录；容量及证据保留规则沿用 workspace 约束。

## 10. 撤销与旧设备迁移

```sh
python3 -B "$MODULE/scripts/xnu_patches.py" reverse --xnu "$XNU_SRC"
```

仅当整组补丁能完整反向应用时才撤销；有后续冲突时拒绝，不恢复整份源文件。同步撤下新归档、编译宏、参数及 provider，再按设备配方重建内核/payload/镜像。已运行的内建模块不能靠删参数在线卸载，应重启到验证过的回退镜像。

旧设备接口迁移：`socrates-usb-log` → `qcom-usb-debug`，`sxnu-ctl` → `qcom-usb-ctl`，`socrates-usb-diag` → `qcom-usb-diag`，serial `SocratesXnuLog` → 默认 `QcomXnuUsbDebug`，结束标记 `SOCRATES PANIC END` → `QCOM PANIC END`。无旧宏/符号隐式兼容别名，主机命令和设备配方必须一同更新。来源设备的历史实现与证据保留原名。

扩展检查使用实际 `start.o.json` 或唯一的 `start.*.json` 汇编记录，但只编译本模块的冷入口指令片段。公开 XNU 缺少部分 Apple tunables，完整 `start.s` 仍需消费者已有的平台前置补丁和最终构建；该片段检查不算完整汇编单元编译或内核链接。
