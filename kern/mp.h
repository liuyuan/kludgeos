#ifndef JOS_KERN_MP_H
#define JOS_KERN_MP_H
#ifndef JOS_KERNEL
# error "This is a JOS kernel header; user programs should not #include it"
#endif

#include <inc/mmu.h>
#include <inc/memlayout.h>
// Saved registers for kernel context switches.
// Don't need to save all the %fs etc. segment registers,
// because they are constant across kernel contexts.
// Save all the regular registers so we don't need to care
// which are caller save, but not the return register %eax.
// (Not saving %eax just simplifies the switching code.)
// The layout of context must match code in swtch.S.
struct Context {
	int eip;
	int esp;
	int ebx;
	int ecx;
	int edx;
	int esi;
	int edi;
	int ebp;
};

#define NCPU 8

struct Cpu {
	uint8_t apicid;			// Local APIC ID
	struct Env *curenv;       	// Process currently running.
	//struct Context context;    	// Switch here to enter scheduler
	struct Taskstate ts;		// Used by x86 to find stack for interrupt
	struct Segdesc gdt[GD_NSEG];	// x86 global descriptor table
	volatile uint32_t booted;	// Has the CPU started?
	int ncli;                   	// Depth of pushcli nesting.
	int intena;                 	// Were interrupts enabled before pushcli? 
};

// See MultiProcessor Specification Version 1.[14]

struct Mp {             		// floating pointer
	uint8_t signature[4];           // "_MP_"
	void *physaddr;               	// phys addr of MP config table
	uint8_t length;                 // 1
	uint8_t specrev;                // [14]
	uint8_t checksum;               // all bytes must add up to 0
	uint8_t type;                   // MP system config type
	uint8_t imcrp;
	uint8_t reserved[3];
};

struct Mpconf {         		// configuration table header
	uint8_t signature[4];           // "PCMP"
	uint16_t length;                // total table length
	uint8_t version;                // [14]
	uint8_t checksum;               // all bytes must add up to 0
	uint8_t product[20];            // product id
	uint32_t *oemtable;		// OEM table pointer
	uint16_t oemlength;             // OEM table length
	uint16_t entry;                 // entry count
	uint32_t *lapicaddr;		// address of local APIC
	uint16_t xlength;               // extended table length
	uint8_t xchecksum;              // extended table checksum
	uint8_t reserved;
};

struct Mpproc {         		// processor table entry
	uint8_t type;                   // entry type (0)
	uint8_t apicid;                 // local APIC id
	uint8_t version;                // local APIC verison
	uint8_t flags;                  // CPU flags
#define MPBOOT 0x02           		// This proc is the bootstrap processor.
	uint8_t signature[4];           // CPU signature
	uint32_t feature;		// feature flags from CPUID instruction
	uint8_t reserved[8];
};

struct Mpioapic {       		// I/O APIC table entry
	uint8_t type;                   // entry type (2)
	uint8_t apicno;                 // I/O APIC id
	uint8_t version;                // I/O APIC version
	uint8_t flags;                  // I/O APIC flags
	uint32_t *addr;			// I/O APIC address
};

// Table entry types
#define MPPROC    0x00  // One per processor
#define MPBUS     0x01  // One per bus
#define MPIOAPIC  0x02  // One per I/O APIC
#define MPIOINTR  0x03  // One per bus interrupt source
#define MPLINTR   0x04  // One per system interrupt source

void
mp_init(void);
int
mp_bcpu(void);
void
ap_setupvm(int);

void
lapic_init(int);
int
cpu(void);
void
lapic_startap(uint8_t, uint32_t);

void
ioapic_init(void);
void
ioapic_enable(int, int);

extern struct Cpu cpus[NCPU];
extern volatile struct ioapic *ioapic;
extern volatile uint32_t *lapic;
extern uint8_t ioapic_id;
extern int ismp;
extern int ncpu;
extern struct Pseudodesc gdt_pd; /* located at kern/pmap.c */

#endif	/* !JOS_KERN_MP_H */
