#include <inc/lib.h>

void
umain(void)
{
	int r;
	//cprintf("icode: spawn /init\n");
	if ((r = spawnl("/init", "init", "initarg1", "initarg2", (char*)0)) < 0)
		panic("icode: spawn /init: %e", r);

	//cprintf("icode: exiting\n");
}
