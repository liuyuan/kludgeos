/* Written by Liu Yuan */
#include <inc/lib.h>

void
sleep(int sec) {
	unsigned end = sys_time_msec() + sec * 1000;
	while (sys_time_msec() < end)
		sys_yield();
}

void
umain(int argc, char **argv)
{
	int i;

	cprintf("System reboot");
	for (i = 3; i >= 0; i--) {
		cprintf(". ", i);
		sleep(1);
	}
	cprintf("rebooting");
	sys_reboot();
}
