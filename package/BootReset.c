// SPDX-License-Identifier: MIT
// fajita board glue for BootLog.c's "restart into fastboot" (boot argument sheng-autoreset=<seconds>). sheng writes a PMIC
// reboot-reason cell first; fajita needs none: boot_a and boot_b hold no boot image, so the OnePlus ABL lands in fastboot after any
// reset. The reset is PSCI SYSTEM_RESET over SMC (the conduit of fajita's device tree). Nothing here runs without BootLog's arguments.
#include <stdint.h>
#include <kern/misc_protos.h>   // snprintf: the kernel has no stdio.h

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
  snprintf(text, cap, "psci v%llx reset=%lld (fajita: any reset lands in fastboot)", (unsigned long long)version, (long long)reset);
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
