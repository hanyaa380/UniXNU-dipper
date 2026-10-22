# SSH over USB into the fajita iPadOS userland

Goal: from the Linux host, over the USB cable, get a root shell on the running
iPadOS 17.7.11 userland on the OnePlus 6T (fajita) and reboot it.

This note records the chosen path, every component, the risks, and — at the end —
exactly what is built and verified offline, what is left, and how to test on device.

## The device, and why this is hard

- XNU 23.6.0 runs the unmodified iPad7,1 userland at EL1 under Qualcomm's
  hypervisor. There is **no network driver** and no Apple USB stack; Apple's USB
  device stack expects Apple's controller, not the SDM845's Synopsys DWC3.
- The USB block (device tree + Mu-Silicium `USB30_PRIM`):
  - dwc3 core `0x0a600000` (window `0xcd00`), wrapper/QSCRATCH `0x0a6f8800`;
  - QUSB2 v2 HS PHY `0x088e2000`; the board is wired USB2-only
    (`dr_mode = "peripheral"`, `maximum-speed = "high-speed"`, single HS PHY);
  - GCC `0x00100000` gates `USB30_PRIM` (GDSC `0xf004`, master `0xf00c`, …);
  - `apps_smmu` `0x15000000` (size `0x80000`) sits in front of DMA; the dwc3
    master is `iommus = <&apps_smmu 0x740 0>`.
- UEFI (Mu-Silicium) brings the controller up in **device mode** for fastboot:
  `UsbfnDwc3Dxe`, `UsbPwrCtrlDxe`, `UsbDeviceDxe` are in its APRIORI/DXE lists. So
  at XNU entry the GDSC is on, the PHY PLL is (expected) locked, and the core is in
  device mode — the probe (below) confirms this read-only before we rely on it.
- Signing: only code in a trust cache runs. The kernel is a DEVELOPMENT build, so
  `mach_loader.c` allows static executables; a plain ad-hoc-signed arm64 binary whose
  cdhash is in an owner trust module will load (A10X userland is plain arm64, no PAC).
- The SMMU is the DMA gatekeeper. `drivers/NativeUfs/SmmuTakeover.h` already shows
  how to take over a stream the firmware left: plan a free context bank + stream
  group, set VA64 bypass, or adopt the firmware's own bypass bank unchanged.

## Candidate paths

| path | channel | host sees | effort | verdict |
| --- | --- | --- | --- | --- |
| (a) DWC3 gadget → CDC-ECM/NCM + `ifnet` + dropbear | IP over USB | `cdc_ether`/`cdc_ncm` netdev; real `ssh root@…` | highest | **the goal** |
| (b) DWC3 gadget → CDC-ACM + shell / dropbear-on-tty | serial | `/dev/ttyACM0` | high | **intermediate**; proves the gadget before `ifnet` |
| (c) usbmux (checkra1n/palera1n style) | TCP over usbmux | `iproxy` → ssh | n/a here | **rejected**: usbmux is `usbmuxd` + Apple's USB device stack on lockdownd; neither exists on this port |

Chosen: **(a), reached in stages through (b).** (a) is the only path that yields
real SSH, and CDC-ECM/-NCM are picked up by the in-box Linux `cdc_ether`/`cdc_ncm`
drivers with no host-side driver work. (b) is the same DWC3 gadget with a simpler
function and is the natural halfway point: once the controller enumerates as CDC-ACM
on the host, swapping the function to CDC-ECM and attaching an `ifnet` is incremental.
usbmux (c) is a dead end on this port because it rides Apple's USB device stack.

Prior art checked: UniXNU-sheng's log channel (`package/linux-ref/init`,
`package/linux-log/init`, `scripts/read-sheng-log.py`) is a **Linux** image using
Linux's own dwc3 + configfs gadget to expose CDC-ACM; it is not XNU code and cannot
be reused directly, but it confirms the hardware does CDC-ACM device mode on this
board and that the host side is just `/dev/ttyACM*`. No XNU-side USB/gadget/`ifnet`
prior art exists anywhere in UniXNU `drivers/` or `kits/` — this is new ground.

## Components

### 1. Kernel: `drivers/NativeDwc3` (the USB device controller)

Staged exactly like NativeUfs — read-only probe first, every stage logged through
counters that `package/BootLog.c` surfaces (row prefix `U`), default-off behind a
boot arg.

- **Stage A — read-only probe (built now).** `native-dwc3-probe=1` maps GCC + the
  dwc3 core window UEFI left up and reads GSNPSID/GCTL/DCFG/DSTS/DALEPENA/event
  buffer + the QUSB2 PHY ready bit; `native-dwc3-probe=2` additionally computes a
  **read-only** `apps_smmu` plan for stream `0x740` using the NativeUfs planner.
  Nothing is written. This confirms the state UEFI left and the DMA path before any
  gadget code touches the controller.
- **Stage B — CDC-ACM gadget (next).** Allocate a coherent event buffer + per-endpoint
  TRB rings (SMMU stream `0x740`, taken over like UFS), soft-reset into device mode,
  run the DWC3 endpoint-0 command set (`DEPCFG`/`DEPXFERCFG`/`DEPSTRTXFER`) to answer
  SET_ADDRESS/SET_CONFIG and the CDC-ACM descriptors, then bulk IN/OUT a byte stream.
  Host sees `/dev/ttyACM0`. A kernel-side line discipline or a launchd job on the tty
  gives a shell.
- **Stage C — CDC-ECM + `ifnet` (the goal).** Swap the function descriptors to CDC-ECM
  (or NCM), register an `ifnet` (`ifnet_allocate`/`ifnet_attach`) that shuttles frames
  between the bulk endpoints and the iPadOS network stack. Host `cdc_ether` brings up
  a point-to-point link; assign static IPs both ends.

DMA/SMMU: the gadget's event buffer and TRB rings are the DMA masters behind stream
`0x740`. The probe already proves the planner handles that stream against the real
`0x80000` window; stage B reuses `SmmuTakeover.h`'s `configureUfsSmmu` to install (or
adopt) the bypass mapping, with the same cache-maintenance (`dc civac`, device-memory
aligned stores) NativeUfs uses.

### 2. Trust cache + signing

- Userland binaries (dropbear, `fajita-reboot`) are owner-built, ad-hoc signed with
  4 KiB code pages (the kernel validates 4 KiB pages — `native-4k-signature-hash-cache`),
  exactly as `scripts/build-adreno-metal-plugin.py` signs the Adreno probe daemon.
- Their cdhashes go into an **owner trust module** via `make-owner-trust-bundle.py`
  (extend its cdhash list), delivered with `--owner-trust-cache` (which carries the
  two original caches byte-for-byte plus the owner module and skips the board
  allow-list). No Apple signature or trust module is changed.
- A daemon outside every sandbox profile needs `com.apple.private.security.no-container`
  (and `platform-application`), the same minimal grant the Adreno daemon carries.

### 3. launchd plumbing — reuse the GPU-addon union mount

The port already has the exact mechanism needed: `patches/xnu/zzy-native-gpu-addon-mount.patch`
(boot arg `sheng-gpu-addon=1`) writes owner files from a payload addon into the
writable Data volume under `/private/var/ngpu/{bin,daemons}` and **union-mounts
`/private/var/ngpu/daemons` over the sealed `/System/Library/LaunchDaemons`** before
launchd starts — a process outside every sandbox profile. So the SSH daemon needs no
new kernel plumbing:

1. place `org.blu-sharky.sshd.plist` in the addon's `daemons/` and `dropbearmulti` +
   `fajita-reboot` in `bin/`;
2. pack with `scripts/make-gpu-addon.py` (or stage into the Data seed with
   `add-gpu-plugin-data.py`), carry it with `unixnu build payload --gpu-addon FILE`;
3. boot with `sheng-gpu-addon=1`; launchd reads the unioned plist and `RunAtLoad`s the
   daemon as root.

The plist's `ProgramArguments` runs `dropbearmulti dropbear -F -E -r <hostkey> …`. As
with the Adreno job, launchd may also need the daemon offered under the name of an
existing `RunAtLoad` job if directory enumeration misses the overlay's new name.

### 4. Reboot — `package/ssh-tools/fajita-reboot.c`

A tiny owner tool that calls `reboot(2)`; any reset lands the phone in the OnePlus
ABL fastboot menu (the board carries no boot image in `boot_a`/`boot_b`). The kernel
already has the PSCI `SYSTEM_RESET` path (`package/BootReset.c`), used today by the
`sheng-autoreset=<seconds>` timer, which is the fallback if `reboot(2)` does not
reach PEHaltRestart through the sheng PMGR stub on a given build.

### 5. Host side

- CDC-ACM (stage B): `/dev/ttyACM0`; `screen`/`picocom` for the shell; the sheng log
  reader shows the same port works on this hardware.
- CDC-ECM (stage C): the kernel loads `cdc_ether`, a `usb0`/`enx…` netdev appears;
  `ip addr add 10.0.0.1/24 dev usb0`, phone side `10.0.0.2`, then `ssh root@10.0.0.2`.
- `ssh root@… reboot` or `ssh root@… fajita-reboot` to restart.

## Risks

- **Hypervisor / EL1.** The controller and SMMU MMIO are at EL1 under Qualcomm's
  hypervisor; the UFS driver already drives MMIO and the SMMU from EL1 here, so DWC3
  MMIO should be reachable, but any register the hypervisor traps would fault — the
  staged read-only probe is precisely to find that before writing anything.
- **SMMU / DMA.** The gadget DMA (event buffer, TRBs) goes through `apps_smmu` stream
  `0x740`. UEFI may leave its own mapping for `0x740` (as it does for UFS `0x100`); we
  adopt it if it is an off-translation bypass bank, else plan a free bank. If UEFI
  tore its mapping down, stream `0x740` faults until we install one — covered by
  `configureUfsSmmu`, but unverifiable until on-device.
- **PHY / controller state after hand-off.** We assume UEFI left the GDSC on, the
  QUSB2 PLL locked and the core in device mode. The probe reports exactly this; if
  UEFI disabled USB on exit, stage B must bring up the GCC clocks and the QUSB2 PHY
  from reset (offsets are in `Dwc3Registers.h`/`Dwc3Phy.h`), a larger task.
- **Signing.** Binaries must be 4 KiB ad-hoc signed and their cdhashes trusted;
  wrong page size or a missing cdhash → AMFI kill. The Adreno flow is the proven
  template.
- **Userland SDK.** Linking an iOS userland binary needs an iOS SDK sysroot
  (libSystem) and a Mach-O signer; **this Linux host has neither** (only the kernel's
  stand-in header SDK). So dropbear/`fajita-reboot` cannot be built+signed+verified on
  this host — see below.

## Status: done / verified / left

**Done and verified offline (this pass):**

- `drivers/NativeDwc3` stage-A read-only probe: register map + decode
  (`Dwc3Registers.h`, `Dwc3Phy.h`), GCC clock gate, QUSB2 PHY ready bit, and a
  read-only `apps_smmu` plan for stream `0x740` reusing `NativeUfs/SmmuTakeover.h`.
  Default-off behind `native-dwc3-probe`.
- Host unit tests (`drivers/NativeDwc3/tests`, `c++ -std=c++17`): register decode and
  the stream-`0x740` SMMU plan. **Pass.**
- Archive builder `scripts/build-fajita-dwc3.py` (compiles against this kernel's
  recorded IOKit ABI + audits the Mach-O), wired into `board.toml` `[kernel] archives`
  and force-loaded via `CONFIG_XNU_EXTRA_LD_ARCHIVES`.
- IOKit personality `patches/xnu/zzv-native-dwc3-personality.patch`.
- `package/BootLog.c` row `U` surfaces the probe over the UFS log.
- **Full Linux kernel build + link succeeds** with the driver, personality and log
  row in place; `NativeDwc3` + `native_dwc3_*` symbols are in the linked kernel.
- `package/ssh-tools/fajita-reboot.c` (source) + `scripts/build-ssh-userland.py`
  (cross-build + signing recipe; stops cleanly where the SDK/signer are absent).
- Two pre-existing build breakages on this branch fixed so the kernel builds at all:
  `BootReset.c` included `<stdio.h>` (fails under `-nostdlibinc`), and `BootLog.c`
  referenced weak_import diagnostics with no definition (ld64 refuses undefined
  weak_import in the static kernel link) — added `package/BootLogStubs.c`.

**Left (needs the device, an iOS SDK, or a host signer):**

- NativeDwc3 stage B (CDC-ACM gadget) and stage C (CDC-ECM + `ifnet`): design above;
  not written. The on-device probe results drive these (adopt vs. bring up PHY/clocks).
- Building + signing dropbear and `fajita-reboot`: needs an iOS SDK sysroot
  (`UNIXNU_IOS_SDK`) or macOS, plus `codesign`/`ldid`/`rcodesign`.
- Extending `make-owner-trust-bundle.py` with the userland cdhashes and the
  `org.blu-sharky.sshd.plist` launchd job (reusing the GPU-addon mount).

## On-device test steps (for the stage-A probe, which is what is built)

These need no USB channel yet — the probe reports over the existing UFS log.

1. Build the kernel (already proven on Linux):
   ```
   cd <UniXNU worktree>
   export UNIXNU_OUT=… UNIXNU_UEFI=/home/john/src/Mu-Silicium
   export UNIXNU_XNU_CLANG=~/unixnu-toolchain/clang/bin/clang
   export UNIXNU_LD64=~/unixnu-toolchain/ld64/bin/arm64-apple-darwin-ld
   export UNIXNU_IIG_OUTPUTS=/home/john/src/UniXNU-fajita/.cache/iig-outputs-21H461
   export PYTHONPATH=<UniXNU worktree>/scripts   # build-fajita-verified-root.py
   export FAJITA_STORAGE_CANDIDATE=… FAJITA_STORAGE_AUDIT=… FAJITA_VERIFIED_ROOT_CACHE=…
   ./unixnu defconfig fajita_defconfig -D /home/john/src/UniXNU-fajita-ssh
   ./unixnu build kernel  -D /home/john/src/UniXNU-fajita-ssh
   ```
2. Build the payload, enabling the probe and the log, e.g.:
   `unixnu build payload -D … --kernel-arg native-dwc3-probe=2 --kernel-arg sheng-log=1`
   (keep the board's other args; `native-dwc3-probe` is absent by default, so the
   probe is off unless you pass it).
3. Flash/boot as usual (you do this), then read the log with the sheng log reader
   over USB. Look for the `U` row:
   `U st=5 rc=0 mode=2 id=5533…. gctl=…. dcfg=…. dsts=…. alep=… cap=2 phy=1 gdsc=8000…. smmu=<slot>/<bank>`
   - `st=5 rc=0` = all stages passed; `id` top half `5533` = a DWC3 core;
   - `cap=2` = GCTL current mode is device; `phy=1` = QUSB2 PLL locked;
   - `gdsc` bit31 set = USB30 GDSC powered;
   - `smmu=slot/bank` (not `-1/-1`) = a bypass plan exists for stream `0x740`.
   A `U fail: …` or `U smmu: …` line names the first stage that did not pass — that
   is the input the stage-B gadget work needs (adopt the firmware mapping vs. bring
   the PHY/clocks up).

Report the `U` row back; it determines the stage-B bring-up path.
