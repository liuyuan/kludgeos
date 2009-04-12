/*
 * The local APIC manages internal (non-I/O) interrupts
 * Written by Liu Yuan
 * Ref: xv6
 * MORE INFO:
 * 	 See Chapter 8 & Appendix C of Intel processor manual volume 3.
 * Mar 28 2009
 * Modified Apr 7, 2009
 */

#include <inc/types.h>
#include <inc/x86.h>
#include <inc/trap.h>
#include <inc/memlayout.h>
#include <inc/error.h>

#include <kern/mp.h>
#include <kern/picirq.h>
#include <kern/pmap.h>

#define RELOC(x) ((x) + KERNBASE)

// Local APIC registers, divided by 4 for use as uint32_t[] indices.
#define ID      (0x0020/4)   // ID
#	define ID_SHIFT	24
#define VER     (0x0030/4)   // Version
#define TPR     (0x0080/4)   // Task Priority
#define EOI     (0x00B0/4)   // EOI
#define SVR     (0x00F0/4)   // Spurious Interrupt Vector
#	define ENABLE 	0x00000100   // Unit Enable
#define ESR     (0x0280/4)   // Error Status
#define ICRLO   (0x0300/4)   // Interrupt Command
/* Below Vector Delivery mode P. 9-18 VOL 3 */
#	define DLMODE_MASK	0x00000700
#	define DLMODE_FIXED	0x00000000
#	define DLMODE_LOW	0x00000100
#	define DLMODE_SMI	0x00000200
#	define DLMODE_RR	0x00000300
#	define DLMODE_NMI	0x00000400
#	define DLMODE_INIT 	0x00000500   // INIT/RESET
#	define DLMODE_STARTUP	0x00000600   // Startup IPI
#	define DLMODE_EXTINT	0x00000700   // Only one ExtINT allowed
#	define DELIVS	0x00001000   // Delivery status
#	define ASSERT 	0x00004000   // Assert interrupt (vs deassert)
#	define DEASSERT	0x00000000
#	define LEVEL 	0x00008000   // Level triggered
#	define DLSTAT_BUSY	0x00001000
#	define BCAST  	0x00080000   // Send to all APICs, including self.
#	define DEST_MASK	0x000c0000
#	define DEST_SELF	0x00040000
#	define DEST_ALLINCL	0x00080000
#	define DEST_ALLEXCL	0x000c0000

#define ICRHI   (0x0310/4)   // Interrupt Command [63:32]
#define TIMER   (0x0320/4)   // Local Vector Table 0 (TIMER)
#	define X1   	0x0000000B   // divide counts by 1
#	define PERIODIC	0x00020000   // Periodic
#define PCINT   (0x0340/4)   // Performance Counter LVT
#define LINT0   (0x0350/4)   // Local Vector Table 1 (LINT0)
#define LINT1   (0x0360/4)   // Local Vector Table 2 (LINT1)
#define ERROR   (0x0370/4)   // Local Vector Table 3 (ERROR)
#	define MASKED 	0x00010000   // Interrupt masked
#define TICR    (0x0380/4)   // Timer Initial Count
#define TCCR    (0x0390/4)   // Timer Current Count
#define TDCR    (0x03E0/4)   // Timer Divide Configuration

volatile uint32_t *lapic;  // Initialized in mp.c

static uint32_t
lapic_read(uint32_t index)
{
	return lapic[index];
}

static void
lapic_write(uint32_t index, uint32_t value)
{
	lapic[index] = value;
	lapic[ID];  // wait for write to finish, by reading
}

void
lapic_init(int c)
{
	uint32_t edx;

	
	if(!lapic) 
		return;
	/* Check if APIC is supported */
	cpuid(0x01, 0, 0, 0, &edx);
	if (!(edx & 0x200))
		return;

	if (c == mp_bcpu()){
			pte_t *pte;
			pte = pgdir_walk(boot_pgdir, (void *)lapic, 1);
			*pte = (physaddr_t)lapic | PTE_W | PTE_P | PTE_PCD; /* Must be Strong Uncacheable */
		}
	// Enable local APIC; set spurious interrupt vector.
	lapic_write(SVR, ENABLE | (IRQ_OFFSET+IRQ_SPURIOUS));

	// The timer repeatedly counts down at bus frequency
	// from lapic[TICR] and then issues an interrupt.  
	lapic_write(TDCR, X1);
	lapic_write(TIMER, PERIODIC | (IRQ_OFFSET + IRQ_TIMER));
	lapic_write(TICR, 10000000); 

	/* Linux uses if () way -_-, but I'd like IOAPIC to distribute ExtINT
	 */
	if (0) {//(c == mp_bcpu()) {
		lapic_write(LINT0, DLMODE_EXTINT);
		lapic_write(LINT1, DLMODE_NMI);
	} else {
		lapic_write(LINT0, MASKED);
		lapic_write(LINT1, MASKED);
	}

	// Disable performance counter overflow interrupts
	// on machines that provide that interrupt entry.
	if(((lapic[VER]>>16) & 0xFF) >= 4)
		lapic_write(PCINT, MASKED);

	// Map error interrupt to IRQ_ERROR.
	lapic_write(ERROR, IRQ_OFFSET+IRQ_ERROR);

	// Clear error status register (requires back-to-back writes).
	lapic_write(ESR, 0);
	lapic_write(ESR, 0);

	// Ack any outstanding interrupts.
	lapic_write(EOI, 0);

	// Send an Init Level De-Assert to synchronise arbitration ID's.
	lapic_write(ICRHI, 0);
	lapic_write(ICRLO, BCAST | DLMODE_INIT | LEVEL);
	while(lapic[ICRLO] & DELIVS)
		;

	// Enable interrupts on the APIC (but not on the processor).
	lapic_write(TPR, 0);	/* 0: Handle all interrupts
				 * 15: Interrupts inhibited
				 * P. Vol.3 9-39
				 */
	//cprintf("CPU %x: lapic_int() success\n", c);
}

static int
lapic_icr_wait()
{
    uint32_t i = 100000;
    while ((lapic_read(ICRLO) & DLSTAT_BUSY) != 0) {
	nop_pause();
	i--;
	if (i == 0) {
	    cprintf("apic_icr_wait: wedged?\n");
	    return -E_INVAL;
	}
    }
    return 0;
}

/* Broadcast IPI to CPUs
 * Success: 0
 * Failure: < 0
 */
int
lapic_broadcast(int self, uint32_t ino)
{
    uint32_t flag = self ? DEST_ALLINCL : DEST_ALLEXCL;
    flag |= ino == T_NMI ? DLMODE_NMI : 0;
    lapic_write(ICRLO, flag | DEASSERT | DLMODE_FIXED | ino);
    return lapic_icr_wait();
}

/* Send IPI to dedicated CPU 
 * Success: 0
 * Failure: < 0
 */
int
lapic_ipi(uint32_t cp_id, uint32_t ino)
{
    lapic_write(ICRHI, cp_id << ID_SHIFT);
    lapic_write(ICRLO, DLMODE_FIXED | DEASSERT | ino);
    return lapic_icr_wait();
}

int
cpu(void)
{
	// Cannot call cpu when interrupts are enabled:
	// result not guaranteed to last long enough to be used!
	// Would prefer to panic but even printing is chancy here:
	// everything, including cprintf, calls cpu, at least indirectly
	// through acquire and release.
	if(read_eflags()&FL_IF){
		static int n;
		if(n++ == 0)
			cprintf("cpu called from %x with interrupts enabled\n",
				((uint32_t*)read_ebp())[1]);
	}

	if(lapic)
		return lapic[ID]>>ID_SHIFT;
	return 0;
}

// Acknowledge interrupt.
void
lapic_eoi(void)
{
	if(lapic)
		lapic_write(EOI, 0);
}

// Spin for a given number of microseconds.
// On real hardware would want to tune this dynamically.
static void
microdelay(int us)
{
	volatile int j = 0;
  
	while(us-- > 0)
		for(j=0; j<10000; j++);
}


#define IO_RTC  0x70

// Start additional processor running bootstrap code at addr.
// See Appendix B of MultiProcessor Specification.
void
lapic_startap(uint8_t apicid, uint32_t addr)
{
	int i;
	uint16_t *wrv;
  
	// "The BSP must initialize CMOS shutdown code to 0AH
	// and the warm reset vector (DWORD based at 40:67) to point at
	// the AP startup code prior to the [universal startup algorithm]."
	outb(IO_RTC, 0xF);  // offset 0xF is shutdown code
	outb(IO_RTC+1, 0x0A);
	wrv = (uint16_t *)RELOC(0x40<<4 | 0x67);  // Warm reset vector
	wrv[0] = 0;
	wrv[1] = addr >> 4;

	// "Universal startup algorithm."
	// Send INIT (level-triggered) interrupt to reset other CPU.
	lapic_write(ICRHI, apicid<<ID_SHIFT);
	lapic_write(ICRLO, DLMODE_INIT | LEVEL | ASSERT);
	microdelay(200);
	lapic_write(ICRLO, DLMODE_INIT | LEVEL);
	microdelay(100);	// should be 10ms, but too slow in Bochs!
	// Send startup IPI (twice!) to enter bootstrap code.
	// Regular hardware is supposed to only accept a STARTUP
	// when it is in the halted state due to an INIT.  So the second
	// should be ignored, but it is part of the official Intel algorithm.
	// Bochs complains about the second one.  Too bad for Bochs.
	for(i = 0; i < 2; i++){
		lapic_write(ICRHI, apicid<<24);
		lapic_write(ICRLO, DLMODE_STARTUP | (addr>>12));
		microdelay(200);
	}
}
