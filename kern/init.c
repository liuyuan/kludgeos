/* See COPYRIGHT for copyright information. */

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/monitor.h>
#include <kern/console.h>
#include <kern/pmap.h>
#include <kern/kclock.h>
#include <kern/env.h>
#include <kern/trap.h>
#include <kern/sched.h>
#include <kern/picirq.h>
#include <kern/time.h>
#include <dev/pci.h>


void
i386_init(void)
{
	int pci_enabled = 0;
	extern char edata[], end[];
	int i = 0;
	// Before doing anything else, complete the ELF loading process.
	// Clear the uninitialized global data (BSS) section of our program.
	// This ensures that all static/global variables start out zero.
	memset(edata, 0, end - edata);

	// Initialize the console.
	// Can't call cprintf until after we do this!
	cons_init();

	// Lab 2 memory management initialization functions
	i386_detect_memory();
	i386_vm_init();

	// Lab 3 user environment initialization functions
	env_init();
	idt_init();

	// Lab 4 multitasking initialization functions
	pic_init();
	kclock_init();
	time_init();
	pci_enabled = pci_init();

	// Should always have an idle process as first one.
	ENV_CREATE(user_idle);

	// Start fs.
	ENV_CREATE(fs_fs);

	// Start the network server.
	if (pci_enabled)
		ENV_CREATE(net_ns);

	// Start init
#if defined(TEST)
	// Don't touch -- used by grading script!
	ENV_CREATE2(TEST, TESTSIZE);
#else
	// Touch all you want.;
	ENV_CREATE(user_icode);
	//ENV_CREATE(user_httpd);
#endif // TEST*

	// Should not be necessary - drain keyboard because interrupt has given up.
	kbd_intr();

	// Schedule and run the first user environment!
	sched_yield();


}


/*
 * Variable panicstr contains argument to first call to panic; used as flag
 * to indicate that the kernel has already called panic.
 */
static const char *panicstr;

/*
 * Panic is called on unresolvable fatal errors.
 * It prints "panic: mesg", and then enters the kernel monitor.
 */
void
_panic(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	if (panicstr)
		goto dead;
	panicstr = fmt;

	va_start(ap, fmt);
	cprintf("kernel panic at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);

dead:
	/* break into the kernel monitor */
	while (1)
		monitor(NULL);
}

/* like panic, but don't */
void
_warn(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	va_start(ap, fmt);
	cprintf("kernel warning at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);
}
