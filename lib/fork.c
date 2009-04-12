// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
#define PTE_COW		0x800


// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *)ROUNDDOWN(utf->utf_fault_va, PGSIZE);
	uint32_t err = utf->utf_err;
	int r;

	pte_t pte = vpt[(uintptr_t)addr / PGSIZE];/* vpt[] is set by entry.S */
	if (!(err & FEC_WR && pte & PTE_COW))
		panic("pgfault:error code is %x, addr is %x", err, addr);

	if ((r = sys_page_alloc(0, PFTEMP, PTE_U | PTE_P | PTE_W)) < 0)
		panic("pafault: %e", r);

	memmove(PFTEMP, addr, PGSIZE);

	if ((r = sys_page_map(0, PFTEMP, 0, addr, PTE_W | PTE_U | PTE_P)) < 0)
		panic("pafault: %e", r);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well. 
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
// 
static int
duppage(envid_t envid, unsigned pn)
{
	int r;
	void *addr;
	pte_t pte;

	pte = vpt[pn] & PTE_USER;
	addr = (void *)(pn * PGSIZE);

	if (!(pte & PTE_SHARE) && (pte & PTE_W || pte & PTE_COW)) {
		/* Map the child */
		if ((r = sys_page_map(0, addr, envid, addr, PTE_P | PTE_U | PTE_COW)) < 0)
			return r;
		/* Remap the parent */
		if ((r = sys_page_map(0, addr, 0, addr, PTE_P | PTE_U | PTE_COW)) < 0)
			return r;
	} else {
		/* NO COWs or share-page*/
		if ((r = sys_page_map(0, addr, envid, addr, pte)) < 0)
			return r;
	}
	return 0;
}


// User-level fork with copy-on-write.
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
envid_t
fork(void)
{
	envid_t envid;
	uint32_t pn;
	int r, i, j;
	extern void _pgfault_upcall(void);

	set_pgfault_handler(pgfault);
	
	if ((envid = sys_exofork()) < 0) {
		return envid;	/* Error */
	}
	if (envid == 0) {	/* Child */
		/* Have to adjust the envid */
		env = &envs[ENVX(sys_getenvid())];

		/* Child returns 0 for fork() */
		return 0;
	}
	/* Parent */
	/* Set up child's address space */
	for (i = 0; i < VPD(UTOP); i++)
		if (vpd[i] & PTE_P)
			for (j = 0; j < NPTENTRIES; j++)
				if (vpt[i * NPDENTRIES + j] & PTE_P){
					if (i*NPDENTRIES + j == (UXSTACKTOP - PGSIZE)/PGSIZE)
						/* Get over exception stack */
						break;

					if ((r = duppage(envid, i*NPDENTRIES + j)) < 0)
						return r;
				}
	if ((r = sys_page_alloc(envid, (void *)(UXSTACKTOP - PGSIZE), 
				PTE_W | PTE_U | PTE_P)) < 0) /* The exception stack */
		return r;

	/* Set up child's pgfault_upcall handler */
	if ((r = sys_env_set_pgfault_upcall(envid, &_pgfault_upcall)) < 0)
		return r;

	/* Mark the child as runnable */
	if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
		return r;

	return envid;
}
