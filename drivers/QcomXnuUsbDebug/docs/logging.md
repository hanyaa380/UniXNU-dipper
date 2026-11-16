# 内核与用户态日志

模块已接入来源会话截至 s79 的日志实现：早期入口记录、消息缓冲区补采、用户态统一日志分页回传，以及正常重启末尾的排空窗口。早期代码和传输算法来自已有实现；用户态入口改为独立的 BSD sysctl，不再借用 AdrenoMetal 的用户客户端。

“完整”指一次明确范围内、通过校验的保留快照。缓冲区有限，系统原本关闭、脱敏、已经丢弃的消息，以及未写入统一日志的任意 stdout/stderr，都无法由 USB 补回。

## 覆盖哪些来源

| 来源 | 接入点 | 条件和限制 |
| --- | --- | --- |
| 冷启动入口 | `start_first_cpu` 在页表建立前保存 CNTVCT | 保存到非零初始化的数据符号；x0 和系统时钟不变 |
| 早期初始化 | 重定位后至 KPRINTF 完成的 15 个阶段 | 保存原始 ticks、入口 ticks、CNTFRQ、入口相对微秒 |
| 早期 kprintf | PE sink 初始化前仅写 CPU 队列 | 不查 CPU data、不取 kprintf 锁、不访问 USB MMIO；原生 os_log 路径保留 |
| 控制台 / printf / IOLog / panic | 原有 PE 与中央控制台挂钩 | 一次格式化的控制台路径避免重复 tee |
| 内核 os_log / TOLOGLOCKED | `bsd/kern/subr_prf.c` 的真实 `putchar` | BSD 日志锁内只入队；TOCONS 已走控制台时不再 tee，constty 重定向时补采 |
| 用户态统一日志 | root 采集器、原生 OSLog 枚举器 | 有限分页快照，必须有本轮 canary、外部进程条目和最终 EOF |

printf/IOLog 自身可能分别格式化到控制台和 os_log；原始流中可以出现两份不同来源的记录。这与同一个 `putchar` 被重复挂钩不同。早期缓存即使未启用 USB 也有固定容量；`qcom-usb-debug=1` 才会初始化硬件并补发。

worker 绑定启动 CPU 后调用 `qcom_usb_debug_worker_ready()`。普通写入只入队，每约 5 ms 批量发送；worker 退出时恢复早期有限轮询路径。`qcom_usb_debug_enqueue_putc()` 始终只写内存，适合消息锁和早期阶段。panic 单独排空，不依赖 worker。

## 用户态入口与帧协议

可选的 [QcomUsbUserLog.c](../kernel/QcomUsbUserLog.c) 内建到 BSD，注册只写 `debug.qcom_usb_userlog`。它再次检查当前调用者为 root，按固定上限 copyin，校验版本、保留字段、序号/偏移、长度及 CRC，然后一次性入队完整十六进制记录。`EAGAIN` 表示记录完全未入队；其他错误停止本次发送。内核不维护跨系统调用的流状态，主机负责检查整流连续性和元数据一致性。

[QcomUserLogWire.h](../include/QcomUserLogWire.h) 是固定的小端 ARM64 ABI，无指针和原生大小字段：48 字节头，最多 1024 字节 payload，单流最多 4 MiB。`flags=1` 是状态，`flags=2` 是 gzip。每个采集器随机生成 session，每个状态/页面分配递增 stream，块序号为 `offset / 1024`。原始文本前缀为 `QCOM_ULOG v=1`。它与截图、重启控制帧共用 USB FIFO，但不是从 Mac 写入内核的通用执行接口。

发送端只对 `EAGAIN` 保留原块重试，最多 100 次，每次等待 100 ms；成功后等待 125 ms，压缩 payload 不超过约 8 KiB/s，hex 线流量约为两倍。权限拒绝、节点不存在、copyin 失败等不会不断重连。队列原子拒收不计作普通文本丢失；失败与重试总数通过后续状态报告。完整帧不代表原始日志服务完整。

## 构建与安装采集器

先完成 [integration.md](integration.md) 的四份补丁、九个挂钩单元及静态库接入。构建静态库时另外传入真实 BSD 编译记录：

```sh
python3 -B "$MODULE/scripts/build.py" \
  --compile-record "$XNU_RECORDS/pexpert/DEVELOPMENT/pe_kprintf.o.json" \
  --userlog-compile-record "$XNU_RECORDS/bsd/DEVELOPMENT/subr_prf.o.json" \
  --soc soc/Sm8550.h --ar "$LLVM_AR" --nm "$LLVM_NM" \
  --output tmp/qud-logging-build
```

此时归档有六个对象。该 sysctl 对象通过 linker set 注册；消费者须以 `-force_load <archive>` 或等效的逐对象内建规则保留整个归档，不能只靠普通 archive 的未解析符号提取。完整链接后检查 `qcom_usb_userlog_sysctl`、`sysctl__debug_qcom_usb_userlog` 和 startup 注册引用确实存在。未传 BSD 记录时归档仍只有原来的五个对象，不含用户态入口。

用所选 iPhoneOS SDK 构建 ARM64 程序；所有输出目录必须是新路径：

```sh
python3 -B "$MODULE/scripts/build-userlog.py" \
  --clang /absolute/path/to/clang++ --sdk /absolute/path/to/iPhoneOS.sdk \
  --output tmp/qud-userlog-build
codesign --force --sign - --entitlements "$MODULE/userland/entitlements.plist" \
  tmp/qud-userlog-build/qcom-usb-log
```

构建报告记录的是签名前二进制；消费者还须记录签名后哈希/CDHash，将其加入自己的 trust cache，并按已有受信任发布流程安装。程序需以 root、所列日志 entitlement 及原生允许的执行策略运行。可先通过设备已有诊断启动机制运行该程序，确认 `collector_started` 状态；再加入消费者的启动任务。不要把文件存在或缓存安装成功当成进程已执行。

iOS16 的原厂 launchd 可能读取 `/System/Library/xpc/launchd.plist` 缓存，仅放 LaunchDaemons 文件不一定生效。缓存、独立签名、trust cache、只读执行视图和任务配置由设备仓库维护；本模块不改 launchd、AMFI 或系统服务策略。来源实现曾要求每 4 KiB 一页的 CodeDirectory；主机验签成功仍需原厂加载器实机接受。不要为接入采集器关闭签名或 MAC 检查。

采集器只为自身 PID 通过 Darwin22 原生 `memorystatus_control` 命令 7/8 设置并回读 active/inactive 128 MiB fatal 上限。root、权限和原生 MAC 检查保留；任一失败或属性不符会报告错误并停止。任务 plist 的内存字段曾被原厂全局 6 MiB 覆盖，不能替代原生回读。

## 系统日志分页与版本边界

采集器显式加载 OSLog.framework，请求 system scope，从启动开始枚举。每页最多 60000 条、8 MiB 文本，每轮最多八页，单条最多 64 KiB；超限标记 `TRUNCATED`。同一个原生枚举器继续下一页，不丢分页边界条目；`nextObject` 及格式化都在逐条 autoreleasepool 内，避免把框架临时对象积到整轮结束。

每页经临时 gzip 文件、关闭和原子 rename 后发送。目录为 `/private/var/tmp/qcom-oslog`，只保留一份当前 gzip。文件/目录、框架、权限、枚举器、gzip、rename、异常及截断均有状态或快照错误。默认最多 40 轮，每轮结束及传输后等待 45 秒；传输较慢时它不是每 45 秒采一次的连续记录器。

20H392 的 public system-scope 工厂可能无 NSError 地返回 nil。仅在 `kern.osversion` 精确为 `20H392` 且工厂无错误时，才尝试 [QcomIos16LogStore.h](../userland/QcomIos16LogStore.h) 中原生 localStore/prepare 路径。检查原始对象布局、字段类型、offset、extent、初始零约束与引用所有权；不改约束字段，原生权限错误直接返回。准备调用返回后的回调等待最多 30 秒，晚到回调持有独立对象，不引用调用者栈。原生调用本身的耗时不受这个等待预算限制。

其他系统版本可使用其正常 public factory；若不支持 system scope，返回明确失败。硬件 SoC 无关不等于用户态私有 ABI 通用。适配新系统应新增明确的 ABI 验证和测试，不直接复制 20H392 的 offset。

## Mac 采集和验收

照 [集成指南](integration.md) 启动唯一串口采集器。采集结束后保留原始 `xnu-usb.log`，再运行：

```sh
python3 -B "$MODULE/scripts/decode-user-logs.py" tmp/RUN/capture/xnu-usb.log \
  --output tmp/RUN/user-logs
python3 -B "$MODULE/scripts/qualify-user-logs.py" tmp/RUN/capture/xnu-usb.log \
  --output tmp/RUN/user-log-qualification.json
python3 -B "$MODULE/scripts/qualify-early-log.py" tmp/RUN
python3 -B "$MODULE/scripts/qualify-logs.py" tmp/RUN/capture/xnu-usb.log \
  --output tmp/RUN/logging-qualification.json
```

也可在采集命令加 `--verify-logs`：采集器先 fsync 并关闭串口，再共享同一解码结果完成三层资格检查，生成 `capture/logs/manifest.json`、`latest-oslog.txt`（有完整组时）和 `qualification.json`。`capture.json` 保留传输状态，同时记录日志资格；任一资格失败时采集器返回非零。该选项面向完整日志轮次，短截图/重启调试轮次可不启用。

已经结束的采集目录可用同一入口验收：

```sh
python3 -B "$MODULE/scripts/log_capture.py" tmp/RUN/capture
```

共享入口另外检查原始文件的字节数、已同步字节数和 SHA256 与 `capture.json` 一致。原始捕获不修改，输出只写新的 `logs/` 目录；重复执行时需另行保留结果，不覆盖旧证据。解码会读取整个原始文件，主机内存需容纳本轮捕获和保留文本。

解码目录与资格报告拒绝覆盖已有文件；早期报告可重复生成。解码器检查每帧 CRC、长度、序号、偏移、元数据、gzip、分页号、条目范围、计数及最终 EOF，只有完整分页组才能成为 `latest-oslog.txt`。更晚的不完整组不能替换上一组。非零退出码必须保留并解释，不能把它改成成功。短轮可能传输零错误但快照未结束，这仍是源覆盖不完整。

用户态资格要求同一 session 的 `collector_started` 随机 nonce、原生额度成功回读、包含该 nonce 的真实 canary、至少一个外部进程及完整 EOF。全链资格还要求早期阶段、恰好一次 `QCOM MSGBUF CANARY v=1` 和 `writer_mode=queued`。检查 STATUS 的 `er/rxo/bo` 与在带 LOSS；`bp/tx` 是待发送字节，不能当成错误计数。某一时刻为零不等于整轮无丢失。

正常重启在最后复位回调中继续轮询，原两秒总预算内两级队列连续为空 250 ms 才成功返回，新字节会重新计时。该窗口给主机 tty 读取尾部的机会，不是主机文件持久化 ACK。最后仍需核对完整 sync/unmount/PM 尾部、设备复位和恢复身份；这些设备验收不由日志解码器代替。

## 本模块检查与来源证据

```sh
python3 -B "$MODULE/tests/check_logging.py" --xnu "$XNU_SRC" --output tmp/qud-logging-tests
```

这组测试运行真实 patched formatter、原生 sink/sender/CPU ring、分页 exporter 和系统源适配器，覆盖 root/copyin、CRC、整块回压、重试上限、内存回读、逐条释放、过大条目、缺页、坏 gzip、旧 canary 及排空窗口。另需运行原有模型、归档构建、用户态构建和扩展后的 XNU 单元检查。

来源 s77 的四页含 172362 条、207 个进程，源时间约 0–435.577 秒；s78 单独验证了完整卸载与复位尾部。s79 收齐十五个早期阶段：计数器时间 38.710 秒进入内核，约 1.01 秒后 USB 就绪。前面的计数器时间主要属于内核执行之前的引导阶段。这些是原设备实现的证据；本模块改用 sysctl 的入口、最终链接和设备部署仍需自己的验收，不能继承实机通过结论。

## 一次运行完整软件检查

从 workspace 根目录执行，下列变量需指向本地已准备好的 XNU 编译记录和工具链：

```sh
python3 -B "$MODULE/tests/check_all.py" \
  --xnu "$XNU_SRC" --records "$XNU_RECORDS" --soc soc/Sm8550.h \
  --target-clang "$TARGET_CLANG" --user-clang /absolute/path/to/clang++ \
  --sdk /absolute/path/to/iPhoneOS.sdk --ar "$LLVM_AR" --nm "$LLVM_NM" \
  --output tmp/qud-complete-check
```

入口先检查 XNU revision 和两份模块编译记录，再依次执行基础模型、日志专项、带 sysctl 的六对象归档、用户态程序和补丁集检查。失败即停止并保留对应日志；全部通过后核对模块源码在检查期间未变。`check-all-report.json` 绑定每步命令、返回码、源码哈希和归档身份。该入口不签名部署用户态程序、不应用设备补丁，也不启动或重启手机。
