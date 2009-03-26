#ifndef JOS_DEV_E100_H
#define JOS_DEV_E100_H
#ifndef JOS_KERNEL
# error "This is a JOS kernel header; user programs should not #include it"
#endif

#include <inc/x86.h>
#include <dev/pci.h>

#define CBL_SIZE	4
#define RFA_SIZE	8

#define E100_MAX_PKT_SIZE		1518

struct E100 {
	uint32_t membase;
	uint32_t iobase;
	uint8_t irq_line;
	struct Tcb *cbl[CBL_SIZE];
	uint8_t cbl_head;
	uint8_t cbl_count;
	struct Rfd *rfa[RFA_SIZE];
	uint8_t rfa_head;
	uint8_t rfa_tail;
	uint8_t rfa_noroom;
};

extern struct E100 e100;

int e100_attach(struct pci_func *pcif);
int e100_add_tcb(char *packet, int size);
int e100_rem_rfd(char *p, int *sz);
void e100_intr(void);
#endif	// !JOS_DEV_E100_H
