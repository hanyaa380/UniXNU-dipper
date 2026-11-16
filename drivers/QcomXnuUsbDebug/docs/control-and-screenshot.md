# 双向控制、Mac 重启与截图

## 运行方法

按 [integration.md](integration.md) 链接模块和四份补丁，启动参数添加 `qcom-usb-debug=1 qcom-usb-ctl=1`。Mac 需要 Python 3 和 pyserial（在所用 Python 环境安装 `python3 -m pip install pyserial`）；PNG 工具本身只使用标准库。

从 workspace 根目录运行，`MODULE` 为模块绝对路径：

```sh
python3 -B "$MODULE/scripts/collect-xnu-usb.py" --serial QcomXnuUsbDebug --output tmp/qud-run/capture --wait 120 --seconds 0 --no-marker --control-socket
```

另一个终端执行：

```sh
python3 -B "$MODULE/scripts/xnu_usb_control.py" --socket tmp/qud-run/capture/usb-control.sock hello
python3 -B "$MODULE/scripts/xnu_usb_control.py" --socket tmp/qud-run/capture/usb-control.sock ping
python3 -B "$MODULE/scripts/xnu_usb_control.py" --socket tmp/qud-run/capture/usb-control.sock status
python3 -B "$MODULE/scripts/screenshot.py" --socket tmp/qud-run/capture/usb-control.sock --output tmp/qud-run/screen.png --timeout 1200
python3 -B "$MODULE/scripts/xnu_usb_control.py" --socket tmp/qud-run/capture/usb-control.sock reboot
```

采集器负责发现/独占 USB CDC 串口和保存原始数据；不要另开串口终端争抢。PNG 路径拒绝覆盖，只有整帧校验通过才创建图片。截图传输期间保持单个控制客户端；其他命令或会话变化会使截图中止。断线/超时不自动重发有副作用命令，先查询状态，必要时重新请求整帧。高分辨率复杂图像的逐块传输可能持续数分钟，工具默认等待预算为 20 分钟。该接口用于静态截图。

## 显示驱动注册

显示驱动确认固定扫描映射已建立后，调用一次注册函数。`scanout_cpu_va` 必须是已映射、CPU 可读且一致的地址；不能把物理地址或 IOVA 强转传入。

```c
#include <QcomXnuUsbDebug.h>

int register_usb_capture(const void *scanout_cpu_va, size_t mapped_bytes,
                         uint32_t width, uint32_t height, uint32_t stride)
{
    const struct qcom_usb_framebuffer fb = {
        .pixels = scanout_cpu_va, .bytes = mapped_bytes,
        .width = width, .height = height, .stride = stride,
        .format = QCOM_USB_BGRA8888,
    };
    return qcom_usb_debug_register_framebuffer(&fb);
}
```

`stride` 为每行字节数，至少 `width*4`；映射必须覆盖所有被读取的行。描述结构会复制，像素映射则必须保留至复位。支持 BGRA8888/RGBA8888，拒绝非法尺寸/格式/长度和重复注册；返回 1 表示注册成功。

动态扫描源、缓存维护和一致帧锁定使用自定义接口：

```c
static int capture_rgb(void *cookie, uint8_t *rgb, size_t capacity,
                       uint32_t *width, uint32_t *height)
{
    /* 显示驱动实现：固定有效扫描对象、同步 CPU 可见性，检查容量，
       拷贝 packed RGB888，并在返回前释放锁和引用。 */
    return display_copy_rgb_snapshot(cookie, rgb, capacity, width, height);
}
/* 显示驱动初始化时调用一次；cookie 指向长期有效的显示状态。 */
/* qcom_usb_debug_register_screenshot(capture_rgb, display_state); */
```

示例中的 `display_copy_rgb_snapshot` 需要由消费者实现。回调在正常 USB worker、USB guard 外执行，不会在 panic 中执行；不得睡眠等待无界条件、分配内存或递归日志/USB。成功返回 1 并设置宽高；失败返回 0。模块先复制整帧到独立静态快照，再供 Mac 分块获取；快照期间显示来源变化不影响已生成图像。

## 线协议及事务

保留来源协议前缀 `SXNU1` / `SXNU1R`，前缀不表示板级依赖。请求包含 64 位 session、32 位递增 sequence、verb 和覆盖正文的 CRC32。HELLO 获取会话和最后序号，PING/STATUS 查询；REBOOT、SCREENSHOT、SHOTNEXT 使用同一事务序号规则。主机和内核 codec 实现格式、长度、CRC、会话及重放检查。控制命令不支持任意内存读取或代码执行。

控制应答最大 512 字节。只有完整应答排入发送队列才提交命令序号及截图游标；回压不会消耗数据，旧序号不会重复执行命令。会话轮换会取消旧快照；接受重启也取消快照。

| 命令 | 成功状态 | 常见失败 |
| --- | --- | --- |
| HELLO | 返回 session/last_seq | 控制未开启或链路未就绪时无应答 |
| PING / STATUS | 状态及计数 | 会话/序号/CRC 不合法时拒绝 |
| REBOOT | `state=queued` | `nohook` 或已有重启请求 |
| SCREENSHOT | `state=ready`，宽高、原始/编码长度、CRC、frame、encoding | `unavailable`、`capture-failed`、`busy` |
| SHOTNEXT | `state=data`，frame/offset/data | `noimage` 或 `done` |

frame 是生成快照的命令序号；每块最多 192 字节，使用 hex 放入 data 字段。`encoding=rgb` 表示紧密 RGB888；`rle` 为连续 `[count,R,G,B]` 四字节组，count 为 1..255，仅当编码较短时采用。CRC32 针对解码后的整帧 RGB。Mac 校验会话、事务、frame、严格连续 offset、长度及整帧 CRC，之后用标准库生成 8bit RGB PNG，并打印尺寸、CRC 和 SHA256。

## 故障定位

没有枚举时，先检查启动参数、平台供电/PHY/device 交接和模块 status，再根据资料核对基址及寄存器布局。能采日志但无控制时检查 worker 是否启动及 `qcom-usb-ctl=1`。`nohook` 检查 SoC reset 资格及其他 PE 回调所有权。`unavailable` 表示显示驱动没有注册；`capture-failed` 检查映射、尺寸、格式和 provider 返回值。帧错位或超时后重新发起完整截图，不拼接多个会话的数据。
