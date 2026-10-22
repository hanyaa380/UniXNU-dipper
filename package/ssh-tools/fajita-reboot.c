// SPDX-License-Identifier: MIT
// fajita-reboot: a tiny owner tool that restarts the OnePlus 6T from a shell on the
// running iPadOS userland. Any reset lands the phone in the OnePlus ABL fastboot
// menu (boot_a / boot_b carry no boot image), which is where bring-up wants it.
//
// It asks the kernel to restart with reboot(2) (RB_AUTOBOOT). On this board the
// restart ultimately reaches PSCI SYSTEM_RESET; see package/BootReset.c, which the
// kernel also uses for the sheng-autoreset timer. If reboot(2) returns (e.g. the
// PMGR stub does not wire PEHaltRestart through on a given build), the boot-arg
// sheng-autoreset=<seconds> is the fallback timed reset.
//
// Built for arm64 iOS by scripts/build-ssh-userland.py and signed into the owner
// trust cache; run as root from the SSH/serial shell.
#include <unistd.h>
#include <stdio.h>
#include <string.h>

// <sys/reboot.h> RB_AUTOBOOT is 0; declare reboot(2) directly to avoid depending on
// private SDK headers in the offline cross-build.
extern int reboot(int howto);

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	// Flush any pending shell output first.
	fflush(NULL);
	sync();
	int rc = reboot(0 /* RB_AUTOBOOT */);
	// reboot(2) does not return on success.
	fprintf(stderr, "fajita-reboot: reboot(2) returned %d; try boot-arg sheng-autoreset=<seconds>\n", rc);
	return 1;
}
