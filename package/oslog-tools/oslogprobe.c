// SPDX-License-Identifier: MIT
// Minimal owner-built launchd/AMFI/console probe for the dipper live-log bring-up.
#include <fcntl.h>
#include <unistd.h>

int
main(void)
{
	int console = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (console >= 0) {
		static const char message[] = "OSLOG_PROBE: owner daemon executed\n";
		(void)write(console, message, sizeof(message) - 1);
	}
	for (;;) {
		sleep(3600);
	}
}
