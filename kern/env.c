/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>
#include <inc/spinlock.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>


struct Env *envs = NULL;		// All environments
//struct Env *curenv = NULL;	        // The current env
static struct Env_list env_free_list;	// Free list

struct Spinlock env_table_lock;

#define ENVGENSHIFT	12		// >= LOGNENV

// Converts an envid to an env pointer.
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
int
envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	if (envid == 0) {
		*env_store = curenv;
		return 0;
	}

	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}


void
env_init(void)
{
	int i;
	for (i = NENV -1; i >= 0; i--) {
		envs[i].env_status = ENV_FREE;
		envs[i].env_id = 0;
		LIST_INSERT_HEAD(&env_free_list, &envs[i], env_link);
		spin_init(&envs[i].env_lock);
	}
	spin_init(&env_table_lock);
}

// Initialize the kernel virtual memory layout for environment e.
// Returns 0 on success, < 0 on error.  Errors include:
//	-E_NO_MEM if page directory or table could not be allocated.
//
static int
env_setup_vm(struct Env *e)
{
	int i, r;
	struct Page *p = NULL;

	if ((r = page_alloc(&p)) < 0)
		return r;

	// Now, set e->env_pgdir and e->env_cr3,
	// and initialize the page directory.
	atomic_inc(&p->pp_ref);
	memmove(page2kva(p), boot_pgdir, PGSIZE);
	e->env_pgdir = (pde_t *)page2kva(p);
	e->env_cr3 = page2pa(p);

	// VPT and UVPT map the env's own page table, with
	// different permissions.
	e->env_pgdir[PDX(VPT)]  = e->env_cr3 | PTE_P | PTE_W;
	e->env_pgdir[PDX(UVPT)] = e->env_cr3 | PTE_P | PTE_U;

	return 0;
}


// Allocates and initializes a new environment.
// On success, the new environment is stored in *newenv_store.
//
// Returns 0 on success, < 0 on failure.  Errors include:
//	-E_NO_FREE_ENV if all NENVS environments are allocated
//	-E_NO_MEM on memory exhaustion
int
env_alloc(struct Env **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	struct Env *e;

	spin_lock(&env_table_lock);
	if (!(e = LIST_FIRST(&env_free_list))) {
		spin_unlock(&env_table_lock);
		return -E_NO_FREE_ENV;
	}

	if ((r = env_setup_vm(e)) < 0) {
		spin_unlock(&env_table_lock);
		return r;
	}

	// Generate an env_id for this environment.
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	if (generation <= 0)	// Don't create a negative env_id.
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs);
	
	// Set the basic status variables.
	e->env_parent_id = parent_id;
	e->env_status = ENV_RUNNABLE;
	e->env_runs = 0;

	// Clear out all the saved register state,
	// to prevent the register values
	// of a prior environment inhabiting this Env structure
	// from "leaking" into our new environment.
	memset(&e->env_tf, 0, sizeof(e->env_tf));

	// Set up appropriate initial values for the segment registers.
	e->env_tf.tf_ds = GD_UD | 3;
	e->env_tf.tf_es = GD_UD | 3;
	e->env_tf.tf_ss = GD_UD | 3;
	e->env_tf.tf_esp = USTACKTOP;
	e->env_tf.tf_cs = GD_UT | 3;
	e->env_tf.tf_eflags |= FL_IF;
	// Clear the page fault handler until user installs one.
	e->env_pgfault_upcall = 0;

	// Also clear the IPC receiving flag.
	e->env_ipc_recving = 0;

	// If this is the file server (e == &envs[1]) give it I/O privileges.
	if (e == &envs[1])
		e->env_tf.tf_eflags |= FL_IOPL_3;
	// commit the allocation
	LIST_REMOVE(e, env_link);
	spin_unlock(&env_table_lock);
	*newenv_store = e;

	return 0;
}


// Allocate len bytes of physical memory for environment env,
// and map it at virtual address va in the environment's address space.
static void
segment_alloc(struct Env *e, void *va, size_t len)
{
	struct Page *p;
	uintptr_t v;
	int np, i, r;

	v = (uintptr_t)ROUNDDOWN(va, PGSIZE);
	np = ROUNDUP(len, PGSIZE) / PGSIZE;
	for (i = 0; i< np; i++, v += PGSIZE) {
		if ((r = page_alloc(&p)) < 0)
			panic("segment_alloc:%e\n", r);
		page_insert(e->env_pgdir, p, (void *)v, PTE_U | PTE_W | PTE_P);
	}
}

// Set up the initial program binary, stack, and processor flags
// for a user process.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
static void
load_icode(struct Env *e, uint8_t *binary, size_t size)
{
	int i,r;
	struct Elf *elf;
	struct Proghdr *ph;
	struct Page *p;

	lcr3(e->env_cr3);
	elf = (struct Elf*)binary;
	if (elf->e_magic != ELF_MAGIC)
		panic("load_icode: not an ELF binary");
	
	e->env_tf.tf_eip = elf->e_entry;

	ph = (struct Proghdr*)(binary + elf->e_phoff);
	for (i = 0; i < elf->e_phnum; i++, ph++) {
		if (ph->p_type != ELF_PROG_LOAD)
			continue;
		segment_alloc(e, (void *)ph->p_va, ph->p_memsz);
		memmove((void *)ph->p_va, binary + ph->p_offset, ph->p_filesz);
		memset((void *)(ph->p_va + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
	}

	if ((r = page_alloc(&p) < 0))
		panic("load_icode:%e\n", r);
	page_insert(e->env_pgdir, p, (void *)(USTACKTOP - PGSIZE), PTE_U | PTE_W | PTE_P);
	lcr3(boot_cr3);
}


// Allocates a new env and loads the named elf binary into it.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
// The new env's parent ID is set to 0.
void
env_create(uint8_t *binary, size_t size)
{
	struct Env *e;
	env_alloc(&e, 0);
	load_icode(e, binary, size);	
}


// Frees env e and all memory it uses.
void
env_free(struct Env *e)
{
	pte_t *pt;
	uint32_t pdeno, pteno;
	physaddr_t pa;
	
	if (e->env_status == ENV_FREE)
		return;
	
	// If freeing the current environment, switch to boot_pgdir
	// before freeing the page directory, just in case the page
	// gets reused.
	if (e == curenv)
		lcr3(boot_cr3);

	// Note the environment's demise.
	// Flush all mapped pages in the user portion of the address space
	static_assert(UTOP % PTSIZE == 0);
	for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

		// only look at mapped page tables
		if (!(e->env_pgdir[pdeno] & PTE_P))
			continue;

		// find the pa and va of the page table
		pa = PTE_ADDR(e->env_pgdir[pdeno]);
		pt = (pte_t*) KADDR(pa);

		// unmap all PTEs in this page table
		for (pteno = 0; pteno <= PTX(~0); pteno++) {
			if (pt[pteno] & PTE_P)
				page_remove(e->env_pgdir, PGADDR(pdeno, pteno, 0));
		}

		// free the page table itself
		e->env_pgdir[pdeno] = 0;
		page_decref(pa2page(pa));
	}

	// free the page directory
	pa = e->env_cr3;
	e->env_pgdir = 0;
	e->env_cr3 = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	e->env_status = ENV_FREE;
	spin_lock(&env_table_lock);
	LIST_INSERT_HEAD(&env_free_list, e, env_link);
	spin_unlock(&env_table_lock);
}


// Frees environment e.
// If e was the current env, then runs a new environment (and does not return
// to the caller).
void
env_destroy(struct Env *e) 
{
	env_free(e);

	if (curenv == e) {
		curenv = NULL;
		sched_yield();
	}
}



// Restores the register values in the Trapframe with the 'iret' instruction.
// This exits the kernel and starts executing some environment's code.
// This function does not return.
void
env_pop_tf(struct Trapframe *tf)
{
	__asm __volatile("movl %0,%%esp\n"
		"\tpopal\n"
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
		"\tiret"
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
}

// Context switch from curenv to env e.
//  (This function does not return.)
void
env_run(struct Env *e)
{
	//static int c = -1;
	curenv = e;
	e->env_runs++;
	lcr3(e->env_cr3);
	e->env_status = ENV_RUNNING;
	spin_unlock(&e->env_lock); /* It's the caller's job to acquire the lock */
	//if (cpu() != c) {
	//	cprintf("CPU %x: Starting env %x\n", cpu(), ENVX(e->env_id));
	//	c = cpu();
	//}
	env_pop_tf(&e->env_tf);
	/* Never reaches here */
}

