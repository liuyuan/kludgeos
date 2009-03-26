#include "ns.h"
#include <inc/lib.h>
#include <inc/memlayout.h>

void
output(envid_t ns_envid) {
	int32_t r;
	envid_t whom;
	int perm;
	struct jif_pkt *pkt;
	binaryname = "ns_output";

	// LAB 6: Your code here:
	// 	- read a packet from the network server
	//	- send the packet to the device driver
	pkt = (struct jif_pkt *)UTEMP;
	while (1) {
		if ((r = ipc_recv(&whom, pkt, &perm)) != NSREQ_OUTPUT)
			continue;
		/* Simply drop the packet if fails */
		sys_nic_send(pkt->jp_data, pkt->jp_len);
	}

}
