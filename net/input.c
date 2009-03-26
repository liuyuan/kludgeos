#include "ns.h"
#include<inc/lib.h>

void
input(envid_t ns_envid) {
	int r;
	struct jif_pkt *pkt;
	binaryname = "ns_input";

	// LAB 6: Your code here:
	// 	- read a packet from the device driver
	//	- send it to the network server

	// Allocate of a page of memory to store
	pkt = (struct jif_pkt *)UTEMP;
	while(1) {
		if ((r = sys_page_alloc(0, UTEMP, PTE_U | PTE_P | PTE_W)) < 0)
			panic("input env %e", r);

		while ((r = sys_nic_recv(pkt->jp_data, &pkt->jp_len)) < 0)
			sys_yield(); /* FIXME:NOT well debuged */

		ipc_send(ns_envid, NSREQ_INPUT, pkt, PTE_U|PTE_P|PTE_W);
	}
}
