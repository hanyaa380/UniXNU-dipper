# fajita: touchscreen bring-up (Synaptics S3706B over GENI I2C)

Status 2026-10-22: rebased onto main (the CPU-rendered UI, NativeLegacyDisplay + NativeHidInput); built and linked into the fajita
kernel; host-tested against register models; **not yet run on the phone**. Default off: without `fajita-touch=<n>` nothing is mapped
or touched, and without `sheng-touch-hid=1` NativeHidInput publishes only its keyboard (Home) collection, as before. Research and sources: the touch/display research notes (S3706B RMI4 at 0x20 on i2c12 = QUP wrapper 1 SE4).

## Pieces

| File | What |
| --- | --- |
| `drivers/NativeGeniI2c/GeniI2c.[ch]` | GENI I2C master, FIFO mode, polled (the Linux `i2c-qcom-geni.c` FIFO path) |
| `drivers/NativeRmi4Touch/Rmi4.[ch]` | RMI4 PDT, F01, F12; S3706B fixed-PDT and no-descriptor (clone) fallbacks |
| `package/FajitaTouchBoard.h` | board constants with sources; pin words, DFS level choice, coordinate mapping, RMI-over-I2C link |
| `package/FajitaTouchProbe.cpp` | the staged probe, the poll thread, the `native_touch_*` globals and `native_touch_set_sink` |
| `scripts/build-fajita-touch.py` | `build/fajita-touch/libFajitaTouch.a` ([kernel] archives; force-loaded by `CONFIG_XNU_EXTRA_LD_ARCHIVES`) |
| `scripts/check-fajita-touch-host.py` | host tests: GENI sequencing, RMI4 parsing, board helpers, RMI over GENI end to end |
| `package/HidInputProbe.cpp` | (fajita kit copy of sheng's) also watches the digitizer's event service: open, DeviceOpenedByEventSystem, user clients, TouchDetectionMode (`native_hid_touch_service`) |
| `patches/NativeHidInput/0001-*.patch` | proposal for UniXNU's NativeHidInput (panel and axis boot args); not applied by any build, not needed (see below) |

`package/TouchState.c` (main's stand-in for the touch snapshot, which nothing wrote) is gone: libFajitaTouch.a defines the same
symbols and both are force-loaded, so they would collide.

The probe starts from the boot progress monitor (`BootProgress.c`, once a second, after IOKit's platform expert exists); fajita's
`LegacyDisplayProbe.cpp` does not call it. **So `boot-progress=2` (the board default) must stay on the command line.**

## Stages and boot arguments

Add to `kernel_args` one stage at a time; each stage runs all lower ones first, and the first failure stops the sequence.

| Boot arguments | Stage | Writes to hardware |
| --- | --- | --- |
| `boot-progress=1 sheng-log=1 fajita-touch=1` | 1: read GCC votes, branch states, S4 DFS levels, the five pins; the engine registers only if the QUP clocks already run | none |
| `... fajita-touch=2` | 2: vote GCC 0x5200c bits 20/21/26 (OR, never cleared), wait for the branches, pins 49/50 to qup12, 125 input pull-up, 88 high (1.8 V), reset 99 high 10 ms, low 10 ms, high, 200 ms; GENI init (FIFO, 8-bit packing), 400 kHz from the TCXO DFS level; ACK probe: write 0xe9 to 0x20, up to 5 tries | GCC vote, TLMM 5 pins, SE4 |
| `... fajita-touch=3` | 3: PDT scan of page 0 (fixed S3706B table if the first entry is empty), F01 identity, F01 ctrl0 = normal + NoSleep + Configured, interrupt enable = F12 abs + F01, F12 descriptors or the hardcoded layout, Control 8 maxima | touch controller registers |
| `... fajita-touch=4 sheng-touch-hid=1` | 4: poll F01 status/interrupts at 100 Hz, read F12 Data1 when its bit is set, publish contacts; NativeHidInput adds its digitizer collection and sends them to the event driver (the board already has `offline-hid-input=1 offline-hid-home=1`) | none new |

Options (all optional):

| Argument | Default | Effect |
| --- | --- | --- |
| `fajita-touch-power=0` | 1 | stage 2 leaves GPIO 88 and 99 alone (only clocks, I2C pins, ACK probe) |
| `fajita-touch-hz=100000` | 400000 | bus speed: 100000, 400000 or 1000000 |
| `fajita-touch-pdt=1` | 0 | use the S3706B fixed PDT (0xe9 F34, 0xe3 F01, 0xdd F12, 4-byte reads) without scanning |
| `fajita-touch-rmi-reset=1` | 0 | F01 software reset (and 200 ms) before configuring |
| `fajita-touch-max-x=N`, `-max-y=N` | F12 Control 8, else 1080 x 2340 | controller coordinate maxima used for the mapping |
| `fajita-touch-orient=B` | 0 | bit 0 swap axes, bit 1 mirror X, bit 2 mirror Y |
| `fajita-touch-rate=Hz` | 100 | poll rate, 10..250 |
| `fajita-touch-irq=1` | 0 | GPIO 125 falling edge through the TLMM summary SPI 208 (INTID 240), 100 ms timeout fallback |
| `fajita-touch-poll-all=1` | 0 | read F12 every poll even without its interrupt bit (for a controller that never sets it) |
| `fajita-touch-letterbox=1` | 0 | with a logical display: drop a contact that begins in the letterbox (default: clamp it to the image edge) |
| `fajita-touch-view=0` | 1 | ignore the display's logical mode; map onto the whole panel |

### Coordinates

Controller (0..max) -> panel pixel (1080 x 2340, after `fajita-touch-orient`) -> logical pixel -> published units (30480 x 20320
across the logical display) -> NativeHidInput's 0..32767 -> IOHIDEventDriver's 0..1 of the screen backboardd knows.
The panel -> logical step is the display's `fajita-display-logical=<W>x<H>` mode (fajita's NativeMobileDisplay copy, `fajita_display_logical`
in `FajitaDisplayLogical.h`): with it, the image is fitted and centred on the panel (2048x2732 -> 1080x1441 at y=449) and a touch on
the image maps to the logical pixel under it (pixel centres, the display's own arithmetic); a touch in the letterbox is clamped to the
nearest image edge (or dropped with `fajita-touch-letterbox=1`). Without the mode (or before the display has published it: `valid` 0)
the mapping is the identity onto the panel. The struct is read each frame through a weak zero definition in libFajitaTouch.a, which
gives way to the display's strong one; `FajitaDisplayLogical.h` is included when the tree has it (`__has_include`), else the same
layout is declared in FajitaTouchProbe.cpp. Pure helpers and their tests: `ftb_map_panel`, `ftb_panel_to_logical`,
`ftb_logical_to_published` in FajitaTouchBoard.h.

## What the log shows

Each stage writes rows to `fajita_touch_rows` and IOLog (`fajita-touch: ...`, on the `-v` console and in the message buffer).
BootLog.c writes them to every UFS log slot under `[fajita touch]`, after the summary row; the summary row is also page row 42.
`st` is the step that failed and `e` the error (negative errno): see the table at the end.

Success, stage by stage (values are illustrative):

```
TCH fajita s4/4 st4 e0 ack0 pdtS f12 1080x2340 fr311 c1 15012,10044 err0 hid0
FT1 clk vote00300000 m00000000 s00000002 s480000000 pll80000000/1 dfs1 cfg00000105 xoL2
FT1 pin sda004/3 scl004/3 vio200/3 rst3c3/3 int1c3/1/e0
FT1 se not read: QUP clocks off (m0 s0 s41)            <- normal if the firmware did not use i2c12
FT2 vote04300000 m00000000 s00000002 s400000000 10us pins004/004 vio3 rst3 int1 fw0327 d16 L2 400kHz ack0 x1
FT3 pdt0 scan e9:4c4b2300 4134 irq4/1 F34@e9 q4c c23 d0 i0 F01@e3 ... i1 F12@dd ... i2
FT3 f01 st81 m01 q1xx id's3706...' c0 00>84 ien06 | f12 q0xx desc d1 80@0 n10 rd80 max1080x2340 map1080x2340 o0
FT4 polls1000 irq37 fr37 cf30 c1 dn3 lb0/0 raw540,1170 ext x101..998 y88..2290 st01 err0 back0 recfg0 gpio0/0
FT5 hid up1 chk40 rep37 res0 n37 edge1/3/1 svc40 open1 eso1 uc2 tdm1 pub1 mapL2048x2732
```

- FT1: `xoL<n>` must be 0..7 (the DFS level giving 19.2 MHz); `xoL-1` means stage 2 will stop (step 21). In the pin row,
  `ctl/io` per pin: function in ctl bits 5:2 (`004` = qup12), io bit 0 the line level. `vio.../3` (out and in high) means the 1.8 V
  enable was already on.
- FT1 se: `proto3` (I2C firmware) and `ifdis0` (FIFO usable) are what stage 2 needs; `depth16` the FIFO.
- FT2: `ack0` is the ACK from 0x20: the controller is powered and out of reset.
- FT3: `pdt0 scan` (or `fixed` for a clone), `F01` and `F12` present, `id'...'` readable, `c0 ..>84`, `desc` or `hardcoded`.
- FT4: `polls` the 100 Hz loop is alive; `irq` polls whose F01 interrupt status had the F12 bit; `fr` F12 reads; `cf` frames with at
  least one contact; `c` contacts in the last frame; `dn` contacts begun (one per finger down); `lb` of them begun in the letterbox /
  contact-frames dropped for it; `raw` controller coordinates of the first contact and `ext` the extent seen so far (use it to set
  `fajita-touch-max-x/-y`); `st` F01 status (`00`/`01` fine, bit 7 = unconfigured, i.e. the controller reset); `err` bus errors.
  `gpio a/b` interrupts / spurious with `fajita-touch-irq=1`. Success: `polls` grows ~100/s, `irq`/`fr`/`cf`/`dn` grow only while
  touching, `err0`.
- FT5 (the HID side, once a second; also printed to IOLog every 10 s): `up1` NativeHidInput started; `chk` its once-a-second
  event-driver checks; `rep` touch reports the event driver took and `res` the last IOReturn (`0` = success); `n` reports counted here
  (`rep` and `n` should equal and grow with `fr` while touching); `edge a/b/c` the edge wrapper installed (1), contacts it saw begin,
  of them marked from-edge (0x800); `svc` checkpoints that found the IOHIDEventDriver; `open1` the event driver is open (backboardd's
  event system took it), `eso1` DeviceOpenedByEventSystem is set on it, `uc` its user clients (expect >= 1); `tdm` the
  TouchDetectionMode backboardd last set (`1` report touches, `fe` wake on tap with the display off, `ff` off, `-` never set);
  `pub1` Built-In/DisplayIntegrated/Primary Digitizer-Touch Screen published on the service; `mapL<W>x<H>` touches go through the
  display's logical mode, `mapP1080x2340` the identity. The summary row `TCH` repeats `dn`, `hid` (= `n`) and `open`.
- Reading a failure: `rep0` with `fr>0` = NativeHidInput is not consuming (missing `sheng-touch-hid=1`, or `chk` stays 0: no event
  driver); `res` nonzero = the event driver refused the report; `open0 uc0` = backboardd never opened the service (it logs "still
  waiting for a main screen digitizer"); `open1` with touches not reaching apps = look at `tdm` and the coordinates (`raw`/`ext`,
  `fajita-touch-orient`).

Failures (`TCH ... st<step> e<error>` and the `FTE` row with the I2C counters and the engine registers at the failure):

| Row | Meaning |
| --- | --- |
| `st1 e-12` | a register window could not be mapped |
| `st11` with a hang | the engine read stalled although the branches looked on (should not happen; report the FT1 clk row) |
| `FT2 ... CLOCKS OFF`, `st2 e-110` | a voted branch never ran (the wrapper core clock is not GCC-controlled on SDM845) |
| `FT2 ... no 19.2MHz DFS level`, `st21 e-22` | the S4 RCG offers no TCXO level (FT1 clk row has the DFS state) |
| `INIT-93`, `st23` | SE4 does not run I2C firmware (proto != 3) |
| `INIT-95`, `st23` | FIFO interface fused off: GPI DMA would be needed (as sheng's SPI needed SE DMA) |
| `ack-6 x5`, `st25 e-6`, `FTE no ACK at 0x20 (LDO28 off?)` | nothing acknowledges 0x20: VDD (LDO28) off, or held in reset; see "LDO28" |
| `ack-110` / `ack-71` | timeout or bus protocol error: lines held low (look at `ios` in FTE), clock not running |
| `pdt-6` / `pdt-19`, `st31` | PDT read refused / no F01 or F12 (try `fajita-touch-pdt=1`) |
| `f01 ... BOOTLOADER`, `st32 e-16` | the controller runs its bootloader, not the touch firmware |
| `f12 ... ERR-19`/`-5`, `st34` | no Data1 / malformed F12 descriptors |
| `FT4 ... err N back M` | bus errors while polling; after 20 in a row the loop drops to one try a second |

## NativeHidInput integration (for the display/HID kit)

libFajitaTouch.a provides exactly the contract sheng's TouchProbe.cpp gives NativeHidInput, so NativeHidInput needs no change:

- Defined here, read by `drivers/NativeHidInput/NativeHidInput.cpp`: `native_touch_seq`, `native_touch_count`
  (`volatile uint32_t`), `native_touch_xs`, `native_touch_ys`, `native_touch_ids` (`volatile int[12]`), and
  `native_touch_hid_reports` (`volatile uint32_t`, incremented by NativeHidInput).
- Defined here, called by NativeHidInput (`weak_import`): `void native_touch_set_sink(void (*)(void *), void *)`. The sink runs on
  the touch thread after each publication; NativeHidInput's `touchNotify` hands the report to its work loop.
- Defined here, called by the board's late probe: `void native_touch_probe(void)` (sheng's `LegacyDisplayProbe.cpp` calls it from
  `native_legacy_display_late_probe`; keep that call). It is idempotent, so the boot progress monitor calling it as well is harmless.
- Not defined here: `native_touch_edge_stats` (NativeHidInput's), the HID probe counters (HidInputProbe.cpp's).
- Units: X 0..30479 spans the panel width, Y 0..20319 its height (NativeHidInput's `transformCoordinate(xs[i],30480)` and
  `(ys[i],20320)`), ids are the F12 object slots 0..9, the published set is complete each time (a missing id is a lift).
- HID boot arguments as on sheng: `offline-hid-input=1 offline-hid-home=1 sheng-touch-hid=1` (and `sheng-touch-split=1` for the
  separate "SPI" digitizer device). sheng reaches SpringBoard with touch on exactly this (combined device, no split).

### What backboardd needs, against the HackiOS reference

HackiOS (iOS 16.5, user-space injection, hackios-xnu-userland `userland/src/inject.c`) shows what backboardd requires of a main
screen digitizer. The kernel path here meets it as follows:

| Requirement | Here |
| --- | --- |
| Primary usage page 0x0D / usage 0x04 | the device's primary usage is the keyboard (the combined descriptor, needed for matching); HidInputProbe.cpp publishes `PrimaryUsagePage` 0x0D / `PrimaryUsage` 4 on the event service and its interface once they matched (backboardd's `BKHIDDirectTouchEventProcessor` drops digitizer events from any other service); `DeviceUsagePairs` (published by IOHIDEventService) lists the Touch Screen collection |
| `Built-In`, `DisplayIntegrated` | set on the device by NativeHidInput (`sheng-touch-hid=1`), and on the event service and interface by HidInputProbe.cpp |
| `Transport` "SPI" | "Virtual" on the combined device ("SPI" only with `sheng-touch-split=1`); sheng shows it is not required on 21H461 |
| Opened by the event system | observed, not forced: FT5 `open`/`eso`/`uc` |
| Hand parent + Finger children, `IsBuiltIn`/`DisplayIntegrated` on events | the original IOHIDEventDriver builds the collection (hand) event with one finger child per contact from the Touch Screen report, and takes the built-in/display-integrated flags from the service |
| Edge flag 0x800 for contacts starting at an edge | NativeHidInput's `edge::` dispatch wrapper (48 px of 3048 x 2032 as fractions: 1.6% of the width, 2.4% of the height; on 2048x2732 logical that is 32 x 64 px, on the bare panel 17 x 55 px). A swipe that starts on the black bar below a logical image is clamped onto the bottom edge, so it counts as an edge start |
| Coordinates 0..1 | published units -> 0..32767 report fields -> the event driver's 0..1 |
| `TouchDetectionMode` 0xfe (wake on tap) | observed only (FT5 `tdm`); the controller keeps reporting, nothing wakes the display |
| Buttons: consumer 0x30/0x40/0xE9/0xEA | only Consumer Menu (Home, report 2, `offline-hid-auto-home`); power and volume keys (PM8998 PON / GPIO) are not wired |

Linking: the display archive (scripts/build-native-legacy-display.py) carries NativeHidInput and HidInputProbe but no touch snapshot;
`CONFIG_XNU_EXTRA_LD_ARCHIVES` force-loads libNativeDwc3.a and libFajitaTouch.a (append, do not replace); `build-fajita-touch.py`
runs after the display archive in `[kernel] archives`. The touch stand-ins in `BootProgressStubs.c` (generated,
scripts/gen-boot-progress-stubs.py; `fajita_touch_rows`, `native_touch_probe` and `native_hid_touch_service` added) are weak and stay.

### Where NativeHidInput is hardwired to sheng's panel (no fajita copy needed)

None of this needs a change for fajita: the probe publishes in sheng's units, so the axis maxima fit, and the edge zone is a fraction
of the screen. A fajita copy of NativeHidInput (drivers/NativeHidInput in this repository overrides the base module) would only be
worth it to tune the edge zone or the Transport string.

- `drivers/NativeHidInput/NativeHidInput.cpp:60-61`: `kEdgeX=48*65536/3048, kEdgeY=48*65536/2032` (edge zone of 48 px of a
  3048 x 2032 panel; on fajita that is 17 px at the long edges and 55 px at the short ones instead of 48).
- `NativeHidInput.cpp:159-163`: `transformCoordinate(xs[i],30480)` and `transformCoordinate(ys[i],20320)` (the axis maxima).
- Diagnostics only: `package/BootProgress.c` `touch_overlay_locked` draws a 30480 x 20320 (landscape) pad, and `package/BootLog.c`
  labels the stroke extent "px of 3048x2032".

Proposal: `patches/NativeHidInput/0001-panel-and-axis-boot-args.patch` (applies to UniXNU `drivers/NativeHidInput`) adds
`native-touch-panel-w/-h`, `native-touch-edge-px` and `native-touch-axis-x/-y`, defaults unchanged. For fajita:
`native-touch-panel-w=1080 native-touch-panel-h=2340 native-touch-edge-px=64` (the same ~4 mm at 402 ppi); the axes stay as they
are, because this probe publishes in sheng's units. Not compiled here (it needs the generated `OriginalHidDeviceABI.h`).

## Open risks

- **LDO28.** VDD of the controller is PM8998 LDO28, an RPMh VRM rail that is not `regulator-boot-on`; no UEFI driver touches it.
  If XBL/ABL leave it off, stage 2 ends with `ack-6 x5` and `FTE no ACK at 0x20 (LDO28 off?)`, with `vio3 rst3` showing the 1.8 V
  enable and reset driven. Nothing read-only can tell LDO28's state from the apps processor (no PMIC or RPMh read path is used).
  The RPMh step is designed but deliberately not implemented: a later stage behind its own argument (`fajita-touch-ldo=1`), run
  before stage 2's reset: look up `ldoa28` in cmd-db (`aop-cmd-db-mem@85fe0000`, sheng's `ShengGmuCommandDb.c` parser), take an
  idle ACTIVE TCS of the apps RSC (drv 2 at 0x179e0000, TCS offset 0xd00, RSC v2 layout of `rpmh-rsc.c`), write three commands
  (voltage 2856 mV at +0x0, mode 7 = HPM at +0x8, enable 1 at +0x4) and wait for completion. Risk high: AOP/RPMh state is shared
  with firmware, a TCS the firmware still owns or a wrong address can hang the SoC. Cheaper alternative first: a vote from UEFI
  before ExitBootServices, if the PMIC/RPMh protocol of the binary RpmhDxe can be used.
- **Hypervisor (QHEE) and bus faults.** No SMC/HVC is issued. The GCC vote (0x15200c), the TLMM pin registers (none of them in
  the TZ-reserved ranges 0-3 and 81-84) and SE4 are the registers Linux writes at EL1 under the same hypervisor, so stage-2 traps are
  not expected; but no XNU driver on fajita has written GCC yet, so stage 2's first write is the first test. Reading SE4 or the
  wrapper with their clocks gated would stall the interconnect or raise an SError (not a hypervisor trap): stage 1 reads them
  only when the branches report running. `fajita-touch-irq=1` relies on ArmCpu's GIC programming of SPI 208; if the firmware
  left another TLMM pin interrupt enabled to the apps processor, the shared summary interrupt could fire without our pin and
  storm (counted in `gpio .../<spurious>`), which is why interrupt mode is opt-in.
- **Clone panels.** Replacement panels often carry controllers that implement only what the OnePlus downstream driver reads. The
  probe falls back on its own to the postmarketOS S3706B table when the first PDT entry is empty (4-byte reads of 0xe9/0xe3/0xdd,
  no other pages), and to the hardcoded F12 layout (Data1 at the data base, ten 8-byte objects, an 88-byte read) when F12 has no
  register descriptors; `fajita-touch-pdt=1` forces the table. A clone reports no axis maxima, so the 1080 x 2340 default is used
  until `fajita-touch-max-x/-y` are set from the FT4 `ext` values; the downstream driver masks coordinates to 12 bits, so a clone
  that puts flags in the high nibble would need that mask too.
- **Orientation.** RMI X/Y are mapped directly to the portrait 1080 x 2340 GOP; sheng needed a Y mirror, fajita may need
  `fajita-touch-orient`.
