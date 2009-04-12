/* reboot.c -- This file impements the reboot function for kludgeOS
 * Need to be fleshed out to keep kernel consistency later
 * Reference: wiki.osdev.org
 * 
 * Written by Liu Yuan -- liuyuan8@mail.ustc.edu.cn
 * TODO: Need to be rewritten for SMP (Apr 7, 2009)
 */
#include <inc/x86.h>
#include <inc/assert.h>
#include "reboot.h"
void
reboot(void)
{
	//outb(0x92, 0x3); /* This would just do with bochs */
	uint8_t good = 0x02;
	while ((good & 0x02) != 0)
		good = inb(0x64);
	outb(0x64, 0xFE);
	panic("Congratulations!You are the first one seeing this msg:p\n");
}
