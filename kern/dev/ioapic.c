/*
 * A slight modification from xv6 to make it work with JOS
 * Liu Yuan
 * Mar 28 2009
 */

// The I/O APIC manages hardware interrupts for an SMP system.
// http://www.intel.com/design/chipsets/datashts/29056601.pdf
// See also picirq.c.

#include <inc/types.h>
#include <kern/pmap.h>
#include <kern/mp.h>
#include <kern/picirq.h>


enum {trace_irq_enable = 1};

#define IOAPIC  0xFEC00000   // Default physical address of IO APIC

#define REG_ID     0x00  // Register index: ID
#define REG_VER    0x01  // Register index: version
#define REG_TABLE  0x10  // Redirection table base

// The redirection table starts at REG_TABLE and uses
// two registers to configure each interrupt.  
// The first (low) register in a pair contains configuration bits.
// The second (high) register contains a bitmask telling which
// CPUs can serve that interrupt.
#define INT_DISABLED   0x00010000  // Interrupt disabled
#define INT_LEVEL      0x00008000  // Level-triggered (vs edge-)
#define INT_ACTIVELOW  0x00002000  // Active low (vs high)
#define INT_LOGICAL    0x00000800  // Destination is CPU id (vs APIC ID)

volatile struct ioapic *ioapic;

// IO APIC MMIO structure: write reg, then read or write data.
struct ioapic {
	uint32_t reg;
	uint32_t pad[3];
	uint32_t data;
};

static uint32_t
ioapic_read(int reg)
{
	ioapic->reg = reg;
	return ioapic->data;
}

static void
ioapic_write(int reg, uint32_t data)
{
	ioapic->reg = reg;
	ioapic->data = data;
}

void
ioapic_init(void)
{
	int i, id, maxintr;

	if(!ismp)
		return;
	
	pte_t *pte;
	pte = pgdir_walk(boot_pgdir, (void *)IOAPIC, 1);
	*pte = IOAPIC | PTE_W | PTE_P | PTE_PCD; /* Strong Uncacheable */
	
	ioapic = (volatile struct ioapic*)IOAPIC;
	maxintr = (ioapic_read(REG_VER) >> 16) & 0xFF;
	id = ioapic_read(REG_ID) >> 24;
	if(id != ioapic_id)
		cprintf("ioapic_init: id isn't equal to ioapic_id; not a MP\n");

	// Mark all interrupts edge-triggered, active high, disabled,
	// and not routed to any CPUs.
	for(i = 0; i <= maxintr; i++){
		ioapic_write(REG_TABLE+2*i, INT_DISABLED | (IRQ_OFFSET + i));
		ioapic_write(REG_TABLE+2*i+1, 0);
	}
	//cprintf("CPU %x: ioapic_init() success\n", cpu());
}

void
ioapic_enable(int irq, int cpunum)
{
	if(!ismp)
		return;

	if (trace_irq_enable)
		cprintf("CPU %x <-- irq %u enabled (IOAPIC)\n", cpunum, irq);

	ioapic_write(REG_TABLE+2*irq, IRQ_OFFSET + irq);
	ioapic_write(REG_TABLE+2*irq+1, cpunum << 24);
}
