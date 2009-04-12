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

#define DEBUG 0
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

	pic_init();	/* In MP, 8259A delivers external INTR to IOAPIC */
	ioapic_init();	/* Distribute extertal INTR to CPUs */
	ioapic_enable(IRQ_KBD, mp_bcpu());
	
	if (!ismp)
		kclock_init();/* Only used in UP */
	
	time_init();
	
	pci_enabled = pci_init();

	// Should always have an idle process as first one. ENVID 0
	ENV_CREATE(user_idle);

	// Start fs.ENVID 1
	ENV_CREATE(fs_fs);

	// Start the network server. ENVID 2
	assert(pci_enabled);
	ENV_CREATE(net_ns);
	
	// Start http daemon
	ENV_CREATE(user_httpd);
	// Start sh.
	ENV_CREATE(user_icode);

	bootothers();
	assert(booted == 0);
	booted = 1;
	//asm("jmp .");
	// Schedule and run the first user environment!
	sched_yield();


}

 /* Bootother.S enters it */
 void
 ap_init(void)
 {
	 static struct Segdesc gdttmp[] = {
		 // 0x0 - unused (always faults -- for trapping NULL far pointers)
		 SEG_NULL,
		 
		 // 0x8 - kernel code segment
		 [GD_KT >> 3] = SEG(STA_X | STA_R, -KERNBASE, 0xffffffff, 0),
		 
		 // 0x10 - kernel data segment
		 [GD_KD >> 3] = SEG(STA_W, -KERNBASE, 0xffffffff, 0),
		 };

	 static struct Pseudodesc gdt_pdtmp = {
		 sizeof(gdttmp) - 1, (physaddr_t)gdttmp - KERNBASE
	 };
	 static int c = 0;		/* We can't use lapic(so cpu()) before paging :( */
	 /* Set up temporary GDT */
 #ifdef DEBUG
	 asm volatile("movw $(0x0600 + 'T'), (0xb8744)");/* NOTE: Should turn off BGA before doing diagnosis */
 #endif
	 asm volatile(
		 "lgdt	(%0) \n\t"
		 "movw	%%ax, %%ds \n\t"
		 "movw	%%ax, %%es \n\t"
		 "movw	%%ax, %%ss \n\t"
		 "ljmp	%1, $1f \n\t" /* Reloade CS */
		 "1:" 
		 :
		 : "r" ((physaddr_t)&gdt_pdtmp - KERNBASE),
		   "i" (GD_KT), "a" (GD_KD)
		 );  
 #ifdef DEBUG
	 asm volatile("movw $(0x0200 + 'I'), (0xF00b8746)");
 #endif
	 /* At this point, we can use stack */
	 ap_setupvm(++c);
	 lapic_init(c);
	 idt_init();
	 xchg(&cpus[cpu()].booted, 1);
	 //asm volatile("jmp .");
	 while (booted == 0)	/* Wait AP all up */;
	 cprintf("AP %x: Initialized\n", cpu());
	 sched_yield();
 }

/* Relocate bootother.S and make it happen */
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
		 if(c == cpus+cpu())
			 continue;

		 /* Fill in %esp, %eip and start code on cpu. Be careful of the _address_.
		  * Use low memory to allocate stacks for other CPUs,
		  * So it would cause a hard limit of  640/32 = 20 APs we can support in theory.
		  * If you'er greedy enough, try allocating pages dynamically 8-)
		  *
		  * We just use stack in link address. Be sure you know the difference
		  * between Load address and Link address :)
		  */
		 *(void**)(KADDR(code)-4) = KADDR(IOPHYSMEM - KSTKSIZE*(c->apicid - 1));
		 *(void**)(KADDR(code)-8) = (void *)PADDR(ap_init);
		 lapic_startap(c->apicid, code);

		 /* Wait for cpu to get through bootstrap. */
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
