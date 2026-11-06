# 构建 dipper XNU 内核

本文说明如何从公开源码、公开的 SDM845 bootstrap 历史和自己合法取得的
iPadOS 17.7.11 固件生成 dipper 的 `kernel.macho`。构建不会下载或分发他人的
Apple kernelcache、预链接结果、trust cache 或 iOS 根卷。

## 1. 目标输入

完整内核必须针对以下 IPSW 身份构建：

```text
Product:      iPad7,1
Version:      17.7.11
Build:        21H461
DeviceClass:  j120ap
ChipID:       0x8011
BoardID:      12
```

`board.toml` 固定了原始 kernelcache 的 SHA-256：

```text
f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7
```

不要用其他 iOS/iPadOS build 的 `kernelcache`、`kernel.macho` 或
`kernel.json` 混搭。预链接地址、kext ABI 和早期安全初始化器都与这个版本绑定。

内核 prelink 需要 macOS 上的 Xcode `compression_tool`。推荐使用 macOS 或
GitHub 的 macOS runner；Linux 可以构建部分原生对象，但不是本教程的完整路径。

建议安装：

```sh
brew install llvm lld cmake ninja dtc lzfse mtools
brew install unifdef
```

## 2. 不公开分发的内容

以下文件不能提交到公开仓库：

```text
kernel.macho
kernel.json
kernelcache
*.adt
*.trustcache
iOS/iPadOS 根卷和 Data 镜像
```

`kernel.macho` 是源码内核与 Apple kernelcache 的预链接结果；`kernel.json`
包含原始组件地址、符号绑定和预链接报告。它们属于 Apple 二进制派生物。

建议为它们使用私有 GitHub 仓库和私有 Actions artifact。公开仓库只保存源码、
脚本、哈希、脱敏日志和构建方法。

## 3. 生成自己的 bootstrap candidate

完整 dipper 配置中的 `build-fajita-storage.py` 需要
`FAJITA_STORAGE_CANDIDATE`。该 candidate 是同一 21H461 kernelcache 的一个
早期 SDM845 预链接内核，用来核对 `IOStorageFamily` ABI 和地址。

公开的 fajita 历史中存在一个尚未启用 `NativeUfs` 和 storage archive 的阶段：

```text
Repository: https://github.com/johnpanos/UniXNU-fajita
Commit:     c78cc766fccbdd6027ab08e4b9a7f916f7eaba1e
```

先构建这个阶段，得到自己的 candidate：

```sh
export BUILD_ROOT="$HOME/unixnu-dipper-build"
export BOOTSTRAP_OUT="$BUILD_ROOT/bootstrap-out"
export IPSW="/path/to/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw"
mkdir -p "$BUILD_ROOT"
cd "$BUILD_ROOT"

git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone https://github.com/johnpanos/UniXNU-fajita

git -C UniXNU-fajita checkout \
  c78cc766fccbdd6027ab08e4b9a7f916f7eaba1e

cd "$BUILD_ROOT/UniXNU"

./unixnu defconfig fajita_defconfig \
  -D ../UniXNU-fajita \
  --out "$BOOTSTRAP_OUT"

./unixnu fetch \
  -D ../UniXNU-fajita \
  --out "$BOOTSTRAP_OUT"

./unixnu ipsw fetch KernelCache \
  -D ../UniXNU-fajita \
  --out "$BOOTSTRAP_OUT" \
  --ipsw "$IPSW"

./unixnu build kernel \
  -D ../UniXNU-fajita \
  --out "$BOOTSTRAP_OUT" \
  --ipsw "$IPSW"
```

成功输出：

```text
$BOOTSTRAP_OUT/fajita/kernel/kernel.macho
$BOOTSTRAP_OUT/fajita/kernel/kernel.json
```

确认 JSON 与 Mach-O 对应：

```sh
shasum -a 256 "$BOOTSTRAP_OUT/fajita/kernel/kernel.macho"
jq '.candidate_sha256' "$BOOTSTRAP_OUT/fajita/kernel/kernel.json"
file "$BOOTSTRAP_OUT/fajita/kernel/kernel.macho"
```

`kernel.json` 必须与 `kernel.macho` 同目录，文件名除后缀外必须相同。

如果公开的 bootstrap 历史与当前 UniXNU 不再兼容，请保存完整错误日志和两侧
commit；不要通过更换 kernelcache、关闭签名检查或复制他人的 candidate 绕过
ABI 检查。

## 4. 构建完整 dipper 内核

获取公开 dipper 仓库：

```sh
cd "$BUILD_ROOT"
git clone https://github.com/hanyaa380/UniXNU-dipper
git -C UniXNU-dipper checkout diag/io-media-registry
```

设置第 3 步生成的 candidate：

```sh
export DIPPER_OUT="$BUILD_ROOT/dipper-out"
export FAJITA_STORAGE_CANDIDATE="$BOOTSTRAP_OUT/fajita/kernel/kernel.macho"
test -f "${FAJITA_STORAGE_CANDIDATE%.macho}.json"
```

构建：

```sh
cd "$BUILD_ROOT/UniXNU"

./unixnu defconfig dipper_defconfig \
  -D ../UniXNU-dipper \
  --out "$DIPPER_OUT"

./unixnu fetch \
  -D ../UniXNU-dipper \
  --out "$DIPPER_OUT"

./unixnu ipsw fetch KernelCache OS Cryptex1,SystemOS \
  -D ../UniXNU-dipper \
  --out "$DIPPER_OUT" \
  --ipsw "$IPSW"

FAJITA_STORAGE_CANDIDATE="$FAJITA_STORAGE_CANDIDATE" \
./unixnu build kernel \
  -D ../UniXNU-dipper \
  --out "$DIPPER_OUT" \
  --ipsw "$IPSW"
```

输出：

```text
$DIPPER_OUT/dipper/kernel/kernel.macho
$DIPPER_OUT/dipper/kernel/kernel.json
```

重新只执行 prelink：

```sh
FAJITA_STORAGE_CANDIDATE="$FAJITA_STORAGE_CANDIDATE" \
./unixnu build prelink \
  -D ../UniXNU-dipper \
  --out "$DIPPER_OUT" \
  --ipsw "$IPSW"
```

## 5. 验证结果

至少保存以下身份：

```sh
sha256sum "$DIPPER_OUT/dipper/kernel/kernel.macho"
sha256sum "$DIPPER_OUT/dipper/kernel/kernel.json"
jq '.candidate_sha256, .candidate_bytes, .runtime_qualified' \
  "$DIPPER_OUT/dipper/kernel/kernel.json"
git -C ../UniXNU-dipper rev-parse HEAD
git -C ../UniXNU-fajita rev-parse HEAD
git -C . rev-parse HEAD
```

构建报告中应出现：

```text
kernel: .../kernel.macho sha256 ...
```

如果最终 payload 中缺少内核，检查 `$DIPPER_OUT/dipper/kernel/kernel.macho`
是否真实存在，而不是只看 Actions 的 `Build kernel` 步骤颜色。

## 6. 使用私有 GitHub Actions

推荐把内核 workflow 放在私有仓库，而不是公开 fork 的 artifact 中。

### 6.1 打包私有 reference candidate

私有输入仓库中创建：

```text
fajita/kernel/kernel.macho
fajita/kernel/kernel.json
```

压缩成：

```text
fajita-kernel.zip
```

ZIP 根目录应直接包含 `fajita/`，这样解压后路径为：

```text
<extract>/fajita/kernel/kernel.macho
<extract>/fajita/kernel/kernel.json
```

### 6.2 Secrets

私有输入仓库准备一个只读 Deploy Key：

```text
PRIVATE_INPUTS_DEPLOY_KEY=<只读 deploy private key>
```

如果设备仓库也是私有的，再设置：

```text
UNIXNU_DEVICE_TOKEN=<只读仓库 token>
```

不要将私钥、token、IPSW URL 中的签名参数或 Apple 二进制写进日志。

### 6.3 Workflow 输入

根据当前设备仓库的 workflow，典型输入为：

```text
device_repository=你的账号/UniXNU-dipper
device_ref=diag/io-media-registry
defconfig=dipper_defconfig
steps=kernel
xcode=26.6
pre_build=./unixnu ipsw fetch KernelCache OS Cryptex1,SystemOS
```

如果你的 workflow 使用了别人的私有输入仓库地址，必须改成自己的仓库。
不要在公开仓库里复用他人的 private-input secret。

## 7. 当前 dipper 状态

截至本教程记录时，公开分支已经验证到：

- UEFI、UniXNU 菜单和 DarwinBoot
- XNU、MMU、ArmCpu 和 GICv3
- 8 个逻辑 CPU 通过 PSCI/SMC 启动
- apps SMMU 和 NativeUfs
- UFS 读取、APFS container/volume 发布
- SystemOS 和 Data 挂载
- launchd 和基础用户态服务
- Apple Logo、完整启动进度条和自选滚动日志

当前主要问题在进度条完成后的显示/backboardd/SpringBoard 接管，不再是 UFS
读取失败。更多真机证据和镜像哈希见：

<https://github.com/hanyaa380/dipper-unixnu-bringup>

## 8. 常见错误

### `FAJITA_STORAGE_CANDIDATE must name ... kernel.macho`

原因通常是环境变量没有传入子进程，或同目录没有同名 `.json`。使用：

```sh
export FAJITA_STORAGE_CANDIDATE=/absolute/path/to/kernel.macho
test -f "${FAJITA_STORAGE_CANDIDATE%.macho}.json"
```

### `does not match the kernelcache`

IPSW 不是固定的 17.7.11 / 21H461，或传入的 kernelcache 不是 arm64 slice。
重新从正确 IPSW 获取，不要修改 `kernelcache_sha256` 来绕过检查。

### `No XNU symbol names`

通常表示 bootstrap pass 没有生成 `link.filelist` 或编译记录。查看：

```text
out-<name>/<board>/logs/kernel-bootstrap.log
out-<name>/<board>/logs/kernel.log
```

### NativeUfs archive 找不到

完整 `dipper_defconfig` 需要 `FAJITA_STORAGE_CANDIDATE`、storage ABI audit、
VerifiedRoot、boot progress 和 display archives。先确认 candidate 和全部
archive builder 的顺序，不要只单独编译 `NativeUfs.cpp`。

### 链接成功但设备不启动

先区分：

```text
UEFI 菜单是否出现
DarwinBoot 是否退出 boot services
XNU 是否到达 bsd_init
UFS/APFS 是否发布 media
launchd 是否执行
```

保存对应阶段的日志、最终 `kernel.macho` SHA256、ADT/trustcache 和启动参数。
只报告“卡在 Logo”不足以定位阶段。
