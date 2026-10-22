// SPDX-License-Identifier: MIT
// fajita board glue for BootLog.c's "restart into fastboot" (boot argument sheng-autoreset=<seconds>). sheng writes a PMIC
// reboot-reason cell first; fajita needs none: boot_a and boot_b hold no boot image, so the OnePlus ABL lands in fastboot after any
// reset. The reset is PSCI SYSTEM_RESET over SMC (the conduit of fajita's device tree). Nothing here runs without BootLog's arguments.
#include <kern/thread.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <pexpert/pexpert.h>

extern volatile uint32_t armcpu_wdog[6], armcpu_imem[4];
extern volatile int64_t armcpu_scm[8];
extern volatile uint64_t armcpu_wdog_pets, armcpu_deadline_start, armcpu_deadline_ticks, armcpu_deadline_seen;
extern volatile uint64_t armcpu_wdog_live[14] __attribute__((weak_import));   // cntfrq, stop, first/last pet, STS, CNTVCT, CNTPCT, IPIs sent/taken, timer IRQs
extern void armcpu_wdog_sample(void) __attribute__((weak_import));
// Data volume persistence (NativeRamBlock.cpp): state 1 blank 2 resumed 3 fresh 4 refused 5 degraded; header = checkHeader result.
extern volatile uint32_t native_data_persist_state __attribute__((weak_import));
extern volatile uint32_t native_data_persist_header __attribute__((weak_import));
extern volatile uint64_t native_data_persist_present __attribute__((weak_import));
extern volatile uint64_t native_data_persist_writes __attribute__((weak_import));
extern volatile uint64_t native_data_persist_errors __attribute__((weak_import));
extern volatile uint64_t native_data_writeback_flushes __attribute__((weak_import));
// Display frame copies (LegacyDisplayProbe.cpp), CNTPCT ticks.
extern volatile uint64_t native_legacy_copy_n __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_last __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_max __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_sum __attribute__((weak_import));

enum { PsciVersion = 0x84000000u, PsciFeatures = 0x8400000au, PsciSystemReset = 0x84000009u };

static uint64_t psci(uint64_t fid, uint64_t a1)
{
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
                   "x13", "x14", "x15", "x16", "x17");
  return x0;
}

int boot_reset_probe(char *text, unsigned cap)
{
  const uint64_t version = psci(PsciVersion, 0);
  const int64_t reset = (int64_t)psci(PsciFeatures, PsciSystemReset);
  snprintf(text, cap, "psci v%llx reset=%lld | wdog mapped=%u rst=%x en=%x sts=%x bark=%x bite=%x pets=%llu | imem w0=%x dload=%x reason=%x->%x | scm avail sdi=%lld dl=%lld wr=%lld sdi=%lld tcsr=%llx dl=%lld wr=%lld tcsr=%llx | deadline start=%llx ticks=%llx seen=%llx",
           (unsigned long long)version, (long long)reset, armcpu_wdog[0], armcpu_wdog[1], armcpu_wdog[2], armcpu_wdog[3], armcpu_wdog[4],
           armcpu_wdog[5], (unsigned long long)armcpu_wdog_pets, armcpu_imem[0], armcpu_imem[1], armcpu_imem[2], armcpu_imem[3],
           (long long)armcpu_scm[0], (long long)armcpu_scm[1], (long long)armcpu_scm[2], (long long)armcpu_scm[3],
           (long long)armcpu_scm[4], (long long)armcpu_scm[5], (long long)armcpu_scm[6], (long long)armcpu_scm[7],
           (unsigned long long)armcpu_deadline_start,
           (unsigned long long)armcpu_deadline_ticks, (unsigned long long)armcpu_deadline_seen);
  if (&armcpu_wdog_live && &armcpu_wdog_sample) {
    armcpu_wdog_sample();
    size_t used = strlen(text);
    if (used < cap)
      snprintf(text + used, cap - used, " | live frq=%llu stop=%llx first=%llx last=%llx now=%llx sts=%llx vct=%llx pct=%llx ipi=%llu/%llu tmr=%llu dbg0 cidr1=%llx arch=%llx prsr=%llx pc=%llx",
               (unsigned long long)armcpu_wdog_live[0], (unsigned long long)armcpu_wdog_live[1], (unsigned long long)armcpu_wdog_live[2],
               (unsigned long long)armcpu_wdog_live[3], (unsigned long long)mach_absolute_time(), (unsigned long long)armcpu_wdog_live[4],
               (unsigned long long)armcpu_wdog_live[5], (unsigned long long)armcpu_wdog_live[6], (unsigned long long)armcpu_wdog_live[7],
               (unsigned long long)armcpu_wdog_live[8], (unsigned long long)armcpu_wdog_live[9], (unsigned long long)armcpu_wdog_live[10],
               (unsigned long long)armcpu_wdog_live[11], (unsigned long long)armcpu_wdog_live[12], (unsigned long long)armcpu_wdog_live[13]);
  }
  if (&native_legacy_copy_n && native_legacy_copy_n) {
    size_t used = strlen(text);
    const uint64_t n = native_legacy_copy_n, per_ms = 19200;   // 19.2 MHz counter
    if (used < cap)
      snprintf(text + used, cap - used, " | fbcopy n=%llu last=%llums max=%llums mean=%llums", (unsigned long long)n,
               (unsigned long long)(native_legacy_copy_last / per_ms), (unsigned long long)(native_legacy_copy_max / per_ms),
               (unsigned long long)(native_legacy_copy_sum / n / per_ms));
  }
  if (&native_data_persist_state && &native_data_persist_present && &native_data_persist_writes && &native_data_persist_errors &&
      &native_data_persist_header) {
    size_t used = strlen(text);
    if (used < cap)
      snprintf(text + used, cap - used, " | data state=%u hdr=%u present=%llu writes=%llu errors=%llu flushes=%llu",
               native_data_persist_state, native_data_persist_header, (unsigned long long)native_data_persist_present,
               (unsigned long long)native_data_persist_writes, (unsigned long long)native_data_persist_errors,
               &native_data_writeback_flushes ? (unsigned long long)native_data_writeback_flushes : 0ULL);
  }
  // PSCI 0.2+ always implements SYSTEM_RESET; PSCI_FEATURES only exists from 1.0.
  return version >= 0x2 ? 0 : 1;
}

int boot_reset_mark_bootloader(char *text, unsigned cap)
{
  snprintf(text, cap, "no reboot-reason cell needed on fajita");
  return 0;
}

int boot_reset_watchdog(uint32_t seconds, char *text, unsigned cap)
{
  snprintf(text, cap, "no hypervisor watchdog backstop on fajita (%u s requested)", seconds);
  return 1;
}

int boot_reset_bootloader(char *text, unsigned cap)
{
  snprintf(text, cap, "reset: PSCI SYSTEM_RESET");
  psci(PsciSystemReset, 0);
  return 99;   // SYSTEM_RESET returned
}
