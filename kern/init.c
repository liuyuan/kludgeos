/* See COPYRIGHT for copyright information. */
/*
 * MP support implemented by Liu yuan
 * Apr 3, 2009
 */
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
#include <kern/mp.h>

#define DEBUG
static void
bootothers(void);

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

	i386_detect_memory();
	i386_vm_init();
	mp_init();
	lapic_init(mp_bcpu());
	
	env_init();
	idt_init();

	pic_init();
	ioapic_init();
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
	
	bootothers();
	// Schedule and run the first user environment!
	sched_yield();


}

/* Temporary GDT before paging being enabled
 * Have to be Global to get seated at _data_ section
 */
struct Segdesc gdttmp[] =
{	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,
	
	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, -KERNBASE, 0xffffffff, 0),
	
	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, -KERNBASE, 0xffffffff, 0),
};

struct Pseudodesc gdt_pdtmp = {
	sizeof(gdttmp) - 1, (physaddr_t)gdttmp - KERNBASE
};
	
/* Bootother.S enters it */
void
ap_init(void)
{
	static int c = 0;		/* We can't use lapic(so cpu()) before paging :( */
	/* Set up temporary GDT
	 * This silly line-by-line embeded asm, huh? Anyway, it is much more readable
	 * at least for me
	 * Liu Yuan Apr 3, 2009
	 */
#ifdef DEBUG
	asm volatile("movw $(0x0600 + 'T'), (0xb8744)");/* NOTE: Should turn off BGA before doing diagnosis */
#endif
	asm volatile("lgdt (%0)" :: "r" ((physaddr_t)&gdt_pdtmp - KERNBASE));	
	asm volatile("movw %%ax,%%ds" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%es" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" :: "a" (GD_KD));
	asm volatile("ljmp %0,$1f\n 1:\n" :: "i" (GD_KT));  // reload cs
#ifdef DEBUG
	asm volatile("movw $(0x0200 + 'I'), (0xF00b8746)");
#endif
	/* At this point, we can use stack */
	ap_setupvm(++c);
	lapic_init(c);
	idt_init();
	xchg(&cpus[cpu()].booted, 1);
	cprintf("AP %x: starded and going to be halted~~~~~~~~~~~\n", cpu());
	asm volatile("jmp .");
	//sched_yield();
}

static void
bootothers(void)
{
	extern uint8_t _binary_obj_kern_bootother_start[], _binary_obj_kern_bootother_size[], _start;
	physaddr_t code;
	struct Cpu *c;
	char *stack;
	
	/* Write bootstrap code to unused memory at 0x7000.
	 * Because CPU starts in real mode :)
	 */
	code = 0x7000;
	memmove(KADDR(code), _binary_obj_kern_bootother_start, (uint32_t)_binary_obj_kern_bootother_size);
	for(c = cpus; c < cpus+ncpu; c++){
		if(c == cpus+cpu())  // We've started already.
			continue;

		/* Fill in %esp, %eip and start code on cpu. Be careful of the _address_.
		 * Use low memory to allocate stacks for other CPUs
		 *
		 * We just use stack in link address. Be sure you know the difference
		 * between Load address and Link address :)
		 */
		*(void**)(KADDR(code)-4) = KADDR(IOPHYSMEM - PGSIZE*(c->apicid - 1)); /* Not sure if apicid increases
										       * in order ?
										       */
		*(void**)(KADDR(code)-8) = (void *)PADDR(ap_init);
		cprintf("CPU %x: ready to bootstrap AP %x \n", cpu(), c->apicid);
		lapic_startap(c->apicid, code);

		// Wait for cpu to get through bootstrap.
		while(c->booted == 0)
			;
	}
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
