// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of the fajita touchscreen: Synaptics S3706B (RMI4) at 0x20 on i2c12 (QUP wrapper 1 SE4).
// `fajita-touch=<n>`:
//   1  read only: GCC votes, branch and DFS state, the five touch pins, and (only when the QUP clocks already run) the engine's
//      firmware revision, FIFO geometry and FIFO-disable fuse
//   2  vote the QUP clocks, claim the pins, raise the 1.8 V enable (GPIO 88), pulse reset (GPIO 99), GENI I2C init at 400 kHz,
//      then probe 0x20 for an ACK (a write of the PDT pointer, nothing else)
//   3  read the RMI4 PDT (or the S3706B fixed table), F01 identity, F01 NoSleep/Configured and interrupt enable, F12 geometry
//   4  poll F01/F12 at fajita-touch-rate Hz (default 100) and publish the contacts for NativeHidInput
// Every stage writes its result to fajita_touch_rows (the UFS log, BootLog.c) and IOLog; the first failure stops the sequence and
// is kept in native_touch_step / native_touch_error. The publication contract (native_touch_* seqlock, native_touch_set_sink)
// is the one sheng's TouchProbe.cpp gives NativeHidInput, so the digitizer needs no change. Board facts are in FajitaTouchBoard.h.
// Nothing here has run on the device yet.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GeniI2c.h"
#include "Rmi4.h"
#include "FajitaTouchBoard.h"
}
// The display's logical mode (fajita-display-logical=<W>x<H>): NativeLegacyDisplay of fajita's NativeMobileDisplay copy exports
// the logical size and the image rectangle on the panel. Until that copy is in the tree this file carries the same layout; the
// weak zero definition below (valid 0: the identity mapping) gives way to the display's strong one when it is linked.
#if __has_include("FajitaDisplayLogical.h")
#include "FajitaDisplayLogical.h"
#else
struct fajita_display_logical {
    uint32_t valid;
    uint32_t logical_width, logical_height;
    uint32_t panel_width, panel_height;
    uint32_t image_x, image_y;
    uint32_t image_width, image_height;
};
extern "C" volatile struct fajita_display_logical fajita_display_logical;
#endif
extern "C" __attribute__((weak)) volatile struct fajita_display_logical fajita_display_logical = {};

#define TOUCH_SLOTS 12   // the array size NativeHidInput and BootProgress declare
// fajita_touch_rows: 0-2 FT1, 3 FT2, 4-5 FT3, 6 FT4, 7 FTE (the last error), 8 FT5 (the HID side). BootLog.c declares the same size.
#define FTB_ROWS 10

extern "C" {
// Read by package/BootProgress.c (page rows, touch pad overlay) and package/BootLog.c (UFS log); the types are those of their
// extern declarations. They replace the weak stand-ins of BootProgressStubs.c.
char native_touch_rows[4][120];
char fajita_touch_rows[FTB_ROWS][120];
volatile uint32_t native_touch_loop_step;
volatile unsigned native_touch_stage_req;
volatile uint32_t native_touch_stage_done, native_touch_step, native_touch_frames, native_touch_valid, native_touch_contacts,
    native_touch_hid_reports;
volatile int native_touch_error;
volatile int native_touch_last_x = -1, native_touch_last_y = -1;
// Latest finger set for NativeHidInput: a seqlock (odd while written). Units: X 0..30479 = panel width, Y 0..20319 = panel height
// (the axis maxima NativeHidInput normalises with), ids = F12 object slot.
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[TOUCH_SLOTS], native_touch_ys[TOUCH_SLOTS], native_touch_ids[TOUCH_SLOTS];
// Finished strokes {start ms, duration ms, first x, first y, last x, last y, frames} and the extent of every published contact
// {min x, max x, min y, max y}, in the published units (BootLog.c prints them divided by 10).
volatile int32_t native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_extent[4] = {0x7fffffff, -1, 0x7fffffff, -1};
// [0] 0 not armed, 1 armed, 2 attach failed; [1] interrupts; [2] spurious (summary without our pin); [3] attach status.
volatile uint32_t native_touch_irq_stats[4];
// Raw controller coordinates of the last contact and their extent (calibration: fajita-touch-max-x/-y).
volatile int32_t fajita_touch_raw[6] = {-1, -1, 0x7fffffff, -1, 0x7fffffff, -1};
// Contacts begun (a slot id that was not down in the previous frame), of them begun in the letterbox, and frames with a contact.
volatile uint32_t native_touch_downs, fajita_touch_letterbox[2], fajita_touch_contact_frames;
// The HID side, all read only here (weak: a kernel without the display archive still links, through BootProgressStubs.c):
// HidInputProbe.cpp's NativeHidInput event counters (index 2 started, 3 event-driver checks, 6 touch reports handled, with the
// IOReturn of the last), NativeHidInput's edge statistics, and HidInputProbe.cpp's view of the event service backboardd opens.
extern volatile uint32_t native_hid_input_events[8] __attribute__((weak_import));
extern volatile int native_hid_input_results[8] __attribute__((weak_import));
extern volatile uint32_t native_touch_edge_stats[4] __attribute__((weak_import));
extern volatile uint32_t native_hid_touch_service[8] __attribute__((weak_import));
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    if (region.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
// The engine is polled with 1 us waits; IODelay would block the thread for each (see sheng's TouchProbe), so short waits spin on
// the architectural counter.
void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}
void sleepMs(void *, unsigned ms) {IOSleep(ms);}

enum Pin {kSda, kScl, kVio, kRst, kInt, kPins};
const unsigned kGpio[kPins] = {FTB_GPIO_SDA, FTB_GPIO_SCL, FTB_GPIO_VIO, FTB_GPIO_RST, FTB_GPIO_INT};

struct Options {
    unsigned stage, hz, power, pdt, rmiReset, maxX, maxY, orient, rate, irq, pollAll, letterbox, view;
};

struct Session {
    Options opt;
    Region gccLow, gccBranch, gccVote, pin[kPins], wrapper, se;
    // stage 1
    uint32_t vote, gpll0Vote, gpll0Mode, haltM, haltS, haltS4, dfsr, rcgCfg, perfCfg[FTB_DFS_LEVELS], perfM[FTB_DFS_LEVELS],
        perfN[FTB_DFS_LEVELS];
    int xoLevel;
    uint32_t ctl[kPins], io[kPins], icfg;
    int seRead;
    uint32_t fw, ifdis, hw0, geniStatus, wrapVer, clkSel, serClk, scl, dmaMode, seIrqEn, ios;
    // stage 2
    uint32_t voteAfter, waitedUs, ctlAfter[kPins], ioAfter[kPins];
    int ackRc;
    unsigned ackTries;
    struct geni_i2c i2c;
    struct ftb_rmi_link link;
    // stage 3
    struct rmi_dev rmi;
    struct ftb_map map;
    // stage 4
    uint32_t polls, irqHits, frames, busErrors, backoffs, reconfigs, lastIrq;
    uint32_t liveIds, droppedIds;   // F12 slots down in the previous frame; of them, those dropped for starting in the letterbox
    struct ftb_view view;           // the display mapping of the last frame
    uint8_t lastStatus;
    int lastRc;
};

void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}

// snprintf returns the length it would have written; clamp a running offset before the next append.
struct Out {
    char *p; size_t cap, n;
    Out(char *buffer, size_t size) : p(buffer), cap(size), n(0) {p[0] = 0;}
    void add(const char *text) {add("%s", text);}
    template <typename A0, typename... A> void add(const char *fmt, A0 a0, A... args) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a0, args...);
        if (w < 0) return;
        n = (n + static_cast<size_t>(w) >= cap) ? cap - 1 : n + static_cast<size_t>(w);
    }
};

void logRow(unsigned i) {if (fajita_touch_rows[i][0]) IOLog("fajita-touch: %s\n", fajita_touch_rows[i]);}

void summary(const Session &s) {
    const struct rmi_dev &r = s.rmi;
    Out o(native_touch_rows[0], sizeof native_touch_rows[0]);
    o.add("TCH fajita s%u/%u st%u e%d ack%d", native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
          s.ackRc);
    if (native_touch_stage_done >= 3) o.add(" pdt%c f12 %ux%u%s", r.pdt_mode == RMI_PDT_FALLBACK ? 'F' : 'S', s.map.max_x, s.map.max_y,
                                            r.f12_hardcoded ? " hc" : "");
    o.add(" fr%u c%u dn%u %d,%d err%u hid%u open%u", native_touch_frames, native_touch_contacts, native_touch_downs,
          native_touch_last_x, native_touch_last_y, s.busErrors, native_touch_hid_reports,
          &native_hid_touch_service ? native_hid_touch_service[1] : 0);
    // The other three rows mirror the latest stage rows for the diagnostic page.
    const unsigned latest = native_touch_stage_done >= 4 ? 6 : native_touch_stage_done >= 3 ? 5 : native_touch_stage_done >= 2 ? 3 : 0;
    strlcpy(native_touch_rows[1], fajita_touch_rows[latest], sizeof native_touch_rows[1]);
    strlcpy(native_touch_rows[2], fajita_touch_rows[native_touch_stage_done >= 3 ? 4 : 2], sizeof native_touch_rows[2]);
    strlcpy(native_touch_rows[3], fajita_touch_rows[7], sizeof native_touch_rows[3]);
}

void errorRow(const Session &s, const char *what) {
    const struct geni_i2c &g = s.i2c;
    Out o(fajita_touch_rows[7], sizeof fajita_touch_rows[7]);
    o.add("FTE %s st%u e%d i2c x%u nack%u to%u err%u rec%u first%d", what, native_touch_step, native_touch_error, g.xfers, g.nacks,
          g.timeouts, g.errors, g.recoveries, g.first_err);
    if (g.snap_valid) o.add(" irq%08x gs%08x gp%x ios%x tx%x rx%x cmd%08x", g.snap_irq, g.snap_status, g.snap_gp_len, g.snap_ios,
                            g.snap_txfs, g.snap_rxfs, g.snap_cmd);
}

// ---- stage 1: read only ----
bool snapshot(Session &s) {
    native_touch_step = 1;
    if (!mapRegion(FTB_GCC_BASE, 0x1000, s.gccLow) || !mapRegion(FTB_GCC_BASE + 0x18000, 0x1000, s.gccBranch) ||
        !mapRegion(FTB_GCC_BASE + 0x52000, 0x1000, s.gccVote)) {fail(1, -12); return false;}
    for (unsigned i = 0; i < kPins; i++)
        if (!mapRegion(FTB_TLMM_BASE + ftb_pin_offset(kGpio[i]), FTB_PIN_STRIDE, s.pin[i])) {fail(1, -12); return false;}
    s.vote = rd32(s.gccVote.va, FTB_GCC_VOTE & 0xfff);
    s.gpll0Vote = rd32(s.gccVote.va, FTB_GCC_GPLL0_VOTE & 0xfff);
    s.gpll0Mode = rd32(s.gccLow.va, FTB_GCC_GPLL0_MODE);
    s.haltM = rd32(s.gccBranch.va, FTB_HALT_M_AHB & 0xfff);
    s.haltS = rd32(s.gccBranch.va, FTB_HALT_S_AHB & 0xfff);
    s.haltS4 = rd32(s.gccBranch.va, FTB_HALT_S4 & 0xfff);
    s.dfsr = rd32(s.gccBranch.va, (FTB_S4_RCG + FTB_RCG_DFSR) & 0xfff);
    s.rcgCfg = rd32(s.gccBranch.va, (FTB_S4_RCG + FTB_RCG_CFG) & 0xfff);
    for (unsigned l = 0; l < FTB_DFS_LEVELS; l++) {
        s.perfCfg[l] = rd32(s.gccBranch.va, (FTB_S4_RCG + FTB_RCG_PERF_CFG(l)) & 0xfff);
        s.perfM[l] = rd32(s.gccBranch.va, (FTB_S4_RCG + FTB_RCG_PERF_M(l)) & 0xfff);
        s.perfN[l] = rd32(s.gccBranch.va, (FTB_S4_RCG + FTB_RCG_PERF_N(l)) & 0xfff);
    }
    s.xoLevel = ftb_xo_level(s.dfsr, s.perfCfg, s.perfM, s.perfN, FTB_DFS_LEVELS, s.rcgCfg);
    for (unsigned i = 0; i < kPins; i++) {s.ctl[i] = rd32(s.pin[i].va, 0); s.io[i] = rd32(s.pin[i].va, 4);}
    s.icfg = rd32(s.pin[kInt].va, 8);
    {
        Out o(fajita_touch_rows[0], sizeof fajita_touch_rows[0]);
        o.add("FT1 clk vote%08x m%08x s%08x s4%08x pll%08x/%x dfs%x cfg%08x xoL%d", s.vote, s.haltM, s.haltS, s.haltS4, s.gpll0Mode,
              s.gpll0Vote & 1, s.dfsr, s.rcgCfg, s.xoLevel);
        Out p(fajita_touch_rows[1], sizeof fajita_touch_rows[1]);
        // ctl/io per pin: 49 sda, 50 scl, 88 vio (1.8 V enable), 99 rst, 125 int (+ its INTR_CFG). With the 1.8 V rail up and the
        // controller powered, the idle bus and the interrupt line read high (io bit 0).
        p.add("FT1 pin sda%03x/%x scl%03x/%x vio%03x/%x rst%03x/%x int%03x/%x/%02x", s.ctl[kSda] & 0x3ff, s.io[kSda] & 3,
              s.ctl[kScl] & 0x3ff, s.io[kScl] & 3, s.ctl[kVio] & 0x3ff, s.io[kVio] & 3, s.ctl[kRst] & 0x3ff, s.io[kRst] & 3,
              s.ctl[kInt] & 0x3ff, s.io[kInt] & 3, s.icfg & 0xff);
    }
    // The engine and wrapper registers are only safe to read while their clocks run: an access to a gated QUP stalls the bus or
    // raises an external abort. Stage 1 never enables anything, so it reads them only if the firmware left them on.
    const bool clocks = ftb_branch_running(s.haltM) && ftb_branch_running(s.haltS) && !(s.haltS4 & FTB_CBCR_CLK_OFF);
    Out e(fajita_touch_rows[2], sizeof fajita_touch_rows[2]);
    if (!clocks) {e.add("FT1 se not read: QUP clocks off (m%x s%x s4%x)", s.haltM >> 31, s.haltS >> 31, s.haltS4 >> 31); return true;}
    native_touch_step = 11;   // a hang right here means the engine access stalled although the branches looked on
    if (!mapRegion(FTB_QUP_WRAPPER_BASE, 0x1000, s.wrapper) || !mapRegion(FTB_SE_BASE, FTB_SE_SIZE, s.se)) {fail(11, -12); return false;}
    s.seRead = 1;
    s.fw = rd32(s.se.va, GI2C_FW_REVISION_RO);
    s.ifdis = rd32(s.se.va, GI2C_IF_DISABLE_RO);
    s.hw0 = rd32(s.se.va, GI2C_HW_PARAM_0);
    s.geniStatus = rd32(s.se.va, GI2C_GENI_STATUS);
    s.wrapVer = rd32(s.wrapper.va, GI2C_QUP_HW_VER);
    s.clkSel = rd32(s.se.va, GI2C_CLK_SEL);
    s.serClk = rd32(s.se.va, GI2C_SER_M_CLK_CFG);
    s.scl = rd32(s.se.va, GI2C_SCL_COUNTERS);
    s.dmaMode = rd32(s.se.va, GI2C_DMA_MODE_EN);
    s.seIrqEn = rd32(s.se.va, GI2C_IRQ_EN);
    s.ios = rd32(s.se.va, GI2C_IOS);
    e.add("FT1 se fw%04x proto%u ifdis%x hw0%08x depth%u st%08x wrap%08x clk%x/%x scl%08x dma%x ios%x", s.fw & 0xffff, (s.fw >> 8) & 0xff,
          s.ifdis, s.hw0, (s.hw0 >> 16) & 0x3f, s.geniStatus, s.wrapVer, s.clkSel, s.serClk, s.scl, s.dmaMode, s.ios);
    native_touch_step = 1;
    return true;
}

// ---- stage 2: clocks, pins, power, reset, engine, ACK ----
void pinWrite(Session &s, unsigned pin, uint32_t ctl) {wr32(s.pin[pin].va, 0, ctl);}
void pinOut(Session &s, unsigned pin, bool high) {wr32(s.pin[pin].va, 4, high ? FTB_IO_OUT : 0);}

bool enable(Session &s) {
    native_touch_step = 2;
    Out o(fajita_touch_rows[3], sizeof fajita_touch_rows[3]);
    // A shared vote (other masters keep theirs); nothing is ever un-voted.
    wr32(s.gccVote.va, FTB_GCC_VOTE & 0xfff, rd32(s.gccVote.va, FTB_GCC_VOTE & 0xfff) | FTB_VOTE_ALL);
    bool on = false;
    for (s.waitedUs = 0; s.waitedUs < 20000 && !on; s.waitedUs += 10) {
        on = ftb_branch_running(rd32(s.gccBranch.va, FTB_HALT_M_AHB & 0xfff)) && ftb_branch_running(rd32(s.gccBranch.va, FTB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccBranch.va, FTB_HALT_S4 & 0xfff) & FTB_CBCR_CLK_OFF);
        if (!on) IODelay(10);
    }
    s.voteAfter = rd32(s.gccVote.va, FTB_GCC_VOTE & 0xfff);
    s.haltM = rd32(s.gccBranch.va, FTB_HALT_M_AHB & 0xfff);
    s.haltS = rd32(s.gccBranch.va, FTB_HALT_S_AHB & 0xfff);
    s.haltS4 = rd32(s.gccBranch.va, FTB_HALT_S4 & 0xfff);
    o.add("FT2 vote%08x m%08x s%08x s4%08x %uus", s.voteAfter, s.haltM, s.haltS, s.haltS4, s.waitedUs);
    if (!on) {o.add(" CLOCKS OFF"); fail(2, -110); return false;}   // never touch the engine without running clocks
    if (s.xoLevel < 0) {o.add(" no 19.2MHz DFS level"); fail(21, -22); return false;}
    // Pins: the I2C pair to qup12 (2 mA, no bias, as qupv3_se12_i2c_active); the interrupt an input with pull-up (ts_default_pins);
    // the 1.8 V enable and reset outputs, the level written before the output is enabled.
    pinWrite(s, kSda, ftb_pin_cfg(FTB_FUNC_QUP12, 2, FTB_PULL_NONE, 0));
    pinWrite(s, kScl, ftb_pin_cfg(FTB_FUNC_QUP12, 2, FTB_PULL_NONE, 0));
    pinWrite(s, kInt, ftb_pin_cfg(FTB_FUNC_GPIO, 16, FTB_PULL_UP, 0));
    if (s.opt.power) {
        native_touch_step = 22;
        pinOut(s, kVio, true);
        pinWrite(s, kVio, ftb_pin_cfg(FTB_FUNC_GPIO, 2, FTB_PULL_NONE, 1));
        IOSleep(FTB_VIO_SETTLE_MS);
        // synaptics_driver_s3320.c tpd_hw_pwron: reset high, low, high, 10 ms apart; then the 200 ms startup delay of the DT.
        pinOut(s, kRst, true);
        pinWrite(s, kRst, ftb_pin_cfg(FTB_FUNC_GPIO, 16, FTB_PULL_UP, 1));
        IOSleep(FTB_RESET_STEP_MS);
        pinOut(s, kRst, false);
        IOSleep(FTB_RESET_STEP_MS);
        pinOut(s, kRst, true);
        IOSleep(FTB_STARTUP_MS);
    }
    for (unsigned i = 0; i < kPins; i++) {s.ctlAfter[i] = rd32(s.pin[i].va, 0); s.ioAfter[i] = rd32(s.pin[i].va, 4);}
    o.add(" pins%03x/%03x vio%x rst%x int%x", s.ctlAfter[kSda] & 0x3ff, s.ctlAfter[kScl] & 0x3ff, s.ioAfter[kVio] & 3,
          s.ioAfter[kRst] & 3, s.ioAfter[kInt] & 1);
    native_touch_step = 23;   // the next accesses touch the engine
    if (!mapRegion(FTB_QUP_WRAPPER_BASE, 0x1000, s.wrapper) || !mapRegion(FTB_SE_BASE, FTB_SE_SIZE, s.se)) {fail(23, -12); return false;}
    s.i2c.se = geni_i2c_io{reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.i2c.wrapper = geni_i2c_io{reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    s.i2c.poll_limit = 20000;
    int r = geni_i2c_init(&s.i2c);
    o.add(" fw%04x d%u", s.i2c.info.fw_revision & 0xffff, s.i2c.info.tx_fifo_depth);
    if (r) {o.add(" INIT%d", r); fail(23, r); return false;}
    r = geni_i2c_set_clock(&s.i2c, static_cast<unsigned>(s.xoLevel), FTB_TCXO_HZ, s.opt.hz);
    if (r) {o.add(" CLK%d", r); fail(24, r); return false;}
    o.add(" L%d %ukHz", s.xoLevel, s.opt.hz / 1000);
    // ACK probe: a one-byte write of the PDT pointer (it only moves the controller's register pointer), up to five times while
    // the controller may still be starting.
    native_touch_step = 25;
    const uint8_t pointer = RMI_PDT_START;
    s.ackRc = GI2C_ERR_ADDR_NACK;
    for (s.ackTries = 1; s.ackTries <= 5; s.ackTries++) {
        s.ackRc = geni_i2c_write(&s.i2c, FTB_I2C_ADDR, &pointer, 1, 1);
        if (s.ackRc != GI2C_ERR_ADDR_NACK && s.ackRc != GI2C_ERR_TIMEOUT) break;
        IOSleep(20);
    }
    if (s.ackTries > 5) s.ackTries = 5;
    o.add(" ack%d x%u", s.ackRc, s.ackTries);
    if (s.ackRc) {
        // No ACK: the controller is unpowered (VDD = PM8998 LDO28, an RPMh rail XNU does not drive) or held in reset; a timeout or a
        // bus error with the lines low (ios) points at the 1.8 V pull-ups instead.
        fail(25, s.ackRc);
        errorRow(s, s.ackRc == GI2C_ERR_ADDR_NACK ? "no ACK at 0x20 (LDO28 off?)" : "ACK probe failed");
        return false;
    }
    s.link = ftb_rmi_link{&s.i2c, FTB_I2C_ADDR, 0};
    return true;
}

// ---- stage 3: RMI4 ----
bool rmiSetup(Session &s) {
    native_touch_step = 3;
    struct rmi_dev &r = s.rmi;
    const struct rmi_bus bus = {&s.link, ftb_rmi_read, ftb_rmi_write, sleepMs};
    rmi_init(&r, &bus);
    r.force_fallback = s.opt.pdt == 1;
    int rc = rmi_scan_pdt(&r);
    {
        Out o(fajita_touch_rows[4], sizeof fajita_touch_rows[4]);
        o.add("FT3 pdt%d %s e9:%02x%02x%02x%02x%02x%02x irq%u/%u", rc, r.pdt_mode == RMI_PDT_FALLBACK ? "fixed" : "scan", r.first_entry[0],
              r.first_entry[1], r.first_entry[2], r.first_entry[3], r.first_entry[4], r.first_entry[5], r.irq_count, r.irq_regs);
        for (unsigned i = 0; i < r.nfn; i++)
            o.add(" F%02x@%x q%x c%x d%x i%u", r.fn[i].number, r.fn[i].entry, r.fn[i].query, r.fn[i].control, r.fn[i].data, r.fn[i].irq_pos);
    }
    logRow(4);
    if (rc) {fail(31, rc); errorRow(s, "PDT"); return false;}
    Out o(fajita_touch_rows[5], sizeof fajita_touch_rows[5]);
    rc = rmi_f01_read_info(&r);
    o.add("FT3 f01 st%02x m%02x q1%02x id'%s'", r.status, r.manufacturer, r.query1, r.product_id);
    if (rc) {o.add(rc == RMI_ERR_BOOTLOADER ? " BOOTLOADER" : " ERR%d", rc); fail(32, rc); errorRow(s, "F01 info"); return false;}
    if (s.opt.rmiReset) {
        rc = rmi_f01_reset(&r, FTB_STARTUP_MS);
        if (!rc) rc = rmi_f01_read_info(&r);
        o.add(" reset%d st%02x", rc, r.status);
        if (rc) {fail(32, rc); errorRow(s, "F01 reset"); return false;}
    }
    rc = rmi_f01_configure(&r, 1);
    o.add(" c0 %02x>%02x ien%02x", r.ctrl0_before, r.ctrl0, r.irq_enable);
    if (rc) {o.add(" ERR%d", rc); fail(33, rc); errorRow(s, "F01 configure"); return false;}
    rc = rmi_f12_probe(&r);
    o.add(" | f12 q0%02x %s d1 %u@%u n%u rd%u max%ux%u", r.f12_query0, r.f12_hardcoded ? "hardcoded" : "desc", r.data1_size,
          r.data1_offset, r.objects, r.read_size, r.max_x, r.max_y);
    if (rc) {o.add(" ERR%d", rc); fail(34, rc); errorRow(s, "F12"); return false;}
    // Axis maxima: the boot arguments, else Control 8, else the panel size.
    s.map.max_x = s.opt.maxX ? s.opt.maxX : r.max_x;
    s.map.max_y = s.opt.maxY ? s.opt.maxY : r.max_y;
    s.map.orient = s.opt.orient;
    o.add(" map%ux%u o%u", s.map.max_x ? s.map.max_x : FTB_DEFAULT_MAX_X, s.map.max_y ? s.map.max_y : FTB_DEFAULT_MAX_Y, s.map.orient);
    return true;
}

// ---- stage 4: contacts to NativeHidInput ----
struct Stroke {int id, x0, y0, x1, y1, frames; uint32_t t0;};
Stroke gStrokes[TOUCH_SLOTS];
unsigned gStrokeCount;
uint32_t touchMs() {
    uint64_t f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return static_cast<uint32_t>(mach_absolute_time() / ((f ? f : 19200000) / 1000));
}
void trackStrokes(unsigned count, const int *ids, const int *xs, const int *ys) {
    const uint32_t now = touchMs();
    for (unsigned i = 0; i < count; ++i) {
        if (xs[i] < native_touch_extent[0]) native_touch_extent[0] = xs[i];
        if (xs[i] > native_touch_extent[1]) native_touch_extent[1] = xs[i];
        if (ys[i] < native_touch_extent[2]) native_touch_extent[2] = ys[i];
        if (ys[i] > native_touch_extent[3]) native_touch_extent[3] = ys[i];
        unsigned k = 0;
        while (k < gStrokeCount && gStrokes[k].id != ids[i]) ++k;
        if (k == gStrokeCount) {
            if (gStrokeCount == TOUCH_SLOTS) continue;
            gStrokes[gStrokeCount++] = {ids[i], xs[i], ys[i], xs[i], ys[i], 0, now};
        }
        gStrokes[k].x1 = xs[i]; gStrokes[k].y1 = ys[i]; ++gStrokes[k].frames;
    }
    for (unsigned k = 0; k < gStrokeCount;) {
        bool live = false;
        for (unsigned i = 0; i < count && !live; ++i) live = ids[i] == gStrokes[k].id;
        if (live) {++k; continue;}
        volatile int32_t *r = native_touch_strokes[native_touch_stroke_count & 15];
        r[0] = static_cast<int32_t>(gStrokes[k].t0); r[1] = static_cast<int32_t>(now - gStrokes[k].t0);
        r[2] = gStrokes[k].x0; r[3] = gStrokes[k].y0; r[4] = gStrokes[k].x1; r[5] = gStrokes[k].y1; r[6] = gStrokes[k].frames;
        native_touch_stroke_count = native_touch_stroke_count + 1;
        gStrokes[k] = gStrokes[--gStrokeCount];
    }
}
}  // namespace

// Whoever consumes the finger set (NativeHidInput) registers a function here; it is called on the touch thread after the seqlock
// closes (the same entry point sheng's TouchProbe.cpp exports).
static void (*gTouchSink)(void *);
static void *gTouchSinkContext;
extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
    gTouchSinkContext = context;
    __atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);
}

namespace {
// The display mapping now: the identity until NativeLegacyDisplay has published its logical mode (or with fajita-touch-view=0).
void readView(const Session &s, struct ftb_view &v) {
    v = ftb_view{};
    if (!s.opt.view || !__atomic_load_n(&fajita_display_logical.valid, __ATOMIC_ACQUIRE)) return;
    v.logical_w = fajita_display_logical.logical_width; v.logical_h = fajita_display_logical.logical_height;
    v.image_x = fajita_display_logical.image_x; v.image_y = fajita_display_logical.image_y;
    v.image_w = fajita_display_logical.image_width; v.image_h = fajita_display_logical.image_height;
    v.valid = 1;
    if (!ftb_view_usable(&v)) v.valid = 0;   // a rectangle off the panel: keep the identity rather than map into nowhere
}

void publish(Session &s, unsigned count, const struct rmi_contact *c) {
    int ids[TOUCH_SLOTS], xs[TOUCH_SLOTS], ys[TOUCH_SLOTS];
    if (count > TOUCH_SLOTS) count = TOUCH_SLOTS;
    readView(s, s.view);
    uint32_t live = 0;
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t bit = c[i].id < 32 ? 1u << c[i].id : 0;
        live |= bit;
        int x, y;
        const int inside = ftb_map_point(&s.map, &s.view, c[i].x, c[i].y, &x, &y);
        if (!(s.liveIds & bit)) {   // a contact begins: where it lands decides whether a letterbox start is dropped
            native_touch_downs = native_touch_downs + 1;
            if (!inside) {
                fajita_touch_letterbox[0] = fajita_touch_letterbox[0] + 1;
                if (s.opt.letterbox == 1) s.droppedIds |= bit;
            }
        }
        if (s.droppedIds & bit) {fajita_touch_letterbox[1] = fajita_touch_letterbox[1] + 1; continue;}
        ids[kept] = c[i].id; xs[kept] = x; ys[kept] = y; ++kept;
        if (c[i].x < fajita_touch_raw[2]) fajita_touch_raw[2] = c[i].x;
        if (c[i].x > fajita_touch_raw[3]) fajita_touch_raw[3] = c[i].x;
        if (c[i].y < fajita_touch_raw[4]) fajita_touch_raw[4] = c[i].y;
        if (c[i].y > fajita_touch_raw[5]) fajita_touch_raw[5] = c[i].y;
    }
    s.liveIds = live;
    s.droppedIds &= live;   // a dropped contact's slot starts fresh once it lifts
    if (count) {fajita_touch_raw[0] = c[0].x; fajita_touch_raw[1] = c[0].y; fajita_touch_contact_frames = fajita_touch_contact_frames + 1;}
    if (kept) {native_touch_last_x = xs[0]; native_touch_last_y = ys[0];}
    count = kept;
    trackStrokes(count, ids, xs, ys);
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count; ++i) {native_touch_xs[i] = xs[i]; native_touch_ys[i] = ys[i]; native_touch_ids[i] = ids[i];}
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    native_touch_contacts = count;
    if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) sink(gTouchSinkContext);
}

void loopRow(const Session &s) {
    Out o(fajita_touch_rows[6], sizeof fajita_touch_rows[6]);
    o.add("FT4 polls%u irq%u fr%u cf%u c%u dn%u lb%u/%u raw%d,%d ext x%d..%d y%d..%d st%02x err%u back%u recfg%u gpio%u/%u",
          s.polls, s.irqHits, s.frames, fajita_touch_contact_frames, native_touch_contacts, native_touch_downs,
          fajita_touch_letterbox[0], fajita_touch_letterbox[1], fajita_touch_raw[0], fajita_touch_raw[1], fajita_touch_raw[2],
          fajita_touch_raw[3], fajita_touch_raw[4], fajita_touch_raw[5], s.lastStatus, s.busErrors, s.backoffs, s.reconfigs,
          native_touch_irq_stats[1], native_touch_irq_stats[2]);
    // The HID side: NativeHidInput started, its event-driver checks, the touch reports the driver took (and the last IOReturn),
    // the reports counted here, the edge wrapper (installed, contacts begun, of them at an edge), and the event service:
    // checkpoints that found it, open (isOpen), DeviceOpenedByEventSystem, user clients, TouchDetectionMode (- = never set),
    // the published Built-In/PrimaryUsage, and the display mapping in use (L logical W x H, P the panel).
    Out h(fajita_touch_rows[8], sizeof fajita_touch_rows[8]);
    const bool ev = &native_hid_input_events && &native_hid_input_results;
    h.add("FT5 hid up%u chk%u rep%u res%x n%u", ev ? native_hid_input_events[2] : 0, ev ? native_hid_input_events[3] : 0,
          ev ? native_hid_input_events[6] : 0, ev ? static_cast<unsigned>(native_hid_input_results[6]) : 0u, native_touch_hid_reports);
    if (&native_touch_edge_stats)
        h.add(" edge%u/%u/%u", native_touch_edge_stats[0], native_touch_edge_stats[1], native_touch_edge_stats[2]);
    if (&native_hid_touch_service) {
        const volatile uint32_t *v = native_hid_touch_service;
        h.add(" svc%u open%u eso%u uc%u", v[0], v[1], v[2], v[3]);
        if (v[4] >> 31) h.add(" tdm%x", v[4] & 0x7fffffffu); else h.add(" tdm-");
        h.add(" pub%u", v[5]);
    }
    h.add(" map%c%ux%u", s.view.valid ? 'L' : 'P', ftb_view_w(&s.view), ftb_view_h(&s.view));
}
}  // namespace

// The controller's interrupt (GPIO 125, falling edge) through the TLMM summary interrupt, SPI 208 = INTID 240, as sheng's
// TouchProbe does with its GPIO 25. Opt-in (fajita-touch-irq=1); the loop keeps a timeout so a lost edge only costs latency.
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
static volatile uint32_t gTouchIrqEvents;
static volatile uint32_t *gTouchPin;
static void touchIrq(void *) {
    if (!(gTouchPin[0xc / 4] & 1)) {native_touch_irq_stats[2] = native_touch_irq_stats[2] + 1; return;}
    gTouchPin[0xc / 4] = 0;
    native_touch_irq_stats[1] = native_touch_irq_stats[1] + 1;
    __atomic_fetch_add(&gTouchIrqEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gTouchIrqEvents);
}
static bool armTouchIrq(Session &s) {
    gTouchPin = reinterpret_cast<volatile uint32_t *>(s.pin[kInt].va);
    const uint32_t cfg = ftb_intr_cfg_falling(gTouchPin[0x8 / 4]);
    gTouchPin[0x8 / 4] = cfg;
    gTouchPin[0xc / 4] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!armcpu_spi_attach) {native_touch_irq_stats[0] = 2; native_touch_irq_stats[3] = 0xffff; return false;}
    const int rc = armcpu_spi_attach(FTB_TLMM_SUMMARY_INTID, false, touchIrq, nullptr, nullptr, nullptr);
    native_touch_irq_stats[3] = static_cast<uint32_t>(rc);
    if (rc) {native_touch_irq_stats[0] = 2; return false;}
    gTouchPin[0x8 / 4] = cfg | FTB_INTR_ENABLE;   // enabled last
    __asm__ volatile("dsb sy" ::: "memory");
    native_touch_irq_stats[0] = 1;
    return true;
}

static void pollLoop(Session &s) {
    native_touch_step = 4;
    struct rmi_dev &r = s.rmi;
    const unsigned period = 1000 / s.opt.rate;
    const bool irqArmed = s.opt.irq && armTouchIrq(s);
    uint32_t seen = gTouchIrqEvents;
    unsigned failures = 0, rowTick = 0;
    for (;;) {
        native_touch_loop_step = 1;
        ++s.polls;
        uint8_t status = 0;
        uint32_t irq = 0;
        int rc = rmi_read_irq(&r, &status, &irq);
        if (!rc) {
            s.lastStatus = status;
            s.lastIrq = irq;
            if (status & RMI_F01_STATUS_UNCONFIGURED) {
                // The controller reset itself (or lost power): configure it again and drop the contacts it no longer reports.
                ++s.reconfigs;
                rc = rmi_f01_configure(&r, 1);
                publish(s, 0, nullptr);
            }
        }
        if (!rc && ((irq & r.abs_mask) || s.opt.pollAll)) {
            native_touch_loop_step = 2;
            if (irq & r.abs_mask) ++s.irqHits;
            struct rmi_contact c[RMI_MAX_OBJECTS];
            const int n = rmi_f12_read_contacts(&r, c, RMI_MAX_OBJECTS);
            if (n >= 0) {
                ++s.frames;
                native_touch_frames = native_touch_frames + 1;
                native_touch_valid = native_touch_valid + 1;
                publish(s, static_cast<unsigned>(n), c);
            } else rc = n;
        }
        if (rc) {
            ++s.busErrors;
            s.lastRc = rc;
            if (!(s.busErrors & 63)) errorRow(s, "poll");
            if (++failures >= 20) {   // a controller that went away: one attempt a second instead of a hundred
                ++s.backoffs;
                failures = 0;
                errorRow(s, "poll backoff");
                IOSleep(1000);
            }
        } else failures = 0;
        if ((++rowTick % s.opt.rate) == 0) {loopRow(s); summary(s); if (!(rowTick % (s.opt.rate * 10))) logRow(8);}
        native_touch_loop_step = 3;
        if (irqArmed) {
            assert_wait_timeout((event_t)&gTouchIrqEvents, THREAD_UNINT, 100, NSEC_PER_MSEC);
            if (gTouchIrqEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
            else thread_block(THREAD_CONTINUE_NULL);
            seen = gTouchIrqEvents;
        } else IOSleep(period);
    }
}

static void readOptions(Options &o) {
    o.hz = FTB_I2C_HZ; o.power = 1; o.pdt = 0; o.rmiReset = 0; o.maxX = o.maxY = 0; o.orient = 0; o.rate = 100; o.irq = 0; o.pollAll = 0;
    o.letterbox = 0; o.view = 1;
    // fajita-touch-letterbox=1 drops a contact that begins outside the displayed image (default 0: clamp it to the image edge, so a
    // swipe up from the black bar below the image is a swipe from the bottom edge); fajita-touch-view=0 ignores the display's
    // logical mode and maps onto the whole panel.
    PE_parse_boot_argn("fajita-touch-letterbox", &o.letterbox, sizeof o.letterbox);
    PE_parse_boot_argn("fajita-touch-view", &o.view, sizeof o.view);
    PE_parse_boot_argn("fajita-touch-hz", &o.hz, sizeof o.hz);
    if (o.hz != 100000 && o.hz != 400000 && o.hz != 1000000) o.hz = FTB_I2C_HZ;
    PE_parse_boot_argn("fajita-touch-power", &o.power, sizeof o.power);
    PE_parse_boot_argn("fajita-touch-pdt", &o.pdt, sizeof o.pdt);
    PE_parse_boot_argn("fajita-touch-rmi-reset", &o.rmiReset, sizeof o.rmiReset);
    PE_parse_boot_argn("fajita-touch-max-x", &o.maxX, sizeof o.maxX);
    PE_parse_boot_argn("fajita-touch-max-y", &o.maxY, sizeof o.maxY);
    if (o.maxX > 65535) o.maxX = 0;
    if (o.maxY > 65535) o.maxY = 0;
    PE_parse_boot_argn("fajita-touch-orient", &o.orient, sizeof o.orient);
    o.orient &= 7;
    PE_parse_boot_argn("fajita-touch-rate", &o.rate, sizeof o.rate);
    if (o.rate < 10 || o.rate > 250) o.rate = 100;
    PE_parse_boot_argn("fajita-touch-irq", &o.irq, sizeof o.irq);
    PE_parse_boot_argn("fajita-touch-poll-all", &o.pollAll, sizeof o.pollAll);
}

static void touchThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-touch");
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    s->ackRc = 1;   // not probed
    s->opt.stage = native_touch_stage_req;
    readOptions(s->opt);
    do {
        bool ok = snapshot(*s);
        logRow(0); logRow(1); logRow(2);
        if (!ok) break;
        native_touch_stage_done = 1;
        summary(*s);
        if (s->opt.stage < 2) break;
        ok = enable(*s);
        logRow(3);
        if (!ok) break;
        native_touch_stage_done = 2;
        summary(*s);
        if (s->opt.stage < 3) break;
        ok = rmiSetup(*s);
        logRow(5);
        if (!ok) break;
        native_touch_stage_done = 3;
        summary(*s);
        if (s->opt.stage < 4) break;
        native_touch_stage_done = 4;
        summary(*s);
        pollLoop(*s);   // does not return
    } while (0);
    summary(*s);
    logRow(7);
    IOLog("fajita-touch: %s\n", native_touch_rows[0]);
    // The session stays allocated: its mappings keep the registers the rows describe readable for a later look.
}

// Entry point, called by the board's late probe (the display kit's LegacyDisplayProbe.cpp, as sheng calls its TouchProbe) and by
// the boot progress monitor once a second; whichever comes first after IOKit's platform expert exists starts the thread.
extern "C" void native_touch_probe(void) {
    static bool absent;
    if (absent || native_touch_stage_req) return;
    if (!IOService::getPlatform()) return;   // too early: IOKit is not up yet (try again on the next call)
    unsigned stage = 0;
    if (!PE_parse_boot_argn("fajita-touch", &stage, sizeof stage) || stage < 1) {absent = true; return;}
    if (stage > 4) stage = 4;
    if (!__sync_bool_compare_and_swap(&native_touch_stage_req, 0u, stage)) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_touch_error = -12;
}
