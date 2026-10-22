# fajita: userland gap audit against sheng

sheng (UniXNU-sheng, Xiaomi Pad 6S Pro) boots the same 21H461 kernelcache and userland to SpringBoard. Everything sheng sets on
top of the base was reviewed for fajita on 2026-10-22, from its board.toml, sheng_defconfig, archive builders and kits/sheng (its
package/). Status: done, todo (needed), later (UI stage), n/a (sheng hardware).

## Already matching

| Item | fajita |
| --- | --- |
| Trust caches: the original static + System bundle (sha256 7416ca4f...) | done, `--trust-cache .cache/trust/original-static-system.bundle` |
| Owner cache mount, startup, union SystemOS | done (defconfig) |
| Owner boot manifest, Data mount, NativeRamBlock, packed Data seed | done; seed from data-seed.json |
| Prelink roots, pin, early_security_init | same values |
| PMGR stub (sheng-pmgr=2) | done (kits/fajita-pmgr) |

## Kernel archives

| sheng builder | What it carries | fajita |
| --- | --- | --- |
| build-boot-progress.py | kits/sheng BootProgress.c (boot-progress=), BootFaultProbe.c, **BootWatchdogPolicy.c** (sheng-disable-watchdogd: watchdogd off with no watchdog backend; sheng-lite: ~30 network/iCloud/photo jobs off; sheng-disable-jobs=), BootDiagExport.c | todo: port, Linux toolchain |
| build-native-legacy-display.py | NativeLegacyDisplay (CPU frames to the framebuffer), NativeHidInput, **NativeKeyStoreEvents** (sheng-aks: AppleKeyStore state events), LegacyDisplayProbe, HidInputProbe, KeyStoreProbe, BootProcessProbe, BootUiTraceProbe, BootLog, BootReset, TouchProbe + Novatek/GENI SPI touch, CpuFreqProbe, GpuPowerProbe, NativeGOPPresenter | done (display-port): scripts/build-native-legacy-display.py, Linux-capable; without BootLog/BootReset (build-boot-progress.py has fajita's), TouchProbe + Novatek/GENI SPI, CpuFreqProbe, GpuPowerProbe; kits/fajita TouchState.c stands in for the touch snapshot |
| build-arm-driver.py NativeVirtioBlock + bindings | the VirtIO storage path | covered by build-fajita-storage.py |
| NativeAdreno, adreno kits, accelerator | sheng's Adreno 740 | n/a (fajita renders on the CPU) |

## Boot arguments

| sheng | Effect | fajita |
| --- | --- | --- |
| -apfs_shared_datavolume | APFS shared Data volume mode | todo |
| ml-timeout-lock-panic-scale=40 | stretch lock-timeout panics (slow console, single core) | todo |
| sheng-staged-apps=1 | copy staged system apps into Data (patch zzy-staged-system-apps) | todo |
| sheng-disable-watchdogd=1, sheng-lite=1 | see BootWatchdogPolicy | todo (with the archive) |
| sheng-aks=1 | NativeKeyStoreEvents | done (display-port) |
| boot-progress=2, boot-ui-focus*= | progress/trace diagnostics | todo (with the archive) |
| offline-legacy-*=1, offline-legacy-phase=4, offline-mobile-graphics-*=1, offline-hid-*=1, offline-key-events=1 | display, HID and key paths | done (display-port); offline-legacy-unit-scale=1, not sheng's 2; no -v (v_display must stay 1) |
| offline-apfs-load=1 | load APFS at the offline checkpoint | check: APFS already loads |
| sheng-smp=1, armcpu-spi-ipi=1 | secondary cores, SPI IPIs | later (single core first) |
| sheng-cpufreq, sheng-gpu-*, sheng-fb-gpu | sheng clocks/GPU | n/a |
| sheng-vsync, sheng-banner | software vsync, diagnostic overlay | done (display-port): sheng-vsync=60, sheng-banner=0 |
| sheng-gpu-addon=1 | addon files into Data, PNG wallpaper mount | done (display-port): wallpaper only, scripts/make-fajita-wallpaper-addon.py |
| sheng-touch=4, sheng-touch-hid=1 | Novatek touch | n/a (fajita: Synaptics S3706, see research) |
| sheng-data-persist=2 | Data writes through to the end of userdata | later |
| serial=3 | UART console | n/a |

## Device tree ([payload.adt]) and payload

| sheng | fajita |
| --- | --- |
| ipad_identity = true (model iPad7,1, target-type J120: libMobileGestalt matches hw.product) | todo (make-fajita-adt.py supports --ipad-identity) |
| chip_id = "207", unique_chip_id = required (IOPlatformUUID seed) | todo: pick a fajita platform seed, keep it out of Git |
| persistent_data = true | later (with sheng-data-persist) |
| text_fixups: SpringBoard AWAttentionAwareness deadlock, PosterBoard file-protection (2), BTAudioHALPlugin loop | todo: same dyld cache, copy verbatim |
| skip_setup_tool = scripts/seed-data-skip-setup.py (--skip-setup) | later (mounts the seed read-write on macOS) |

## Known next failures from UniXNU's QEMU checkpoints

- tzinit needs a writable /private/var/db/timezone (fixed: db/timezone in data-seed.json).
- next recorded abort: `couldn't dlopen libobjc-trampolines.dylib` (scripts/check-native-ram-data-checkpoints.py).
