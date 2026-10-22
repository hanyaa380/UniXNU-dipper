// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita CPU clocks: the SDM845 OSM ("qcom,cpufreq-hw" v1) frequency domains and the OSM L3, plus a per-core clock measurement.
// `fajita-cpufreq=<n>` (default off: nothing is mapped or read without it):
//   1  read only: each domain's enable, dcvs_ctrl, perf_state (requested LUT index), current_vote (applied L value) and its LUT;
//      every core's clock measured before anything is written
//   2  additionally request the highest LUT level of silver (d0), gold (d1) and the L3, then measure again
// `fajita-cpufreq-cap=<kHz>` (a value below 100000 is taken as MHz): mode 2 picks the highest level at or below it (CPU domains only).
// `fajita-cpufreq-turbo=1`: allow the LUT's turbo rows (core count 1, e.g. gold 2745.6/2803.2 MHz); default off, as Linux without boost.
// `fajita-cpufreq-pmu=1`: also count cycles with PMCCNTR_EL0 during the measurement (traps to EL2 would panic: opt-in only).
// `fajita-cpufreq-period=<s>`: re-measure every <s> seconds (default 30, 0 = only before and after the vote).
//
// Register layout (Linux drivers/cpufreq/qcom-cpufreq-hw.c qcom_soc_data, sdm845.dtsi cpufreq@17d43000 / interconnect@17d41000):
// enable 0x0 (bit 0), dcvs_ctrl 0xbc (bit 0: per-core perf_state rows, +4*i), LUT 0x110 + 32*i (31:30 source, 18:16 core count,
// 7:0 L value), volt LUT 0x114 + 32*i (11:0 mV, read only here), current_vote 0x704 (9:0 L value), perf_state 0x920 (LUT index).
// Source 0 is GPLL0/2 = 300 MHz, otherwise L * 19.2 MHz; the table ends at two equal frequencies. Voltages are never written:
// the OSM firmware sequences PLL, CPR voltage and LMh for the requested index (Linux writes nothing but perf_state either).
//
// Measurement: a thread bound to each CPU runs, with interrupts off, 20000 iterations of 100 dependent adds (1 cycle each on A55
// and A75) between two CNTPCT reads: MHz = adds / elapsed. The PMU count, when enabled, checks that one add is one cycle.
// Everything runs in its own kernel thread, started from the boot progress monitor (sheng_cpufreq_poll, once a second) once IOKit
// is up and, when fajita-wdog is set, after ArmCpu armed the APSS watchdog, so a bus hang on the OSM windows ends in a bite.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern "C" {
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;
char fajita_cpufreq_rows[6][256];
processor_t thread_bind(processor_t);
processor_t cpu_to_processor(int);
int cpu_number(void);
int ml_get_max_cpu_number(void);
boolean_t ml_set_interrupts_enabled(boolean_t);
wait_result_t thread_block(thread_continue_t);
kern_return_t kernel_thread_start_priority(thread_continue_t, void *, integer_t, thread_t *);
extern volatile uint32_t armcpu_wdog[6] __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_running __attribute__((weak_import));
}

namespace {
constexpr uint32_t kRegEnable = 0x0, kRegDcvsCtrl = 0xbc, kRegFreqLut = 0x110, kRegVoltLut = 0x114, kRegCurrentVote = 0x704,
                   kRegPerfState = 0x920, kLutRow = 32, kLutEntries = 40;
constexpr uint32_t kXoKHz = 19200, kGpllHalfKHz = 300000;
constexpr unsigned kCpus = 8, kIters = 20000;   // 100 adds per iteration (addChain)

struct Domain {
    const char *name;
    uint64_t phys;          // page-aligned window start
    uint32_t offset;        // the block's offset inside it
    unsigned cpus;          // CPUs of the domain (per-core perf_state rows)
    bool cpu;               // CPU domain (current_vote, turbo rows, cap) or L3
    OSSharedPtr<IOMemoryMap> map;
    uintptr_t va;
    unsigned rows;
    uint32_t khz[kLutEntries];
    uint16_t mv[kLutEntries];
    bool turbo[kLutEntries];
    uint32_t psBefore;
    int voted;              // index written, -1 none
    uint32_t votedKHz;
};
Domain gDom[3] = {
    {"d0", 0x17d43000, 0x000, 4, true, nullptr, 0, 0, {}, {}, {}, 0, -1, 0},
    {"d1", 0x17d45000, 0x800, 4, true, nullptr, 0, 0, {}, {}, {}, 0, -1, 0},
    {"L3", 0x17d41000, 0x000, 0, false, nullptr, 0, 0, {}, {}, {}, 0, -1, 0},
};
unsigned gMode, gTurbo, gPmu, gPeriod = 30;
uint32_t gCapKHz;
bool gStarted;

struct Sample {
    volatile uint32_t done;
    int ranOn;              // cpu_number() after the bind (-1: never ran, -2: no processor)
    uint32_t mhz;
    uint64_t cycles;        // PMCCNTR delta (pmu=1)
    uint64_t adds;
};
Sample gSample[kCpus];
volatile uint32_t gBusy[kCpus];

uint32_t rd32(const Domain &d, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(d.va + d.offset + reg);}
void wr32(const Domain &d, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(d.va + d.offset + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint64_t ticks() {uint64_t t; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t) :: "memory"); return t;}
uint64_t tickHz() {uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 19200000;}
unsigned uptime() {return (unsigned)(ticks() / tickHz());}

uint64_t addChain(uint64_t iters) {
    uint64_t x = 0;
    __asm__ volatile("1:\n\t.rept 100\n\tadd %0, %0, #1\n\t.endr\n\tsubs %1, %1, #1\n\tb.ne 1b" : "+r"(x), "+r"(iters) :: "cc");
    return x;
}

void measureThread(void *arg, wait_result_t) {
    const unsigned cpu = (unsigned)(uintptr_t)arg;
    Sample &s = gSample[cpu];
    processor_t p = cpu_to_processor((int)cpu);
    if (!p) {s.ranOn = -2; s.done = 1; gBusy[cpu] = 0; return;}
    thread_bind(p);
    thread_block(THREAD_CONTINUE_NULL);
    boolean_t ie = ml_set_interrupts_enabled(FALSE);
    s.ranOn = cpu_number();
    addChain(kIters / 10);   // warm up the loop (and give the OSM a moment if the core just left idle)
    uint64_t c0 = 0, c1 = 0;
    if (gPmu) {
        uint64_t pmcr;
        __asm__ volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
        __asm__ volatile("msr pmccfiltr_el0, xzr\n\tmsr pmcntenset_el0, %0\n\tmsr pmcr_el0, %1\n\tisb"
                         :: "r"(1ULL << 31), "r"(pmcr | 1) : "memory");
        __asm__ volatile("isb\n\tmrs %0, pmccntr_el0" : "=r"(c0) :: "memory");
    }
    uint64_t t0 = ticks();
    uint64_t adds = addChain(kIters);
    uint64_t t1 = ticks();
    if (gPmu) __asm__ volatile("isb\n\tmrs %0, pmccntr_el0" : "=r"(c1) :: "memory");
    ml_set_interrupts_enabled(ie);
    s.adds = adds;
    s.cycles = c1 - c0;
    s.mhz = t1 > t0 ? (uint32_t)(adds * tickHz() / ((t1 - t0) * 1000000)) : 0;
    thread_bind(PROCESSOR_NULL);
    __atomic_store_n(&s.done, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&gBusy[cpu], 0, __ATOMIC_RELEASE);
}

// One sample per CPU, each from its own bound thread, waited for at most 300 ms; a CPU that never runs it keeps its slot busy.
void measure(uint32_t out[kCpus], uint32_t ipc[kCpus]) {
    int max = ml_get_max_cpu_number();
    for (unsigned cpu = 0; cpu < kCpus; ++cpu) {
        out[cpu] = 0; ipc[cpu] = 0;
        if ((int)cpu > max) continue;
        if (cpu && &armcpu_smp_running && cpu > armcpu_smp_running) continue;   // secondaries not started
        if (gBusy[cpu]) {out[cpu] = 1; continue;}                               // 1: the previous sample never ran
        Sample &s = gSample[cpu];
        s.done = 0; s.ranOn = -1; s.mhz = 0; s.cycles = 0;
        gBusy[cpu] = 1;
        thread_t t = nullptr;
        if (kernel_thread_start_priority(measureThread, (void *)(uintptr_t)cpu, 95, &t) != KERN_SUCCESS) {gBusy[cpu] = 0; continue;}
        thread_deallocate(t);
        for (unsigned ms = 0; ms < 300 && !__atomic_load_n(&s.done, __ATOMIC_ACQUIRE); ++ms) IOSleep(1);
        if (!s.done) {out[cpu] = 1; continue;}
        out[cpu] = s.ranOn == (int)cpu ? s.mhz : 2;                              // 2: ran on another CPU
        if (s.cycles) ipc[cpu] = (uint32_t)(s.adds * 100 / s.cycles);
    }
}

void mhzRow(char *row, const char *tag, unsigned pass, const uint32_t mhz[kCpus], const uint32_t ipc[kCpus]) {
    int n = snprintf(row, 256, "[cpufreq %s] t=%us pass=%u MHz", tag, uptime(), pass);
    for (unsigned c = 0; c < kCpus && n > 0 && n < 256; ++c) n += snprintf(row + n, 256 - n, " c%u=%u", c, mhz[c]);
    if (gPmu)
        for (unsigned c = 0; c < kCpus && n > 0 && n < 256; ++c) n += snprintf(row + n, 256 - n, "%s%u", c ? "," : " adds/cyc%=", ipc[c]);
}

bool mapDomain(Domain &d) {
    OSSharedPtr<IOMemoryDescriptor> md(IOMemoryDescriptor::withPhysicalAddress(d.phys, 0x2000, kIODirectionInOut), OSNoRetain);
    if (md) d.map = OSSharedPtr<IOMemoryMap>(md->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    d.va = d.map ? (uintptr_t)d.map->getVirtualAddress() : 0;
    return d.va != 0;
}

void readLut(Domain &d) {
    uint32_t prev = 0;
    d.rows = 0;
    for (unsigned i = 0; i < kLutEntries; ++i) {
        uint32_t row = rd32(d, kRegFreqLut + i * kLutRow);
        uint32_t khz = (row >> 30) ? kXoKHz * (row & 0xff) : kGpllHalfKHz;
        if (i > 0 && khz == prev) break;
        d.khz[i] = khz;
        d.mv[i] = (uint16_t)(rd32(d, kRegVoltLut + i * kLutRow) & 0xfff);
        d.turbo[i] = d.cpu && ((row >> 16) & 7) == 1;
        d.rows = i + 1;
        prev = khz;
    }
}

int pick(const Domain &d) {
    int best = -1; uint32_t bestKHz = 0;
    for (unsigned i = 0; i < d.rows; ++i) {
        if (d.turbo[i] && !gTurbo) continue;
        if (d.cpu && gCapKHz && d.khz[i] > gCapKHz) continue;
        if (d.khz[i] >= bestKHz) {best = (int)i; bestKHz = d.khz[i];}
    }
    return best;
}

void vote(Domain &d) {
    int level = pick(d);
    if (level < 0) return;
    wr32(d, kRegPerfState, (uint32_t)level);
    if (d.cpu && (rd32(d, kRegDcvsCtrl) & 1))
        for (unsigned c = 1; c < d.cpus; ++c) wr32(d, kRegPerfState + c * 4, (uint32_t)level);
    d.voted = level;
    d.votedKHz = d.khz[level];
}

void lutRow(char *row, const Domain &d) {
    int n = snprintf(row, 256, "[cpufreq lut %s] rows=%u MHz", d.name, d.rows);
    for (unsigned i = 0; i < d.rows && n > 0 && n < 256; ++i) n += snprintf(row + n, 256 - n, " %u%s", d.khz[i] / 1000, d.turbo[i] ? "*" : "");
    if (d.rows && n > 0 && n < 256) snprintf(row + n, 256 - n, " | mV %u..%u", d.mv[0], d.mv[d.rows - 1]);
}

void summary() {
    char *out = native_cpufreq_row;
    int n = snprintf(out, sizeof native_cpufreq_row, "m%u", gMode);
    for (const Domain &d : gDom) {
        if (n <= 0 || n >= (int)sizeof native_cpufreq_row) break;
        size_t cap = sizeof native_cpufreq_row - n;
        if (!d.va) {n += snprintf(out + n, cap, " %s:nomap", d.name); continue;}
        uint32_t en = rd32(d, kRegEnable) & 1, ps = rd32(d, kRegPerfState);
        uint32_t top = d.rows ? d.khz[d.rows - 1] / 1000 : 0;
        if (d.cpu) {
            uint32_t cur = (rd32(d, kRegCurrentVote) & 0x3ff) * kXoKHz / 1000;
            n += snprintf(out + n, cap, " %s:e%u dc%u ps%u>%u cur%u top%u", d.name, en, rd32(d, kRegDcvsCtrl) & 1, d.psBefore, ps, cur, top);
        } else {
            n += snprintf(out + n, cap, " %s:e%u ps%u>%u @%u top%u", d.name, en, d.psBefore, ps, ps < d.rows ? d.khz[ps] / 1000 : 0, top);
        }
    }
    snprintf(fajita_cpufreq_rows[0], 256, "[cpufreq] %s | cap=%ukHz turbo=%u pmu=%u wdog=%u", native_cpufreq_row, gCapKHz, gTurbo, gPmu,
             &armcpu_wdog ? armcpu_wdog[0] : 0);
    native_cpufreq_stage = 2;
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-cpufreq");
    unsigned wdog = 0;
    if (PE_parse_boot_argn("fajita-wdog", &wdog, sizeof wdog) && wdog && &armcpu_wdog)
        for (unsigned s = 0; s < 120 && !armcpu_wdog[0]; ++s) IOSleep(1000);   // ArmCpu maps and arms it in its start
    for (Domain &d : gDom) {
        if (!mapDomain(d)) continue;
        d.psBefore = rd32(d, kRegPerfState);
        readLut(d);
    }
    lutRow(fajita_cpufreq_rows[3], gDom[0]);
    lutRow(fajita_cpufreq_rows[4], gDom[1]);
    lutRow(fajita_cpufreq_rows[5], gDom[2]);
    summary();
    uint32_t mhz[kCpus], ipc[kCpus];
    measure(mhz, ipc);
    mhzRow(fajita_cpufreq_rows[1], "pre", 0, mhz, ipc);
    IOLog("fajita-cpufreq: %s\n%s\n", fajita_cpufreq_rows[0], fajita_cpufreq_rows[1]);
    unsigned pass = 0;
    for (;;) {
        if (gMode >= 2)
            for (Domain &d : gDom)
                if (d.va && d.voted < 0 && (rd32(d, kRegEnable) & 1)) vote(d);   // a domain that reads disabled is retried each pass
        IOSleep(pass ? gPeriod * 1000 : 1000);
        summary();
        measure(mhz, ipc);
        mhzRow(fajita_cpufreq_rows[2], "now", ++pass, mhz, ipc);
        if (pass == 1) IOLog("fajita-cpufreq: %s\n%s\n", fajita_cpufreq_rows[0], fajita_cpufreq_rows[2]);
        if (!gPeriod && gMode < 2) break;
        while (!gPeriod) IOSleep(60000);   // keep the mappings (and the row's register reads) alive
    }
}
}

// Called by the boot progress monitor once a second (BootProgress.c); the first call after IOKit is up starts the worker.
extern "C" void sheng_cpufreq_poll(void) {
    static bool absent;
    if (absent || gStarted) {if (gStarted && gDom[0].va) summary(); return;}
    if (!IOService::getPlatform()) return;
    unsigned mode = 0, cap = 0;
    if (!PE_parse_boot_argn("fajita-cpufreq", &mode, sizeof mode) || !mode) {absent = true; return;}
    PE_parse_boot_argn("fajita-cpufreq-cap", &cap, sizeof cap);
    PE_parse_boot_argn("fajita-cpufreq-turbo", &gTurbo, sizeof gTurbo);
    PE_parse_boot_argn("fajita-cpufreq-pmu", &gPmu, sizeof gPmu);
    PE_parse_boot_argn("fajita-cpufreq-period", &gPeriod, sizeof gPeriod);
    gCapKHz = cap && cap < 100000 ? cap * 1000 : cap;
    gMode = mode;
    gStarted = true;
    native_cpufreq_stage = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
