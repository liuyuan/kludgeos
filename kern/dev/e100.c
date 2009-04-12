/*
 * e100.c -- This file implements the eepro100 driver for KludgeOS
 *
 * NOTE:This simple driver is subject to *poor* design (thus never expect
 * neither resonable performance nor full-fledged functionality because
 * I could just make bare use of the hardware interface provided by eepro100.
 *
 * Written by Liu Yuan -- liuyuan8@email.ustc.edu.cn
 */
#include <inc/x86.h>
#include <inc/string.h>
#include <kern/pmap.h>
#include <kern/picirq.h>
#include <kern/mp.h>

#include "e100.h"

/*****Control/Status Registers******
 *  +--------------+--------------+ 0h <-
 *  | SCB COMMAND  | SCB STATUS   |      \
 *  +--------------+--------------+ 4h    System Control Block
 *  |     SCB  General Pointer    |      /
 *  +--------------+--------------+ 8h <-
 *  |       	  PORT            |
 *  +--------------+--------------+ ch
 *  | | | | | | | | | | | | | | | |
 **********************************
 *  The CSRs makes up the _CSR SPACE_(internal to the device), Totaly 64 Bytes
 *  SCB is a major place where your CPU communicate with the Lan controller.
 *
 *  Address scheme: iobase + offset or membase + offset.
 *
 *  SCB STATUS: The _device_ places status of CU&RU for CPU
 *  SCB COMMAND: This is reg to write commands for CU&RU
 *  SCB GP: Points to various data in main memory
 *  PORT:Allow the CPU to reset the device or dump information
 *
 *  MORE INFO SEE P.40 at intel OpenSDM.
 */
enum scb_offsets {
	scb_status = 0,
	scb_cmd = 2,
	scb_pointer = 4,
	scb_port = 8,
	scb_eeprom = 14,	/* TODO:get MAC with the help of this later */
};

/* These bits indicates the cause of the current interrupts
 * MORE INFO SEE P.43
 */
enum scb_status {
	scb_status_rus_suspended = 1 << 2,
	scb_status_rnr = 1 << 12, /* The RU is not ready */
	scb_status_cna = 1 << 13, /* CU leaves Active state */
	scb_status_fr = 1 << 14,  /* RU finished receiving a frame */
	scb_status_cx = 1 << 15,  /* CU finished executing a command*/
};
/* CPU writes to command byte to issue an action command
 * MORE INFO SEE P.45
 */
enum scb_command {
	scb_ru_start	= 1,
	scb_ru_resume	= 2,
	scb_ruc_ldbase	= 6,
	scb_cu_start	= 1 << 4,  /* Star CU execution */
	scb_cuc_ldbase	= 6 << 4, /* Load the base address for CU */
};

enum port_command {
	software_reset 	= 0,
	selftest	= 1,
	selective_reset = 2,
};

struct E100 e100;
/*******************************************************************
 *                   Control Block LIST	                  	   *
 *******************************************************************/

/**************TCB*****************
 *  +--------------+--------------+
 *  |    COMMAND   |    STATUS    |
 *  +--------------+--------------+
 *  |            LINK             |
 *  +--------------+--------------+
 *  |       TBD ARRAY ADDR        |
 *  +--------------+--------------+
 *  |TBD COUNT|THRS|TCB BYTE COUNT|
 *  +--------------+--------------+
 *  |            DATA             |
 *  +--------------+--------------+
 **********************************
 *  MORE INFO SEE P.113
 */
struct Tcb {
	volatile uint16_t status;
	uint16_t command; 	/* Action commands*/
	uint32_t link;		/* Links to the next TCB in physical addr */
	uint32_t tbdaddr; 	/* Not used in simplified mode */
	uint16_t size;
	uint8_t threshold;	/* Spesify how much data in adapter's FIFO*/
	uint8_t tbdcount;	/* Not used */
	uint8_t data[E100_MAX_PKT_SIZE]; /* Stores Packet to be sent */
};

/* Action comands in TCB */
enum tcb_command {
	tcb_tx	= 4,		/* Transmit */
	tcb_s	= 1 << 14,	/* Suspended */
};

/* Helper macros to manipulate our CBL */

#define CBL_IS_EMPTY() (e100.cbl_count == 0)
#define CBL_IS_FULL() (e100.cbl_count == CBL_SIZE)
/* CBL_NEXT() returns where the data to be sent is loaded in CBL */
#define CBL_NEXT() ((e100.cbl_head + e100.cbl_count) % CBL_SIZE)
/* CBL_HEAD returns where in CBL the CU choose to send when starting */
#define CBL_HEAD() (e100.cbl_head)

/* Make up a DMA ring for CBL.Transmitting with tcb_s, after every
 * sending completes, the adapter will interrupt us so we can
 * do some accounting and then restart it at CBL_HEAD().
 *
 * Simplified mode.
 *
 * +-----------------------------+
 * |                             |
 * |  +---+  +---+  +---+  +---+ |
 * +->|TCB|->|TCB|->|TCB|->|TCB|-+
 *    +---+  +---+  +---+  +---+
 */
static void
cbl_init()
{
	int i,r;
	struct Page *p;

	e100.cbl_count = 0;
	e100.cbl_head = 0;
	for (i = 0; i < CBL_SIZE; i++) {
		if ((r = page_alloc(&p)) < 0)
			panic("e100_init");

		e100.cbl[i] = (struct Tcb *)page2kva(p);
		e100.cbl[i]->status = 0;
		e100.cbl[i]->command = tcb_tx | tcb_s;
		e100.cbl[i]->tbdaddr = 0xffffffff;
		e100.cbl[i]->size = 0;
		e100.cbl[i]->threshold = 0xE0; /* Maximum bytes to be present in
						* the adapter's FIFO*/
		e100.cbl[i]->tbdcount = 0;
	}

	/* Set up the links in a _circular_ fasion. The adapter without
	 * MMU just deals with _physical_ address
	 */
	for (i = 0; i < CBL_SIZE; i++)
		e100.cbl[i]->link = PADDR(e100.cbl[(i+1) % CBL_SIZE]);

	/* Set up the cu base register. The adapter addresses our cbl
	 * this way: BASE(CU) + OFFSET(SCB General Pointer)
	 */
	outl(e100.iobase + scb_pointer, 0); /* BASE = 0 */
	outw(e100.iobase + scb_cmd, scb_cuc_ldbase);
}

static void
e100_startcu(void)
{
	if (CBL_IS_EMPTY())
		return;	/* Silently return if no packets to send */

	outl(e100.iobase + scb_pointer, PADDR(e100.cbl[CBL_HEAD()]));
	outw(e100.iobase + scb_cmd, scb_cu_start);
}

/* This function implement main part of sending a packet, adding packet into
 * transimit block list.
 *
 * p: points to the packet to be sent
 * sz: size of the packet
 * Returns -1 when it drops the packet
 * Returns 0 on success
 */
int
e100_add_tcb(char *p, int sz)
{
	int is;
	int n;
	if (CBL_IS_FULL())
		return -1;	/* Simply drop the packet */

	is = CBL_IS_EMPTY();	/* If the cbl is empty, the adapter should be in idle/suspended state */
	n = CBL_NEXT();
	e100.cbl[n]->status = 0;
	memmove(e100.cbl[n]->data, p, sz);
	e100.cbl[n]->size = sz;
	e100.cbl_count++;

	if (is)
		e100_startcu();
	return 0;
}

/**********************************************************************
 *                      Recieve Frame Area			      *
 **********************************************************************/
/* Mark the tail of circular list with suspended bit set and others retains
 * intact, So when the adapter hit the tail, it will get suspended (no room
 * for incoming packets) and then we may make room for the adapter by calling
 * rfa_makeroom() thus place a new tail for RFA.
 *
 * Simplified mode.
 *
 * +----------------------+
 * |                      |
 * |  +---+  +---+  +---+ |
 * +->|RFD|..|RFD|->|RFD|-+
 *    +---+  +---+  +---+
 *
 * MORE INFO SEE P107
 */
struct Rfd {
	volatile uint16_t status;
	uint16_t command;
	uint32_t link;
	uint32_t reserved;
	volatile uint16_t status2;
	uint16_t size;
	uint8_t data[E100_MAX_PKT_SIZE];
};
/* P.108 */
enum rfd_command {
	rfd_s	= 1 << 14,	/* Suspended */
};

enum rfd_status {
	rfd_status_c	= 1 << 15, /* This bit should be checked */
	rfd_status2_eof = 1 << 15, /* This too should be checked */
	rfd_status2_count_mask = 0x3fff,
};

/* Helper macros to manipulate our RFA */
#define RFA_NEXT() (e100.rfa_head)
#define RFA_INC() (e100.rfa_head = (e100.rfa_head + 1) % RFA_SIZE)
#define RFA_TAIL() (e100.rfa_tail)
#define RFA_INIT(index, tail) do {				\
		e100.rfa[index]->status = 0;			\
		e100.rfa[index]->command = tail ? rfd_s : 0;	\
		e100.rfa[index]->status2 = 0;			\
		e100.rfa[index]->size = E100_MAX_PKT_SIZE;	\
	} while (0)



static void
rfa_init()
{
	int i,r;
	struct Page *p;

	e100.rfa_head = 0;
	e100.rfa_tail = RFA_SIZE - 1;
	e100.rfa_noroom = 0;
	for (i = 0; i < RFA_SIZE; i++) {
		if ((r = page_alloc(&p)) < 0)
			panic("e100_init");

		e100.rfa[i] = (struct Rfd *)page2kva(p);
		if (i == RFA_SIZE - 1)
			RFA_INIT(i, 1); /* Mark the tail as the one with 's' bit set */
		else
			RFA_INIT(i, 0);
	}
	for (i = 0; i < RFA_SIZE; i++)
		e100.rfa[i]->link = PADDR(e100.rfa[(i+1) % RFA_SIZE]);

	outl(e100.iobase + scb_pointer, 0);
	outw(e100.iobase + scb_cmd, scb_ruc_ldbase);

	outl(e100.iobase + scb_pointer, PADDR(e100.rfa[0]));
	outw(e100.iobase + scb_cmd, scb_ru_start);
}



static void
rfa_makeroom(void)
{
	int n,old;
	n = RFA_NEXT();
	old = RFA_TAIL();

	e100.rfa_noroom = 1;
	/* No room left */
	if ((old + 1) % RFA_SIZE == n)
		return;

	/* Mark the tail right behind the 'next' */
	e100.rfa_tail = (n - 1 + RFA_SIZE) % RFA_SIZE;
	RFA_INIT(e100.rfa_tail, 1);

	/* Now we have room */
	e100.rfa_noroom = 0;

	outw(e100.iobase + scb_cmd, scb_ru_resume);
}
/* This function implement the main part of receiving a frame
 *
 * Returns 0 on success
 * Returns -1 if fails
 */
int
e100_rem_rfd(char *p, int *sz)
{
	int n;
	n = RFA_NEXT();

	if ((e100.rfa[n]->status & rfd_status_c) &&
	    (e100.rfa[n]->status2 & rfd_status2_eof)) {
		*sz = e100.rfa[n]->status2 & rfd_status2_count_mask;
		memmove(p, e100.rfa[n]->data, *sz);
		RFA_INIT(n, 0); /* Mark it as free */
		RFA_INC();

		/* Now we can make room for RFA */
		if (e100.rfa_noroom)
			rfa_makeroom();
		return 0;	/* Successfully get an packet */
	}

	p = NULL;
	sz = 0;
	return -1;   /* If fails simply returning without a packet */
}

/* ***************SCB status word****************
 * +--------+--------+--------+--------+--------+
 * |STAT/ACK|  CUS   |  RUS   |   0    |   0    |
 * +--------+--------+--------+--------+--------+
 * |  15:8  |  7:6   |  5:2   |   1    |   0    |
 * +--------+--------+--------+--------+--------+
 ************************************************
 * STAT/ACK: Represent the causes of interrupts and _writing_ to the bits
 *	     will acknowledge the pending interrupts(Tell the adapter to
 *	     clear the interrupts).
 *
 * Bit 13, CNA:This bit indicates when the CU has left the active state
 *	      or has entered the idle state.(The tx work is done)
 *
 * Bit 12, RNR:This bit indicates when the RU leaves the ready state.
 *	      (The RFA is full)
 *
 * More details at P.43, intel openSDM
 */
static void
e100_cli(uint16_t type)
{
	outb(e100.iobase + scb_status + 1, type >> 8);
}

/* This function will be called for IRQ_NIC */
void
e100_intr()
{
	int s;
	s = inw(e100.iobase + scb_status);
	/* Acknowledge all of the current interrupt sources ASAP.
	 * For 82557, the bits[9:8] are reserved
	 */
	//outw(e100.iobase + scb_status, s & 0xfc00);
	if (s & scb_status_cna) {
		e100.cbl_count--; /* The packet got sent, so decrement
				   * the count of cbl
				   */
		if (!CBL_IS_EMPTY())
			e100_startcu();
		e100_cli(scb_status_cna);
	}

	if (s & scb_status_rnr || s & scb_status_rus_suspended){
		rfa_makeroom();
		e100_cli(scb_status_rnr);
	}

	e100_cli(scb_status_cx | scb_status_fr);
	/* Have to clear the interrupt on the PIC too */
	irq_eoi(e100.irq_line);
}

static void
wait5us(void)
{
	inb(0x84);
	inb(0x84);
	inb(0x84);
	inb(0x84);
}

static void
e100_init(void)
{
	/* Reset the adapter by IO Space */
	outl(e100.iobase + scb_port, software_reset);
	/* After reset,intel manul said we have to wait for 10us
	 * to issue another one
	 */
	wait5us();
	wait5us();

	cbl_init();
	rfa_init();
	
	/* Enable the previously allocated IRQ line */
	irq_setmask_8259A(irq_mask_8259A & ~(1 << e100.irq_line));
	ioapic_enable(e100.irq_line, mp_bcpu());//ismp?1:0);
}

/* This function will be called at PCI walking-thru and store the information
 * about I/O space and memory, irq line for our card
 */
int
e100_attach(struct pci_func *pcif)
{
	pci_func_enable(pcif);

	e100.membase = pcif->reg_base[0];
	e100.iobase = pcif->reg_base[1];
	e100.irq_line = pcif->irq_line;

	e100_init();

	return 1;
}
