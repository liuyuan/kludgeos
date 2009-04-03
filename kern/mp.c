/*
 * This file implement MP initilization ,namely, to get a
 * MP configuration header for kludgeOS
 *
 * Referrence: mp.c of xv6 (just a slight modification)
 * 
 * Written by liu yuan -- liuyuan8@mail.ustc.edu.cn
 * Mar 28 2009
 */
#include <inc/memlayout.h>
#include <inc/types.h>
#include <inc/string.h>
#include <inc/x86.h>
#include <kern/mp.h>
#include <kern/pmap.h>

#define RELOC(x) (x + KERNBASE)

struct Cpu cpus[NCPU];
static struct Cpu *bcpu;
int ismp;
int ncpu;
uint8_t ioapic_id;

int
mp_bcpu(void)
{
	return bcpu - cpus;
}

static uint8_t
sum(uint8_t *addr, int len)
{
	int i, sum;
  
	sum = 0;
	for(i=0; i<len; i++)
		sum += addr[i];
	return sum;
}

// Look for an MP structure in the len bytes at addr.
static struct Mp*
mp_search1(uint8_t *addr, int len)
{
	uint8_t *e, *p;

	e = RELOC(addr) + len;
	for(p = RELOC(addr); p < e; p += sizeof(struct Mp))
		if(memcmp(p, "_MP_", 4) == 0 && sum(p, sizeof(struct Mp)) == 0)
			return (struct Mp*)p;
	return 0;
}

// Search for the MP Floating Pointer Structure, which according to the
// spec is in one of the following three locations:
// 1) in the first KB of the EBDA;
// 2) in the last KB of system base memory;
// 3) in the BIOS ROM between 0xF0000 and 0xFFFFF.
static struct Mp*
mp_search(void)
{
	uint8_t *bda;
	uint32_t p;
	struct Mp *mp;
	bda = (uint8_t*)RELOC(0x400);
	if((p = ((bda[0x0F]<<8)|bda[0x0E]) << 4)){
		if((mp = mp_search1((uint8_t*)p, 1024)))
			return mp;
	} else {
		p = ((bda[0x14]<<8)|bda[0x13])*1024;
		if((mp = mp_search1((uint8_t*)p-1024, 1024)))
			return mp;
	}
	return mp_search1((uint8_t*)0xF0000, 0x10000);
}

#define DEBUG

#ifdef DEBUG
void 
dump_mp(struct Mp *mp)
{
	cprintf("MP header: 	\
\n\t signature:%.*s		\
\n\t physaddr:%x		\
\n\t length:%x			\
\n\t specrev:%x			\
\n\t checksum:%x		\
\n\t type:%x			\
\n\t imcrp:%x			\
\n", 
		4, mp->signature, mp->physaddr, mp->length, mp->specrev, mp->checksum,
		mp->type, mp->imcrp);
}
#endif
// Search for an MP configuration table.  For now,
// don't accept the default configurations (physaddr == 0).
// Check for correct signature, calculate the checksum and,
// if correct, check the version.
// To do: check extended table checksum.
static struct Mpconf*
mp_config(struct Mp **pmp)
{
	struct Mpconf *conf;
	struct Mp *mp;

	if((mp = mp_search()) == 0 || mp->physaddr == 0)
		return 0;

#ifdef DEBUG
	dump_mp(mp);
#endif
	conf = (struct Mpconf*)RELOC(mp->physaddr);
	if(memcmp(conf, "PCMP", 4) != 0)
		return 0;
	if(conf->version != 1 && conf->version != 4)
		return 0;
	if(sum((uint8_t*)conf, conf->length) != 0)
		return 0;
	*pmp = mp;
	return conf;
}

#ifdef DEBUG
void 
dump_mp_config(struct Mpconf *conf)
{
	cprintf("MP_CONFIG header: 	\
\n\t signature:%.*s		\
\n\t table length:%x		\
\n\t version:%x			\
\n\t checksum:%x		\
\n\t product:%s			\
\n\t OEM msg:%s			\
\n\t entry count:%x		\
\n\t lapicaddr:%x		\
\n\t extended table lengh:%x	\
\n\t extended checksum:%x	\
\n", 
		4, conf->signature, conf->length, conf->version, conf->checksum,
		conf->product, conf->oemtable, conf->entry, conf->lapicaddr, conf->xlength,
		conf->xchecksum);
}
#endif

void
mp_init(void)
{
	uint8_t *p, *e;
	struct Mp *mp;
	struct Mpconf *conf;
	struct Mpproc *proc;
	struct Mpioapic *ioapic;
	pte_t *pte;

	bcpu = &cpus[ncpu];
	if((conf = mp_config(&mp)) == 0)
		return;

#ifdef DEBUG
	dump_mp_config(conf);
#endif
	ismp = 1;
	/* Get the lapic address */
	lapic = (uint32_t *)(conf->lapicaddr);
	for(p=(uint8_t*)(conf+1), e=(uint8_t*)conf+conf->length; p<e; ){
		switch(*p){
		case MPPROC:
			proc = (struct Mpproc*)p;
			cpus[ncpu].apicid = proc->apicid;
			if(proc->flags & MPBOOT)
				bcpu = &cpus[ncpu];
			ncpu++;
			p += sizeof(struct Mpproc);
			continue;
		case MPIOAPIC:
			ioapic = (struct Mpioapic*)p;
			ioapic_id = ioapic->apicno;
			p += sizeof(struct Mpioapic);
			continue;
		case MPBUS:
		case MPIOINTR:
		case MPLINTR:
			p += 8;
			continue;
		default:
			cprintf("mp_init: unknown config type %x\n", *p);
			panic("mp_init");
		}
	}
	cprintf("mp_init() success! IOAPIC:%x\n", ioapic_id);
	if(mp->imcrp){
		// Bochs doesn't support IMCR, so this doesn't run on Bochs.
		// But it would on real hardware.
		outb(0x22, 0x70);   // Select IMCR
		outb(0x23, inb(0x23) | 1);  // Mask external interrupts.
	}
}

void
ap_setupvm(int c)
{
	uint32_t cr0;
	pte_t *pte;
	int i;
	struct Pseudodesc ap_gdt_pd;
	
	for (i = 0; i < GD_NSEG - 1; i++)
		cpus[c].gdt[i] = gdt[i];

	ap_gdt_pd.pd_lim = sizeof(cpus[c].gdt) - 1;
	ap_gdt_pd.pd_base = (uintptr_t)cpus[c].gdt;
	
	boot_pgdir[0] = boot_pgdir[PDX(KERNBASE)]; /* Temporarily */
	lcr3(boot_cr3);

	/* Turn on Paging */
	cr0 = rcr0();
	cr0 |= CR0_PE|CR0_PG|CR0_AM|CR0_WP|CR0_NE|CR0_TS|CR0_EM|CR0_MP;
	cr0 &= ~(CR0_TS|CR0_EM);
	lcr0(cr0);
	
	/* Reload segments */
	asm volatile("lgdt (%0)" :: "r" (&ap_gdt_pd));
	asm volatile("movw %%ax,%%gs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%es" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" :: "a" (GD_KD));
	asm volatile("ljmp %0,$1f\n 1:\n" :: "i" (GD_KT));  // reload cs
	asm volatile("lldt %%ax" :: "a" (0));
	
	boot_pgdir[0] = 0;	/* Kill boot_pgdir[0] */

	lcr3(boot_cr3);
	
	/* Set up kernel stack used by ts.esp0 */
	pte = pgdir_walk(boot_pgdir, (void *)(KSTACKTOP - KSTKSIZE * (cpu()*2 + 1)), 1);
	*pte = ROUNDDOWN(read_esp() & 0xffffff, PGSIZE) | PTE_P | PTE_W;
	/* Now VM setup, starts the fun :) */
	//cprintf("CPU %d ESP %x, MAP %x\n", cpu(), read_esp(), *pte);
	//asm volatile("jmp .");
}
