#include <inc/lib.h>
#include <inc/atomic.h>

int
pageref(void *v)
{
	pte_t pte;

	if (!(vpd[PDX(v)] & PTE_P))
		return 0;
	pte = vpt[VPN(v)];
	if (!(pte & PTE_P))
		return 0;
	return atomic_read(&pages[PPN(pte)].pp_ref);
}
