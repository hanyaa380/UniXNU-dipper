// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

// Exact private entry from Darwin 23 arm/rtclock.h. Its full data header is
// not exported to IOKit; the IRQ path calls the real kernel implementation.
extern "C" void rtclock_intr (unsigned int);
// Kernel-internal entries used for secondary CPU startup (Darwin 23 osfmk).
extern "C" vm_offset_t ml_static_vtop (vm_offset_t);
extern "C" char LowResetVectorBase[], LowResetVectorEnd[], ResetHandlerData[], const_boot_args[];
extern "C" kern_return_t processor_start (processor_t);
extern "C" kern_return_t kernel_thread_start (void (*) (void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate (thread_t);
extern "C" thread_t current_thread (void);
extern "C" kern_return_t thread_terminate (thread_t);

extern "C" {
volatile uint32_t armcpu_registered = 0;
volatile uint32_t armcpu_gic_ready = 0;
volatile uint64_t armcpu_timer_interrupts = 0;
volatile uint64_t armcpu_ipi_interrupts = 0;
volatile uint64_t armcpu_ipi_sent = 0;       // SGIs the kernel asked ArmCpu::signalCPU to send
volatile uint64_t armcpu_ipi_selftest_sent = 0;
// Boot core's own GIC registers read back once after enabling (own frame, the same one just written): 0 IGROUPR0, 1 ISENABLER0, 2 GICD_CTLR,
// 3 GICD_TYPER, 4 IGRPMODR0, 5 ICC_PMR_EL1, 6 ICC_IGRPEN1_EL1, 7 ICC_SRE_EL1.
volatile uint32_t armcpu_gic_boot[8];
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];   // per logical core: architectural timer interrupts and SGI 0 taken
static bool gSpiIpi = false;                // boot-arg armcpu-spi-ipi=1: IPIs are software-pended SPIs 1008+core routed to that core (ICC_SGI1R_EL1 generates nothing on this board)
static bool gSelfIpi = false;               // boot-arg armcpu-self-ipi=1: every core sends itself SGI 0 from initCPU
volatile uint32_t armcpu_irq_phase = 0,armcpu_irq_id = 1023;
volatile uint32_t armcpu_quiesce_calls = 0,armcpu_halt_calls = 0;
// Secondary-CPU bring-up evidence (sheng-smp=1). Step values: 1 nub started, 2 registered, 3 start thread running,
// 4 PSCI CPU_ON issued, 5 entered initCPU on the new core, 6 GIC redistributor ready, 7 running.
volatile uint32_t armcpu_smp_requested = 0, armcpu_smp_running = 0, armcpu_smp_conduit = 0;
volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8], armcpu_smp_seen_mpidr[8], armcpu_smp_fail[8], armcpu_boot_mpidr = 0;
// System registers seen on each secondary core at initCPU entry: CurrentEL, SCTLR_EL1, TCR_EL1, MAIR_EL1 (low words), MIDR_EL1.
volatile uint32_t armcpu_smp_sys[8][5], armcpu_boot_sys[5];
volatile int32_t armcpu_smp_psci_rc[8];
}

#define ARMCPU_SMP_SLOTS 8

class ArmCpu : public IOCPU {
  OSDeclareDefaultStructors (ArmCpu)
  OSSharedPtr<IOMemoryMap> distributor;
  OSSharedPtr<IOMemoryMap> redistributors;
  IOCPUInterruptController *controller = nullptr;
  const OSSymbol *cpuName = nullptr;
  uintptr_t dist = 0;
  uintptr_t redist = 0;
  uint64_t affinity = 0;
  uintptr_t redistBase = 0;
  uint64_t redistSize = 0;
  bool skipGic = false;
  bool diagnosePower = false;
  bool secondary = false;
  uint32_t logicalId = 0;
  bool mapGic (void);
  bool initDistributor (void);
  bool wakeRedistributor (bool local);
  bool finishSecondary (void);
  static void drainPending (void);
  bool prepareGic (void);
  void startSecondary (void);
  static void startThread (void *argument, wait_result_t);
  void enableGic (void);
  void interrupt (void);
  static void dispatch (void *target, void *, void *, int);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void initCPU (bool boot) override;
  void quiesceCPU (void) override;
  void haltCPU (void) override;
  kern_return_t startCPU (vm_offset_t, vm_offset_t) override;
  void signalCPU (IOCPU *target) override;
  uint64_t signalTarget (void) const { return affinity; }
  void enableCPUTimeBase (bool enabled) override;
  const OSSymbol *getCPUName (void) override;
};

OSDefineMetaClassAndStructors (ArmCpu, IOCPU)

// The EL1 virtual timer's INTID: PPI 27 on most SoCs; Qualcomm SDM845 wires it to PPI 3 (INTID 19), so a board passes
// armcpu-timer-intid=19.
static uint32_t gTimerIntid = 27;
// fajita-deadline=<seconds>: an unattended run must come back even when threads stop (memory exhaustion, a stuck kernel thread). The
// timer interrupt keeps arriving then, so its handler issues PSCI SYSTEM_RESET <seconds> after ArmCpu starts; with empty boot
// slots the OnePlus ABL lands in fastboot. 0 (default) = off.
extern "C" { volatile uint64_t armcpu_deadline_start = 0, armcpu_deadline_ticks = 0, armcpu_deadline_seen = 0; }
// fajita-wdog=<mode>: the SDM845 APSS watchdog (qcom,apss-wdt-sdm845, 0x17980000, kpss layout: RST 0x4, EN 0x8, STS 0xC, BARK_TIME
// 0x10, BITE_TIME 0x14, 32768 Hz sleep clock). Every fajita run froze ~2 minutes after kernel start with even the timer interrupt
// gone, which a watchdog left running by the bootloader would explain (Linux's qcom_wdt pets a watchdog it finds running).
// 1 = read the registers once (armcpu_wdog[], shown by BootReset's probe text), 2 = also pet it from the timer interrupt, which
// needs no thread, 3 = arm it as the unattended-run backstop: the bootloader leaves it disabled (EN=0 on hardware), so a freeze
// that stops even the timer interrupt (fajita froze ~2 minutes in, every run) would otherwise need a key press. Mode 3 writes the
// bootloader restart reason (IMEM 0x146bf65c = 0x77665500, the cookie the Qualcomm ABL reads), then enables the watchdog with
// bark 16 s and bite 32 s (the register maxima, 32768 Hz) and pets it from every timer interrupt. A freeze bites within 32 s and the
// ABL lands in fastboot. 0 (default) = untouched.
extern "C" { volatile uint32_t armcpu_wdog[6] = { 0, 0, 0, 0, 0, 0 }; volatile uint64_t armcpu_wdog_pets = 0;
             volatile uint32_t armcpu_imem[4] = { 0, 0, 0, 0 }; }   // IMEM 0x146bf000: word 0, dload_type 0x1c, restart_reason 0x65c, after write
// Qualcomm SCM over SMC (SMCCC SIP, the calls Linux qcom_scm makes from EL1 on this firmware). Mode 3 switches off the two things
// that turn a watchdog bite into "Qualcomm CrashDump Mode" before it arms the watchdog: the SDI debug image (0xC2000109) and the
// download-mode bit 0x10 of TCSR_BOOT_MISC_DETECT (0x01FD3000), which the stock firmware sets every boot and the OS must clear
// (0xC2000110, fallback secure write 0xC2000502); TCSR is read back with 0xC2000501. Sources: fajita-soc-reset-freeze-research.md.
// armcpu_scm[]: avail(SDI) avail(dload) avail(io write) sdi-rc tcsr-before dload-rc write-rc tcsr-after
extern "C" { volatile int64_t armcpu_scm[8] = { -99, -99, -99, -99, -99, -99, -99, -99 }; }
struct ScmResult { uint64_t a0, a1; };
static ScmResult scm (uint64_t fn, uint64_t info, uint64_t a2, uint64_t a3)
{
  register uint64_t x0 __asm__ ("x0") = fn;
  register uint64_t x1 __asm__ ("x1") = info;
  register uint64_t x2 __asm__ ("x2") = a2;
  register uint64_t x3 __asm__ ("x3") = a3;
  register uint64_t x4 __asm__ ("x4") = 0;
  register uint64_t x5 __asm__ ("x5") = 0;
  __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5)
                    :: "memory", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
  return { x0, x1 };
}
static int64_t scmAvailable (uint32_t fn)
{
  ScmResult r = scm (0xC2000601, 0x1, fn, 0);   // QCOM_SCM_INFO_IS_CALL_AVAIL: a1 = 1 when available
  return r.a0 ? -static_cast<int64_t> (r.a0 & 0xff) - 100 : static_cast<int64_t> (r.a1);
}
static void scmDisableCrashdump (void)
{
  armcpu_scm[0] = scmAvailable (0xC2000109);
  armcpu_scm[1] = scmAvailable (0xC2000110);
  armcpu_scm[2] = scmAvailable (0xC2000502);
  armcpu_scm[3] = static_cast<int64_t> (scm (0xC2000109, 0x2, 1, 0).a0);                    // SDI off after a watchdog reset
  ScmResult before = scm (0xC2000501, 0x1, 0x01FD3000, 0);
  armcpu_scm[4] = before.a0 ? -1 : static_cast<int64_t> (before.a1);
  armcpu_scm[5] = static_cast<int64_t> (scm (0xC2000110, 0x2, 0, 0).a0);                    // download mode off
  ScmResult again = scm (0xC2000501, 0x1, 0x01FD3000, 0);
  if (!again.a0 && (again.a1 & 0x10)) {
    armcpu_scm[6] = static_cast<int64_t> (scm (0xC2000502, 0x2, 0x01FD3000, again.a1 & ~0x10ULL).a0);   // secure write fallback
  }
  ScmResult after = scm (0xC2000501, 0x1, 0x01FD3000, 0);
  armcpu_scm[7] = after.a0 ? -1 : static_cast<int64_t> (after.a1);
}
static OSSharedPtr<IOMemoryMap> gWdogMap;
static uintptr_t gWdog = 0;
static unsigned gWdogMode = 0;
// Redistributor ISENABLER0: SGIs 0..15 (only SGI 0 is used by the kernel; the others let the self-SGI probe tell which INTIDs the GIC accepts) and the timer PPI.
#define kSgiPpiEnable (0xffffU | (1U << gTimerIntid))

static const uint32_t kIpiSpiBase = 1008;   // INTIDs 1008..1015 stand for 'IPI to logical core 0..7'

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

// Also petted from the boot-progress monitor (BootLog's once-a-second tick): XNU's timer is tickless, so an idle system can go
// longer than the 32 s bite without a timer interrupt (an idle kernel-only run bit falsely). A real freeze stops both.
// Only the boot CPU pets: with SMP a healthy secondary kept the watchdog alive while CPU0 was frozen (fajita, 2-core run, 2026-10-22).
// fajita-wdog-stop=<seconds>: a positive control for the bark capture. Petting stops that long after the first pet, so a healthy
// system barks 16 s later and must show "FAJITA BARK" in the log (then the panic screen resets); a bite instead means the bark
// does not reach the CPU. 0 (default) = pet forever.
// armcpu_wdog_live (BootReset's probe text, refreshed every log slot): cntfrq, stop, first and last pet (mach_absolute_time), STS,
// raw CNTVCT/CNTPCT, then IPIs sent and taken and timer interrupts (and the CPU0 debug probe). The stop runs on mach_absolute_time: an inline CNTVCT read in a kext came back as a constant (0 or 3) on
// fajita (runH-barkctl2), so a CNTVCT-based stop never fired.
extern "C" { volatile uint64_t armcpu_wdog_live[14] = { 0 }; }   // [10..13]: CPU0 debug CIDR1, EDDEVARCH, EDPRSR, one EDPCSR sample
// fajita-wdog-stop-panic=1 checks the capture path alone: at the stop time the timer interrupt calls barkCapture() itself, so
// "FAJITA BARK" in the log and a panic-screen reset prove the panic flush works from interrupt context without the watchdog.
static uint64_t gWdogStopTicks = 0;
static unsigned gWdogStopPanic = 0;
static void barkCapture (void) __attribute__ ((noreturn));
static void wdogPet (bool fromInterrupt)
{
  const uint64_t now = mach_absolute_time ();
  if (!armcpu_wdog_live[2]) armcpu_wdog_live[2] = now;
  if (gWdogStopTicks && now - armcpu_wdog_live[2] >= gWdogStopTicks) {
    if (gWdogStopPanic && fromInterrupt) barkCapture ();
    return;
  }
  write32 (gWdog, 0x4, 1);   // WDT_RST: restart the count
  armcpu_wdog_live[3] = now;
  armcpu_wdog_pets = armcpu_wdog_pets + 1;
}

// A cache-inhibited kernel mapping of a physical range that stays for the boot (BootDiagExport's screenshot of the boot framebuffer);
// 0 on failure. IOKit maps through the kernel map, not the small ml_io_map submap.
extern "C" uint64_t armcpu_map_physical (uint64_t physical, uint64_t length)
{
  OSSharedPtr<IOMemoryDescriptor> d (IOMemoryDescriptor::withPhysicalAddress (physical, length, kIODirectionIn), OSNoRetain);
  IOMemoryMap *map = d ? d->map (kIOMapAnywhere | kIOMapInhibitCache) : nullptr;   // kept: never released
  return map ? static_cast<uint64_t> (map->getVirtualAddress ()) : 0;
}

extern "C" void armcpu_wdog_sample (void)
{
  uint64_t vct, pct;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (vct));
  __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (pct));
  armcpu_wdog_live[5] = vct;
  armcpu_wdog_live[6] = pct;
  armcpu_wdog_live[7] = armcpu_ipi_sent;
  armcpu_wdog_live[8] = armcpu_ipi_interrupts;
  armcpu_wdog_live[9] = armcpu_timer_interrupts;
  if (gWdog) armcpu_wdog_live[4] = read32 (gWdog, 0xc);
}

extern "C" int cpu_number (void);
extern "C" void armcpu_wdog_pet (void)
{
  if (gWdog && gWdogMode >= 2 && cpu_number () == 0) wdogPet (false);
}

// Distributor side of the software IPI of logical core `core`: group 1, edge-triggered, priority 0x80, routed to affinity `aff`, enabled.
static void spiIpiConfigure (uintptr_t dist, uint32_t core, uint64_t aff)
{
  const uint32_t id = kIpiSpiBase + core;
  write32 (dist, 0x80 + (id / 32) * 4, read32 (dist, 0x80 + (id / 32) * 4) | (1U << (id % 32)));
  write32 (dist, 0xc00 + (id / 16) * 4, read32 (dist, 0xc00 + (id / 16) * 4) | (2U << ((id % 16) * 2)));
  write32 (dist, 0x400 + (id / 4) * 4, 0x80808080);
  *reinterpret_cast<volatile uint64_t *>(dist + 0x6000 + 8ULL * id) = aff & 0xff00ffffffULL;
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (dist, 0x100 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void spiIpiSend (uintptr_t dist, uint32_t core)
{
  const uint32_t id = kIpiSpiBase + core;
  __asm__ volatile ("dsb ishst" ::: "memory");
  write32 (dist, 0x200 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t counter (void)
{
  uint64_t value;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (value));
  return value;
}

static bool waitClear (uintptr_t base, unsigned offset, uint32_t mask)
{
  uint64_t frequency;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
  uint64_t started = counter ();
  while ((read32 (base, offset) & mask) != 0) {
    if (counter () - started > frequency / 10) {
      return false;
    }
  }
  return true;
}

// GIC state shared by all CPU nubs. The boot CPU maps the distributor and redistributor frames; a secondary core
// only needs the virtual addresses and its own redistributor frame.
static OSSharedPtr<IOMemoryMap> gDistMap, gRedistMap;
static uintptr_t gDist = 0, gFrames = 0;
static uint64_t gFramesSize = 0;

// fajita-wdog=3: the watchdog's bark (SPI 0, INTID 32, BARK_TIME = 16 s after the last pet, before the 32 s bite) as a normal
// non-secure IRQ to the boot CPU. If it is taken, the CPU was alive with interrupts on while nothing petted (the timer interrupt had
// stopped); the handler records where it interrupted (ELR_EL1/SPSR_EL1), the virtual timer and the GIC state of the timer INTID, and
// panics so the panic screen flushes the text to the UFS log and resets. If the board bites instead, interrupts were masked or a
// higher exception level held the CPU (fajita-soc-reset-freeze-research.md).
static const uint32_t kBarkIntid = 32;
static void spiConfigure (uintptr_t dist, uint32_t id, uint64_t aff)
{
  write32 (dist, 0x80 + (id / 32) * 4, read32 (dist, 0x80 + (id / 32) * 4) | (1U << (id % 32)));        // group 1 (non-secure)
  write32 (dist, 0xc00 + (id / 16) * 4, read32 (dist, 0xc00 + (id / 16) * 4) & ~(3U << ((id % 16) * 2))); // level (Linux DT: level high)
  *reinterpret_cast<volatile uint8_t *>(dist + 0x400 + id) = 0x80;
  *reinterpret_cast<volatile uint64_t *>(dist + 0x6000 + 8ULL * id) = aff & 0xff00ffffffULL;
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (dist, 0x100 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

// fajita-wdog-bark-cpu=<n>: the logical CPU the bark is routed to (default 0). With SMP, 1 makes a live secondary the observer of a
// frozen boot CPU (in a 2-core run CPU1 stayed alive while CPU0 froze). fajita-cpu0-debug=1 maps CPU0's external debug block (CoreSight
// per-core stride 0x100000 from 0x7000000: ETM at +0x40000 is sdm845.dtsi's etm@7040000, debug assumed at +0x10000) so the capture can
// read CPU0's PC sample register EDPCSR (0x0A0, then EDPCSRhi 0x0AC) and EDPRSR (0x314), which work while CPU0 cannot take interrupts.
static unsigned gBarkCpu = 0;
static OSSharedPtr<IOMemoryMap> gCpu0DebugMap;
static uintptr_t gCpu0Debug = 0;
static uint64_t cpu0PcSample (void)
{
  if (!gCpu0Debug) return 0;
  const uint32_t lo = read32 (gCpu0Debug, 0xa0);
  const uint32_t hi = read32 (gCpu0Debug, 0xac);
  return (static_cast<uint64_t> (hi) << 32) | lo;
}

extern "C" size_t boot_progress_cpu_tail (int cpu, char *out, size_t cap) __attribute__ ((weak_import));
static char gBarkTail[400];
static void barkCapture (void)
{
  // The bark is level-triggered and stays asserted until the next pet: mask it at the distributor first (ICENABLER), so the panic
  // path is not re-entered. The bite still follows 16 s later, after the panic screen has flushed this text.
  if (gDist) { write32 (gDist, 0x180 + (kBarkIntid / 32) * 4, 1U << (kBarkIntid % 32)); __asm__ volatile ("dsb sy" ::: "memory"); }
  // No MRS of SP_EL0: XNU runs with SPSel=0, where that read is an undefined instruction (runK-panicctl). The current SP stands in.
  uint64_t elr, spsr, vct, cval, ctl, daif, far, esr, sp0;
  __asm__ volatile ("mrs %0, elr_el1" : "=r" (elr));
  __asm__ volatile ("mrs %0, spsr_el1" : "=r" (spsr));
  __asm__ volatile ("mrs %0, far_el1" : "=r" (far));
  __asm__ volatile ("mrs %0, esr_el1" : "=r" (esr));
  __asm__ volatile ("mov %0, sp" : "=r" (sp0));
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (vct));
  __asm__ volatile ("mrs %0, cntv_cval_el0" : "=r" (cval));
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (ctl));
  __asm__ volatile ("mrs %0, daif" : "=r" (daif));
  uint32_t timerPend = 0, timerActive = 0, timerEnable = 0;
  if (gFrames) {
    // The boot CPU's SGI/PPI frame: ISPENDR0 0x200, ISACTIVER0 0x300, ISENABLER0 0x100.
    timerEnable = read32 (gFrames + 0x10000, 0x100); timerPend = read32 (gFrames + 0x10000, 0x200); timerActive = read32 (gFrames + 0x10000, 0x300);
  }
  uint64_t pc0[4] = { 0, 0, 0, 0 };
  uint32_t prsr0 = 0;
  if (gCpu0Debug) {
    prsr0 = read32 (gCpu0Debug, 0x314);
    for (unsigned i = 0; i < 4; i++) pc0[i] = cpu0PcSample ();
  }
  gBarkTail[0] = 0;
  if (&boot_progress_cpu_tail) boot_progress_cpu_tail (0, gBarkTail, sizeof (gBarkTail));
  panic ("FAJITA BARK cpu%d: cpu0 last irqs [%s] mach=%llx | cpu0 edpcsr=%llx %llx %llx %llx edprsr=%x | elr=%llx spsr=%llx far=%llx esr=%llx sp=%llx | vct=%llx "
         "cval=%llx (cval-vct=%lld) ctl=%llx daif=%llx | cpu0 ppi19 en=%u pend=%u act=%u (raw %x/%x/%x) | timer irqs=%llu ipis=%llu "
         "pets=%llu core0 timer=%llu",
         cpu_number (), gBarkTail, (unsigned long long)mach_absolute_time (), pc0[0], pc0[1], pc0[2], pc0[3], prsr0, elr, spsr, far, esr, sp0, vct, cval, (long long)(cval - vct), ctl, daif,
         (timerEnable >> gTimerIntid) & 1, (timerPend >> gTimerIntid) & 1, (timerActive >> gTimerIntid) & 1, timerEnable, timerPend,
         timerActive, (unsigned long long)armcpu_timer_interrupts, (unsigned long long)armcpu_ipi_interrupts,
         (unsigned long long)armcpu_wdog_pets, (unsigned long long)armcpu_core_timer[0]);
}

static IOCPUInterruptController *gController = nullptr;
static volatile uint32_t gGicShared = 0;
static uint64_t gRedistStride = 0; // DT redistributor-stride; 0 = infer from GICR_TYPER.VLPIS
volatile uint32_t armcpu_gic_stride = 0, armcpu_gic_typer_probe[2];
static bool gSmpWanted = false;
// Secondary nubs never wait for the boot CPU (IOSleep needs the timer that only the boot nub enables): they queue here and
// the boot nub, or a late secondary once the boot CPU is registered, finishes them.
static class ArmCpu *volatile gPending[ARMCPU_SMP_SLOTS];
static volatile uint32_t gBootRegistered = 0;
static bool gRemoteGic = false; // sheng-smp-gic=1: the boot CPU prepares a new core's redistributor before CPU_ON

// Shared peripheral interrupts (GICv3 SPIs, INTID 32..1019). A driver attaches a handler for one INTID; the line is set to
// Group 1 non-secure, priority 0xa0, the given trigger and routed to the boot CPU (GICD_IROUTER, affinity routing is on),
// then enabled. The handler runs in the primary interrupt context, before the EOI, and must clear a level source.
// An SPI without a handler is counted and disabled instead of being acknowledged forever.
#define ARMCPU_SPI_SLOTS 16
struct ArmCpuSpi {uint32_t id; void (*handler) (void *); void *context; volatile uint64_t count;};
static ArmCpuSpi gSpi[ARMCPU_SPI_SLOTS];
static volatile uint32_t gSpiUsed = 0;
static uint64_t gBootAffinity = 0;
extern "C" {
volatile uint64_t armcpu_spi_unhandled = 0;
volatile uint32_t armcpu_spi_unhandled_id = 0;
}

IOService *ArmCpu::probe (IOService *provider, SInt32 *score)
{
  OSData *type = OSDynamicCast (OSData, provider->getProperty ("device_type"));
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  uint32_t id;
  if ((type == nullptr) || (type->getLength () != 4) ||
      (memcmp (type->getBytesNoCopy (), "cpu", 4) != 0) ||
      (reg == nullptr) || (reg->getLength () < sizeof (id))) {
    return nullptr;
  }
  memcpy (&id, reg->getBytesNoCopy (), sizeof (id));
  if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) {
    unsigned smp = 0;
    PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
    if (smp == 0 || ml_get_cpu_number (id) < 0 || ml_get_cpu_number (id) >= ARMCPU_SMP_SLOTS) {
      return nullptr; // Secondary startup is opt-in (sheng-smp=1).
    }
  }
  return IOCPU::probe (provider, score);
}

bool ArmCpu::mapGic (void)
{
  IORegistryEntry *soc = IORegistryEntry::fromPath ("/arm-io", gIODTPlane);
  IORegistryEntry *gic = IORegistryEntry::fromPath ("/arm-io/intc", gIODTPlane);
  if ((soc == nullptr) || (gic == nullptr)) {
    if (soc != nullptr) { soc->release (); }
    if (gic != nullptr) { gic->release (); }
    return false;
  }
  OSData *ranges = OSDynamicCast (OSData, soc->getProperty ("ranges"));
  OSData *reg = OSDynamicCast (OSData, gic->getProperty ("reg"));
  OSData *compatible = OSDynamicCast (OSData, gic->getProperty ("compatible"));
  uint64_t bus[3];
  uint64_t regions[4];
  bool valid = (ranges != nullptr) && (ranges->getLength () == sizeof (bus)) &&
               (reg != nullptr) && (reg->getLength () == sizeof (regions)) &&
               (compatible != nullptr) && (compatible->getLength () == 11) &&
               (memcmp (compatible->getBytesNoCopy (), "arm,gic-v3", 11) == 0);
  if (valid) {
    memcpy (bus, ranges->getBytesNoCopy (), sizeof (bus));
    memcpy (regions, reg->getBytesNoCopy (), sizeof (regions));
  }
  OSData *strideProperty = OSDynamicCast (OSData, gic->getProperty ("redistributor-stride"));
  if (strideProperty != nullptr && strideProperty->getLength () == sizeof (uint64_t)) {
    memcpy (&gRedistStride, strideProperty->getBytesNoCopy (), sizeof (uint64_t));
    if (gRedistStride != 0x20000 && gRedistStride != 0x30000 && gRedistStride != 0x40000) { gRedistStride = 0; }
  }
  armcpu_gic_stride = static_cast<uint32_t> (gRedistStride);
  soc->release ();
  gic->release ();
  if (!valid || (regions[1] < 0x10000) || (regions[3] < 0x20000)) {
    return false;
  }
  for (unsigned index = 0; index < 4; index += 2) {
    if ((regions[index] < bus[0]) || (regions[index] - bus[0] > bus[2]) ||
        (regions[index + 1] > bus[2] - (regions[index] - bus[0])) ||
        (bus[1] + regions[index] - bus[0] < bus[1])) {
      return false;
    }
  }
  OSSharedPtr<IOMemoryDescriptor> distMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[0] - bus[0], regions[1], kIODirectionInOut), OSNoRetain);
  OSSharedPtr<IOMemoryDescriptor> rdMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[2] - bus[0], regions[3], kIODirectionInOut), OSNoRetain);
  if (!distMemory || !rdMemory) {
    return false;
  }
  distributor = OSSharedPtr<IOMemoryMap> (distMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  redistributors = OSSharedPtr<IOMemoryMap> (rdMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  if (!distributor || !redistributors) {
    return false;
  }
  dist = static_cast<uintptr_t> (distributor->getVirtualAddress ());
  if (((read32 (dist, 0xffe8) >> 4) & 15) != 3) {
    return false;
  }
  redistBase = static_cast<uintptr_t> (redistributors->getVirtualAddress ());
  redistSize = regions[3];
  gDistMap = distributor;
  gRedistMap = redistributors;
  gDist = dist;
  gFrames = redistBase;
  gFramesSize = redistSize;
  return true;
}

bool ArmCpu::initDistributor (void)
{
  uint32_t ctl = read32 (dist, 0);
  write32 (dist, 0, ctl & ~3U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  write32 (dist, 0, (ctl & ~3U) | 16U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  unsigned interrupts = ((read32 (dist, 4) & 31) + 1) * 32;
  for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
    write32 (dist, 0x180 + id / 8, 0xffffffff);
  }
  return true;
}

// Runs on the core whose redistributor is configured: the frame is selected by that core's own MPIDR affinity.
bool ArmCpu::wakeRedistributor (bool local)
{
  uintptr_t frames = redistBase;
  // local: this core finds its own frame from MPIDR. Remote: the boot CPU prepares a not-yet-started core's frame
  // from the DT affinity, so the new core never needs GIC MMIO of its own.
  if (local) { __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (affinity)); }
  uint32_t target = static_cast<uint32_t> ((affinity & 0xffffff) | ((affinity >> 8) & 0xff000000));
  redist = 0;
  for (uint64_t offset = 0; offset + 0x20000 <= redistSize;) {
    armcpu_gic_typer_probe[0] = static_cast<uint32_t> (offset);
    uint64_t typer = *reinterpret_cast<volatile uint64_t *> (frames + offset + 8);
    armcpu_gic_typer_probe[1] = static_cast<uint32_t> (typer);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      redist = frames + offset;
      break;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  if (redist == 0) { return false; }
  write32 (redist, 0x14, read32 (redist, 0x14) & ~2U);
  if (!waitClear (redist, 0x14, 4)) { return false; }
  uintptr_t sgi = redist + 0x10000;
  write32 (sgi, 0x180, 0xffffffff);
  write32 (sgi, 0x80, 0xffffffff);
  write32 (sgi, 0xd00, 0);
  write32 (sgi, 0x280, 0xffffffff);
  for (unsigned offset = 0x400; offset < 0x420; offset += 4) {
    write32 (sgi, offset, 0x80808080);
  }
  if (!local) { write32 (sgi, 0x100, kSgiPpiEnable); }
  __asm__ volatile ("dsb sy" ::: "memory");
  return waitClear (redist, 0, 8);
}

bool ArmCpu::prepareGic (void)
{
  if (!mapGic () || !initDistributor () || !wakeRedistributor (true)) { return false; }
  gGicShared = 1;
  return true;
}

void ArmCpu::enableGic (void)
{
  uint64_t value;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  value |= 1;
  __asm__ volatile ("msr icc_sre_el1, %0\n\tisb" :: "r" (value) : "memory");
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  if ((value & 1) == 0) { panic ("ArmCpu: GICv3 system register interface unavailable"); }
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (value));
  value &= ~2ULL; // EOImode 0: EOIR also deactivates the interrupt.
  __asm__ volatile ("msr icc_ctlr_el1, %0" :: "r" (value) : "memory");
  value = 0xff;
  __asm__ volatile ("msr icc_pmr_el1, %0" :: "r" (value) : "memory");
  value = 0;
  __asm__ volatile ("msr icc_bpr1_el1, %0" :: "r" (value) : "memory");
  if (!secondary) { gBootAffinity = affinity & 0xff00ffffffULL; }
  if (!secondary || !gRemoteGic) {
    write32 (redist + 0x10000, 0x100, kSgiPpiEnable);
    write32 (dist, 0, read32 (dist, 0) | 0x12U);
  }
  value = 1;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
  if (!secondary) {
    uint64_t pmr, ig, sre;
    __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (pmr));
    __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (ig));
    __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (sre));
    armcpu_gic_boot[0] = read32 (redist + 0x10000, 0x80);
    armcpu_gic_boot[1] = read32 (redist + 0x10000, 0x100);
    armcpu_gic_boot[2] = read32 (dist, 0);
    armcpu_gic_boot[3] = read32 (dist, 4);
    armcpu_gic_boot[4] = read32 (redist + 0x10000, 0xd00);
    armcpu_gic_boot[5] = static_cast<uint32_t> (pmr);
    armcpu_gic_boot[6] = static_cast<uint32_t> (ig);
    armcpu_gic_boot[7] = static_cast<uint32_t> (sre);
  }
  armcpu_gic_ready = 1;
}


bool ArmCpu::start (IOService *provider)
{
  unsigned skip = 0;
  PE_parse_boot_argn ("armcpu-skip-gic", &skip, sizeof (skip));
  skipGic = skip != 0;
  unsigned selfIpi = 0;
  PE_parse_boot_argn ("armcpu-self-ipi", &selfIpi, sizeof (selfIpi));
  gSelfIpi = selfIpi == 1;
  unsigned spiIpi = 0;
  PE_parse_boot_argn ("armcpu-spi-ipi", &spiIpi, sizeof (spiIpi));
  gSpiIpi = spiIpi == 1;
  unsigned diagnostic = 0;
  PE_parse_boot_argn("armcpu-diagnostics", &diagnostic, sizeof(diagnostic));
  diagnosePower=diagnostic==1;
  unsigned timerIntid = 27;
  if (PE_parse_boot_argn ("armcpu-timer-intid", &timerIntid, sizeof (timerIntid)) && timerIntid >= 16 && timerIntid < 32) {
    gTimerIntid = timerIntid;
  }
  if (!gWdog && PE_parse_boot_argn ("fajita-wdog", &gWdogMode, sizeof (gWdogMode)) && gWdogMode) {
    OSSharedPtr<IOMemoryDescriptor> wdog (IOMemoryDescriptor::withPhysicalAddress (0x17980000, 0x1000, kIODirectionInOut), OSNoRetain);
    if (wdog) gWdogMap = OSSharedPtr<IOMemoryMap> (wdog->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    if (gWdogMap) {
      gWdog = static_cast<uintptr_t> (gWdogMap->getVirtualAddress ());
      armcpu_wdog[0] = 1;
      for (unsigned i = 1; i < 6; i++) armcpu_wdog[i] = read32 (gWdog, 4 * i);   // RST, EN, STS, BARK, BITE
    }
    if (gWdog && gWdogMode >= 3) {
      scmDisableCrashdump ();
      OSSharedPtr<IOMemoryDescriptor> imem (IOMemoryDescriptor::withPhysicalAddress (0x146bf000, 0x1000, kIODirectionInOut), OSNoRetain);
      OSSharedPtr<IOMemoryMap> imemMap = imem ? OSSharedPtr<IOMemoryMap> (imem->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain) : nullptr;
      if (imemMap) {
        uintptr_t base = static_cast<uintptr_t> (imemMap->getVirtualAddress ());
        armcpu_imem[0] = read32 (base, 0x0);
        armcpu_imem[1] = read32 (base, 0x1c);
        armcpu_imem[2] = read32 (base, 0x65c);
        write32 (base, 0x65c, 0x77665500);   // restart reason: bootloader (fastboot)
        armcpu_imem[3] = read32 (base, 0x65c);
      }
      write32 (gWdog, 0x8, 0);         // EN off while programming
      write32 (gWdog, 0x10, 0x7ffff);  // BARK_TIME 16 s
      write32 (gWdog, 0x14, 0xfffff);  // BITE_TIME 32 s
      write32 (gWdog, 0x4, 1);         // RST: restart the count
      // EN: bit 0 counts, bit 1 raises the bark interrupt (Linux qcom_wdt: QCOM_WDT_ENABLE | QCOM_WDT_ENABLE_IRQ). With bit 0 alone a
      // frozen run bit at 32 s without ever barking (runF-bark).
      write32 (gWdog, 0x8, 3);
      armcpu_wdog[2] = read32 (gWdog, 0x8);
      armcpu_wdog[4] = read32 (gWdog, 0x10);
      armcpu_wdog[5] = read32 (gWdog, 0x14);
      PE_parse_boot_argn ("fajita-wdog-bark-cpu", &gBarkCpu, sizeof (gBarkCpu));
      unsigned debug = 0;
      if (PE_parse_boot_argn ("fajita-cpu0-debug", &debug, sizeof (debug)) && debug) {
        // After arming: if this APB read stalls the bus, the watchdog bites.
        OSSharedPtr<IOMemoryDescriptor> d (IOMemoryDescriptor::withPhysicalAddress (0x7010000, 0x1000, kIODirectionInOut), OSNoRetain);
        if (d) gCpu0DebugMap = OSSharedPtr<IOMemoryMap> (d->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        if (gCpu0DebugMap) {
          gCpu0Debug = static_cast<uintptr_t> (gCpu0DebugMap->getVirtualAddress ());
          armcpu_wdog_live[10] = read32 (gCpu0Debug, 0xff4);   // CIDR1: 0x90 or 0xf0 for a CoreSight/debug component
          armcpu_wdog_live[11] = read32 (gCpu0Debug, 0xfbc);   // EDDEVARCH: 0x47706a15 (v8.0 debug) .. 0x47708a15 (v8.2)
          armcpu_wdog_live[12] = read32 (gCpu0Debug, 0x314);   // EDPRSR
          armcpu_wdog_live[13] = cpu0PcSample ();              // 0xffffffff: sampling prohibited (NIDEN off)
        }
      }
    }
  }
  unsigned wdogStop = 0;
  if (gWdog && !gWdogStopTicks && PE_parse_boot_argn ("fajita-wdog-stop", &wdogStop, sizeof (wdogStop)) && wdogStop) {
    uint64_t ticks = 0;
    nanoseconds_to_absolutetime (static_cast<uint64_t> (wdogStop) * NSEC_PER_SEC, &ticks);
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (armcpu_wdog_live[0]));
    gWdogStopTicks = ticks;
    PE_parse_boot_argn ("fajita-wdog-stop-panic", &gWdogStopPanic, sizeof (gWdogStopPanic));
    armcpu_wdog_live[1] = ticks;
  }
  unsigned deadline = 0;
  if (!armcpu_deadline_ticks && PE_parse_boot_argn ("fajita-deadline", &deadline, sizeof (deadline)) && deadline >= 60) {
    uint64_t frequency = 0;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
    // Elapsed time from the first timer interrupt (which records armcpu_deadline_start), never an absolute value: the counter keeps
    // running across warm resets. (A start read here came back as garbage on hardware.)
    armcpu_deadline_start = 0;
    armcpu_deadline_ticks = (frequency >= 1000000 && frequency <= 100000000 ? frequency : 19200000) * (uint64_t)deadline;
  }
  uint32_t physical;
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  memcpy (&physical, reg->getBytesNoCopy (), sizeof (physical));
  logicalId = static_cast<uint32_t> (ml_get_cpu_number (physical));
  secondary = logicalId != static_cast<uint32_t> (ml_get_boot_cpu_number ());
  if (secondary) {
    if (!IOCPU::start (provider)) { return false; }
    armcpu_smp_step[logicalId] = 1;
    affinity = physical;
    armcpu_smp_mpidr[logicalId] = physical;
    cpuName = OSSymbol::withCStringNoCopy ("ARM architectural secondary CPU");
    if (cpuName == nullptr) { return false; }
    retain ();
    __atomic_store_n (&gPending[logicalId], this, __ATOMIC_RELEASE);
    if (gBootRegistered != 0) { drainPending (); }
    return true;
  } else {
    uint64_t bootMpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (bootMpidr));
    armcpu_boot_mpidr = static_cast<uint32_t> (bootMpidr);
    if ((!skipGic && !prepareGic ()) || !IOCPU::start (provider)) { return false; }
  }
  cpuName = OSSymbol::withCStringNoCopy ("ARM architectural boot CPU");
  if (cpuName == nullptr) { return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = physical;
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (physical));
  info.cluster_type = ml_get_boot_cluster_type ();
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { panic ("ArmCpu: CPU %u registration failed: %d", logicalId, status); }
  setCPUNumber (info.log_id);
  armcpu_registered = 1;
  unsigned smp = 0;
  PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
  gSmpWanted = smp != 0;
  unsigned remote = 0;
  PE_parse_boot_argn ("sheng-smp-gic", &remote, sizeof (remote));
  gRemoteGic = remote != 0;
  armcpu_smp_requested = gSmpWanted ? 1 : 0;
  unsigned cpuCount = static_cast<unsigned> (ml_get_max_cpu_number ()) + 1;
  controller = new IOCPUInterruptController;
  if ((controller == nullptr) || (controller->initCPUInterruptController (cpuCount, 1) != kIOReturnSuccess)) {
    panic ("ArmCpu: CPU interrupt controller allocation failed");
  }
  controller->registerCPUInterruptController ();
  controller->enableCPUInterrupt (this);
  if (controller->registerInterrupt (provider, logicalId, this, &ArmCpu::dispatch, nullptr) != kIOReturnSuccess) {
    panic ("ArmCpu: CPU IRQ action registration failed");
  }
  gController = controller;
  initCPU (true);
  registerService ();
  __atomic_store_n (&gBootRegistered, 1U, __ATOMIC_RELEASE);
  drainPending ();
  return true;
}

void ArmCpu::drainPending (void)
{
  for (unsigned id = 1; id < ARMCPU_SMP_SLOTS; id++) {
    ArmCpu *cpu = __atomic_exchange_n (&gPending[id], static_cast<ArmCpu *> (nullptr), __ATOMIC_ACQ_REL);
    if (cpu != nullptr) { cpu->finishSecondary (); }
  }
}

bool ArmCpu::finishSecondary (void)
{
  dist = gDist;
  redistBase = gFrames;
  redistSize = gFramesSize;
  if (gRemoteGic && !wakeRedistributor (false)) { armcpu_smp_fail[logicalId] = 3; return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = static_cast<uint32_t> (affinity);
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (static_cast<uint32_t> (affinity)));
  info.cluster_type = ml_get_boot_cluster_type ();
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { armcpu_smp_fail[logicalId] = 4; return false; }
  setCPUNumber (info.log_id);
  armcpu_smp_step[logicalId] = 2;
  setCPUState (kIOCPUStateUninitalized);
  registerService ();
  startSecondary ();
  return true;
}

// PSCI conduit comes from the /psci node of the device tree ("smc" or "hvc"); no default is assumed.
static bool psciConduit (bool *hvc)
{
  IORegistryEntry *psci = IORegistryEntry::fromPath ("/psci", gIODTPlane);
  if (psci == nullptr) { return false; }
  OSData *method = OSDynamicCast (OSData, psci->getProperty ("method"));
  bool ok = false;
  if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "smc", 4) == 0) {
    *hvc = false; ok = true;
  } else if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "hvc", 4) == 0) {
    *hvc = true; ok = true;
  }
  psci->release ();
  return ok;
}

static int64_t psciCall (bool hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
  register uint64_t x0 __asm__ ("x0") = function;
  register uint64_t x1 __asm__ ("x1") = a;
  register uint64_t x2 __asm__ ("x2") = b;
  register uint64_t x3 __asm__ ("x3") = c;
  if (hvc) {
    __asm__ volatile ("hvc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  } else {
    __asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  }
  return static_cast<int64_t> (x0);
}

void ArmCpu::startThread (void *argument, wait_result_t)
{
  ArmCpu *self = static_cast<ArmCpu *> (argument);
  armcpu_smp_step[self->logicalId] = 3;
  // Blocks until the new core reaches the scheduler; a core that never arrives only stalls this helper thread.
  processor_start (self->machProcessor);
  thread_terminate (current_thread ());
}

void ArmCpu::startSecondary (void)
{
  thread_t thread;
  if (kernel_thread_start (&ArmCpu::startThread, this, &thread) != KERN_SUCCESS) {
    armcpu_smp_psci_rc[logicalId] = -1000;
    return;
  }
  thread_deallocate (thread);
}

void ArmCpu::initCPU (bool boot)
{
  if (secondary) {
    if (!boot) { panic ("ArmCpu: secondary resume is unsupported"); }
    armcpu_smp_step[logicalId] = 5;
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
    armcpu_smp_seen_mpidr[logicalId] = static_cast<uint32_t> (mpidr);
    // A failure is recorded and the core parked (rather than a panic that hides the other cores' state).
    if ((mpidr & 0xffff) != (affinity & 0xffff)) { armcpu_smp_fail[logicalId] = 1; for (;;) { __asm__ volatile ("wfe"); } }
    if (!gRemoteGic && !wakeRedistributor (true)) { armcpu_smp_fail[logicalId] = 2; for (;;) { __asm__ volatile ("wfe"); } }
    armcpu_smp_step[logicalId] = 6;
    enableGic ();
    if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
    // The shared IOCPUInterruptController blocks registrations until every core is enabled, so the secondary cores
    // install their handler directly. ml_install_interrupt_handler() records it in the calling core's cpu_data.
    ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
    enableCPUTimeBase (true);
    setCPUState (kIOCPUStateRunning);
    armcpu_smp_step[logicalId] = 7;
    armcpu_smp_running = armcpu_smp_running + 1;
    // XNU clears SIGPdisabled for a core the first time that core takes an IPI, and cpu_signal() silently skips a core until then; Apple's
    // CPU driver provokes it with a self-IPI from initCPU (osfmk/kern/machine.c, processor_up).
    if (gSelfIpi) { signalCPU (this); }
    return;
  }
  if (!boot) { panic ("ArmCpu: secondary/resume CPU startup is unsupported"); }
  if (skipGic) {
    // Explicit A/B hardware diagnostic: do not touch GIC MMIO or enable an
    // interrupt source. CPU registration still happens; the timer is left off.
    setCPUState (kIOCPUStateRunning);
    return;
  }
  enableGic ();
  if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
  if (gWdog && gWdogMode >= 3 && gDist && logicalId == gBarkCpu) { spiConfigure (gDist, kBarkIntid, affinity); }   // see gBarkCpu
  enableCPUTimeBase (true);
  setCPUState (kIOCPUStateRunning);
  if (gSelfIpi) { signalCPU (this); }
}

void ArmCpu::dispatch (void *target, void *, void *, int)
{
  static_cast<ArmCpu *> (target)->interrupt ();
}

void ArmCpu::interrupt (void)
{
  unsigned drained=0;
  for (;;) {
    armcpu_irq_phase=1;
    uint64_t id;
    __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (id));
    armcpu_irq_id=static_cast<uint32_t>(id & 0xffffff);
    if ((id & 0xffffff) >= 1020) { armcpu_irq_phase=0;break; }
    if(diagnosePower && ++drained>128)panic("ArmCpu: IRQ drain exceeds 128 id=%llx timer=%llu IPI=%llu",id,armcpu_timer_interrupts,armcpu_ipi_interrupts);
    if ((id & 0xffffff) == gTimerIntid) {
      armcpu_timer_interrupts = armcpu_timer_interrupts + 1;
      armcpu_core_timer[logicalId & 7] = armcpu_core_timer[logicalId & 7] + 1;
      if (gWdog && gWdogMode >= 2 && logicalId == 0) wdogPet (true);   // boot CPU only (see armcpu_wdog_pet)
      if (armcpu_deadline_ticks) {
        uint64_t now;
        __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (now));
        armcpu_deadline_seen = now;
        if (!armcpu_deadline_start) armcpu_deadline_start = now;
        // Unsigned difference: correct across the wrap (fajita's virtual counter starts near 2^64 and wraps ~20 minutes in).
        if (now - armcpu_deadline_start >= armcpu_deadline_ticks) {
          register uint64_t x0 __asm__ ("x0") = 0x84000009ULL;   // PSCI SYSTEM_RESET
          __asm__ volatile ("smc #0" : "+r" (x0) :: "memory", "x1", "x2", "x3");
        }
      }
      armcpu_irq_phase=2;
      rtclock_intr (0);
      armcpu_irq_phase=3;
    } else if ((id & 0xffffff) == kBarkIntid && gWdogMode >= 3) {
      barkCapture ();
    } else if ((id & 0xffffff) == 0) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      armcpu_irq_phase=4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase=5;
    } else if (gSpiIpi && (id & 0xffffff) >= kIpiSpiBase && (id & 0xffffff) < kIpiSpiBase + 8) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      armcpu_irq_phase = 4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase = 5;
    } else {
      const uint32_t intid = static_cast<uint32_t> (id & 0xffffff);
      ArmCpuSpi *spi = nullptr;
      const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
      for (uint32_t slot = 0; intid >= 32 && slot < used && spi == nullptr; slot++) {
        if (gSpi[slot].id == intid) { spi = &gSpi[slot]; }
      }
      if (spi != nullptr) {
        armcpu_irq_phase = 7;
        spi->count = spi->count + 1;
        spi->handler (spi->context);
      } else if (intid >= 32 && gDist != 0) {
        armcpu_spi_unhandled = armcpu_spi_unhandled + 1;
        armcpu_spi_unhandled_id = intid;
        write32 (gDist, 0x180 + (intid / 32) * 4, 1U << (intid % 32));
      } else {
        panic ("ArmCpu: unexpected architectural IRQ %llu", id);
      }
    }
    armcpu_irq_phase=6;
    __asm__ volatile ("msr icc_eoir1_el1, %0\n\tisb" :: "r" (id) : "memory");
  }
}

void ArmCpu::signalCPU (IOCPU *target)
{
  ArmCpu *peer = OSDynamicCast (ArmCpu, target);
  if (peer == nullptr || (peer->signalTarget () & 0xff) >= 16) {
    panic ("ArmCpu: unsupported CPU signal target");
  }
  uint64_t aff = peer->signalTarget ();
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) |
                 ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_sent = armcpu_ipi_sent + 1;
  if (gSpiIpi) { spiIpiSend (gDist, peer->logicalId & 7); return; }
  __asm__ volatile ("dsb ishst\n\tmsr icc_sgi1r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
}

// Diagnostic: sends SGI 0 to the core whose MPIDR affinity is aff (the DT "reg" value, e.g. 0x100 for core 1) exactly as signalCPU
// does. The handler on the target runs the kernel's IPI callback with nothing pending, which is harmless. Returns nothing: the caller
// compares armcpu_ipi_interrupts before and after.
// Read-only snapshot of the GIC state that decides whether SGI 0 reaches the core with MPIDR affinity aff: its redistributor's SGI frame and
// the distributor control register. out: 0 IGROUPR0, 1 ISENABLER0, 2 ISPENDR0, 3 IGRPMODR0, 4 GICR_WAKER, 5 GICD_CTLR, 6 IPRIORITYR0, 7 found(1).
// armcpu_gic_diag_stage names the access in progress, so a read that never returns is identified from another thread:
// 0x100+frame walk, 2 IGROUPR0, 3 ISENABLER0, 4 ISPENDR0, 5 IGRPMODR0, 6 WAKER, 7 GICD_CTLR, 8 IPRIORITYR0, 9 done, 0xe not found.
volatile uint32_t armcpu_gic_diag_stage = 0;
extern "C" void armcpu_gic_diag (uint64_t aff, uint32_t *out)
{
  for (unsigned i = 0; i < 8; i++) { out[i] = 0xdeadbeef; }
  if (gDist == 0 || gFrames == 0) { armcpu_gic_diag_stage = 0xe; return; }
  const uint32_t target = static_cast<uint32_t> ((aff & 0xffffff) | ((aff >> 8) & 0xff000000));
  for (uint64_t offset = 0; offset + 0x20000 <= gFramesSize;) {
    armcpu_gic_diag_stage = 0x100 + static_cast<uint32_t> (offset >> 16);
    const uint64_t typer = *reinterpret_cast<volatile uint64_t *> (gFrames + offset + 8);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      const uintptr_t redist = gFrames + offset, sgi = redist + 0x10000;
      armcpu_gic_diag_stage = 2; out[0] = read32 (sgi, 0x80);
      armcpu_gic_diag_stage = 3; out[1] = read32 (sgi, 0x100);
      armcpu_gic_diag_stage = 4; out[2] = read32 (sgi, 0x200);
      armcpu_gic_diag_stage = 5; out[3] = read32 (sgi, 0xd00);
      armcpu_gic_diag_stage = 6; out[4] = read32 (redist, 0x14);
      armcpu_gic_diag_stage = 7; out[5] = read32 (gDist, 0);
      armcpu_gic_diag_stage = 8; out[6] = read32 (sgi, 0x400);
      out[7] = 1;
      armcpu_gic_diag_stage = 9;
      return;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  armcpu_gic_diag_stage = 0xe;
}

// Does an SGI sent to the calling core itself become pending at its own CPU interface, and which INTIDs? Only system registers are used (no GIC
// MMIO). With IRQs masked, SGIs 0..15 are sent one at a time to this core's own MPIDR affinity through ICC_SGI1R_EL1 (Group 1) and, when that
// shows nothing, through ICC_SGI0R_EL1 (Group 0); ICC_HPPIR1_EL1 / ICC_HPPIR0_EL1 is polled and a pending SGI is acknowledged and ended here so the
// kernel never sees it. out[8], out[9]: CNTVCT ticks of one ICC_SGI1R_EL1 write and of one ICC_PMR_EL1 read (a trapped write costs far more). out: 0 MPIDR, 1 INTIDs pending as Group 1, 2 INTIDs pending as Group 0, 3 ICC_PMR, 4 ICC_IGRPEN1, 5 ICC_SRE, 6 ICC_CTLR, 7 ICC_RPR.
extern "C" void armcpu_sgi_selfprobe (uint64_t *out)
{
  uint64_t mpidr, daif, v;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
  __asm__ volatile ("mrs %0, daif\n\tmsr daifset, #2\n\tisb" : "=r" (daif) :: "memory");
  for (unsigned i = 0; i < 10; i++) { out[i] = 0; }
  out[0] = mpidr;
  __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (v)); out[3] = v;
  __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (v)); out[4] = v;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (v)); out[5] = v;
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (v)); out[6] = v;
  __asm__ volatile ("mrs %0, icc_rpr_el1" : "=r" (v)); out[7] = v;
  if ((mpidr & 0xff) < 16) {
    const uint64_t base = ((mpidr & 0xff00000000ULL) << 16) | ((mpidr & 0xff0000) << 16) | ((mpidr & 0xff00) << 8) | (1ULL << (mpidr & 0xff));
    for (uint64_t intid = 0; intid < 16; intid++) {
      const uint64_t sgi = base | (intid << 24);
      uint64_t h = 1023, ta, tb;
      __asm__ volatile ("dsb ishst\n\tisb\n\tmrs %0, cntvct_el0\n\tisb" : "=r" (ta) :: "memory");
      __asm__ volatile ("msr icc_sgi1r_el1, %0\n\tisb\n\tmrs %1, cntvct_el0" : "=r" (tb) : "r" (sgi) : "memory");
      if (intid == 0) { out[8] = tb - ta; }
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir1_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[1] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir1_el1, %0\n\tisb" :: "r" (v) : "memory");
        continue;
      }
      __asm__ volatile ("dsb ishst\n\tmsr icc_sgi0r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir0_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[2] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar0_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir0_el1, %0\n\tisb" :: "r" (v) : "memory");
      }
    }
  }
  {
    uint64_t ta, tb;
    __asm__ volatile ("isb\n\tmrs %0, cntvct_el0\n\tisb\n\tmrs %2, icc_pmr_el1\n\tisb\n\tmrs %1, cntvct_el0" : "=r" (ta), "=r" (tb), "=r" (v) :: "memory");
    out[9] = tb - ta;
  }
  __asm__ volatile ("msr daif, %0\n\tisb" :: "r" (daif) : "memory");
}

extern "C" void armcpu_ipi_selftest (uint64_t aff)
{
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) | ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_selftest_sent = armcpu_ipi_selftest_sent + 1;
  if (gSpiIpi) { spiIpiSend (gDist, static_cast<uint32_t> ((aff >> 8) & 7)); return; }
  __asm__ volatile ("dsb ishst\n\tmsr icc_sgi1r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
}

void ArmCpu::enableCPUTimeBase (bool enabled)
{
  uint64_t value = enabled ? 1 : 2;
  __asm__ volatile ("msr cntv_ctl_el0, %0\n\tisb" :: "r" (value) : "memory");
}

void ArmCpu::quiesceCPU (void)
{
  armcpu_quiesce_calls=armcpu_quiesce_calls+1;
  if(diagnosePower)panic("ArmCpu: unsupported quiesce requested before timer/GIC disable caller=%p halt=%u timer=%llu phase=%u",__builtin_return_address(0),armcpu_halt_calls,armcpu_timer_interrupts,armcpu_irq_phase);
  enableCPUTimeBase (false);
  uint64_t value = 0;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
}

// The boot CPU never parks: a system-sleep request (powerd after the ~2 minute idle timer) reaches IOCPUSleepKernel, which halts the
// boot CPU last, and with no sleep/wake path here that left CPU0 with its timer, GIC and IRQs off for good: fajita's freeze ~119 s
// after XNU started (HackiOS saw the same: hackios-xnu-kernel redwood.patch, GenericARMPE.cpp haltCPU). Returning makes the sleep
// a no-op wake. fajita-halt-panic=1 panics instead, naming the caller.
void ArmCpu::haltCPU (void)
{
  armcpu_halt_calls=armcpu_halt_calls+1;
  if (logicalId == 0) {
    unsigned haltPanic = 0;
    if (PE_parse_boot_argn ("fajita-halt-panic", &haltPanic, sizeof (haltPanic)) && haltPanic) {
      panic ("FAJITA HALT: haltCPU on the boot CPU, caller=%p halts=%u timer irqs=%llu", __builtin_return_address (0),
             armcpu_halt_calls, (unsigned long long)armcpu_timer_interrupts);
    }
    return;
  }
  quiesceCPU ();
  setCPUState (kIOCPUStateStopped);
  __asm__ volatile ("msr daifset, #15" ::: "memory");
  for (;;) { __asm__ volatile ("wfi"); }
}

// The new core runs with its MMU and caches off until start_cpu enables them, so it reads the reset vector, ResetHandlerData
// and the boot arguments from memory. Write those lines back to the point of coherency first: on this SoC the boot CPU's
// dirty cache lines are not guaranteed to be visible to a core with caches off (XNU only flushes the per-CPU data).
static void cleanToMemory (const void *start, size_t length)
{
  uint64_t ctr;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
  uintptr_t line = static_cast<uintptr_t> (4) << ((ctr >> 16) & 15);
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~(line - 1), end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += line) { __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory"); }
  __asm__ volatile ("dsb sy\n\tisb" ::: "memory");
}

kern_return_t ArmCpu::startCPU (vm_offset_t, vm_offset_t)
{
  if (!secondary) { return KERN_NOT_SUPPORTED; }
  bool hvc = false;
  if (!psciConduit (&hvc)) { armcpu_smp_psci_rc[logicalId] = -1001; return KERN_NOT_SUPPORTED; }
  armcpu_smp_conduit = hvc ? 2 : 1;
  // PSCI 0.2 CPU_ON (SMC64 function id); the entry point is the kernel's physical reset vector page, whose reset
  // code selects this core's cpu_data entry by MPIDR affinity. The context id is unused by that code.
  uint64_t entry = static_cast<uint64_t> (ml_static_vtop (reinterpret_cast<vm_offset_t> (LowResetVectorBase)));
  cleanToMemory (LowResetVectorBase, 0x10000);   // reset vector, start_cpu and the code up to the MMU enable
  cleanToMemory (ResetHandlerData, 0x1000);
  cleanToMemory (const_boot_args, 0x1000);
  armcpu_smp_step[logicalId] = 4;
  int64_t result = psciCall (hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0);
  armcpu_smp_psci_rc[logicalId] = static_cast<int32_t> (result);
  return result == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

const OSSymbol *ArmCpu::getCPUName (void)
{
  return cpuName;
}

// Readback after configuration: bit 0 group 1, bit 1 enabled, bit 2 edge, bits 8..15 priority; IROUTER separately.
extern "C" IOReturn armcpu_spi_attach (uint32_t intid, bool edge, void (*handler) (void *), void *context,
                                       uint32_t *readback, uint64_t *router)
{
  if (gDist == 0 || armcpu_gic_ready == 0 || handler == nullptr) { return kIOReturnNotReady; }
  const unsigned interrupts = ((read32 (gDist, 4) & 31) + 1) * 32;
  if (intid < 32 || intid >= 1020 || intid >= interrupts) { return kIOReturnBadArgument; }
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return kIOReturnExclusiveAccess; }
  }
  if (used >= ARMCPU_SPI_SLOTS) { return kIOReturnNoResources; }
  const uint32_t word = (intid / 32) * 4, bit = 1U << (intid % 32);
  write32 (gDist, 0x180 + word, bit); // ICENABLER while it is reconfigured
  if (!waitClear (gDist, 0, 1U << 31)) { return kIOReturnTimeout; }
  write32 (gDist, 0x80 + word, read32 (gDist, 0x80 + word) | bit); // IGROUPR: group 1 (RAZ/WI if secure owns it)
  *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid) = 0xa0;
  const unsigned cfg = 0xc00 + (intid / 16) * 4, shift = (intid % 16) * 2 + 1;
  write32 (gDist, cfg, (read32 (gDist, cfg) & ~(1U << shift)) | ((edge ? 1U : 0U) << shift));
  *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid) = gBootAffinity;
  gSpi[used] = {intid, handler, context, 0};
  __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gDist, 0x100 + word, bit); // ISENABLER
  if (readback != nullptr) {
    *readback = ((read32 (gDist, 0x80 + word) & bit) ? 1U : 0U) | ((read32 (gDist, 0x100 + word) & bit) ? 2U : 0U) |
                (((read32 (gDist, cfg) >> shift) & 1U) << 2) |
                (static_cast<uint32_t> (*reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid)) << 8);
  }
  if (router != nullptr) { *router = *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid); }
  return kIOReturnSuccess;
}

extern "C" uint64_t armcpu_spi_count (uint32_t intid)
{
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return gSpi[slot].count; }
  }
  return 0;
}
