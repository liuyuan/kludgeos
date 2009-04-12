/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>
#include <kern/time.h>
#include <kern/dev/e100.h>
#include <kern/reboot.h>


static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
	user_mem_assert(curenv, s, len, 0);
	cprintf("%.*s", len, s);

}

// Read a character from the system console.
// Returns the character.
static int
sys_cgetc(void)
{
	int c;

	/* The cons_getc() primitive doesn't wait for a character, But we do.
	 * TODO: Make kernel interruptable later
	 * Now just a makeshift, without kernel being interruptable, Ether
	 * device refuses to function properly.
	 */
	while ((c = cons_getc()) == 0);
/*  { */
/* 		assert(curenv->env_status == ENV_RUNNING); */
/* 		spin_lock(&curenv->env_lock); */
/* 		curenv->env_status = ENV_RUNNABLE; */
/* 		spin_unlock(&curenv->env_lock); */
/* 		sched_yield();/\* This enables daemons to get CPU cycles when */
/* 			       * sh is running. */
/* 			       *\/ */
/* 	} */
	return c;
}

static void
sys_reboot(void)
{
	reboot();
}

static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

static void
sys_yield(void)
{
	assert(curenv->env_status == ENV_RUNNING);
	spin_lock(&curenv->env_lock);
	curenv->env_status = ENV_RUNNABLE;
	spin_unlock(&curenv->env_lock);
	sched_yield();
}

// Allocate a new environment.
static envid_t
sys_exofork(void)
{

	struct Env *e;
	if (env_alloc(&e, curenv->env_id) < 0)
		return -E_NO_FREE_ENV;
	e->env_status = ENV_NOT_RUNNABLE;
	e->env_tf = curenv->env_tf;
	e->env_tf.tf_regs.reg_eax = 0;		    /* For child */
	return e->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
static int
sys_env_set_status(envid_t envid, int status)
{
	struct Env *e;
	int r;
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	if (status != ENV_RUNNABLE && status != ENV_NOT_RUNNABLE)
		return -E_INVAL;

	spin_lock(&e->env_lock);
	e->env_status = status;
	spin_unlock(&e->env_lock);
	return 0;
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3) with interrupts enabled.
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	struct Env *e;
	int r;
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;

	user_mem_assert(e, tf, SIZEOF_STRUCT_TRAPFRAME, 0);
	spin_lock(&e->env_lock);
	e->env_tf = *tf;
	spin_unlock(&e->env_lock);
	return 0;
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	int r;
	struct Env *e;
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	/* The security-check of func is pushed onto page_fault_handler  */
	spin_lock(&e->env_lock);
	e->env_pgfault_upcall = func;
	spin_unlock(&e->env_lock);
	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// Return 0 on success, < 0 on error.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	struct Env *e;
	struct Page *p;
	int r;
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;

	if (!(perm & PTE_U) || !(perm & PTE_P) || perm & ~PTE_USER)
		return -E_INVAL;

	if ((uintptr_t)va >= UTOP || ((uintptr_t)va % PGSIZE))
		return -E_INVAL;

	if ((r = page_alloc(&p)) < 0)
		return r;

	memset(page2kva(p), 0, PGSIZE);
	if ((r = page_insert(e->env_pgdir, p, va, perm)) < 0) {
		assert(atomic_read(&p->pp_ref) == 0);
		page_free(p);
		return r;
	}
	return 0;
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Return 0 on success, < 0 on error.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	struct Env *es, *ed;
	struct Page *p;
	pte_t *pte;
	int r;
	if ((r = envid2env(srcenvid, &es, 1)) < 0 || (r = envid2env(dstenvid, &ed, 1)) < 0)
		return r;

	if ((uintptr_t)srcva >= UTOP || (uintptr_t)srcva % PGSIZE ||
	    (uintptr_t)dstva >= UTOP || (uintptr_t)dstva % PGSIZE)
		return -E_INVAL;

	if ((!(perm & (PTE_U + PTE_P))) || perm & ~PTE_USER)
		return -E_INVAL;

	if (!(p = page_lookup(es->env_pgdir, srcva, &pte)))
		return -E_INVAL;

	if (((perm & PTE_W) && !(*pte & PTE_W)))
		return -E_INVAL;
	
	if ((r = page_insert(ed->env_pgdir, p, dstva, perm)) < 0)
		return r;
	
	return 0;
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
// Return 0 on success, < 0 on error.
static int
sys_page_unmap(envid_t envid, void *va)
{
	struct Env *e;
	int r;
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	
	if ((uintptr_t)va >= UTOP || (uintptr_t)va % PGSIZE)
		return -E_INVAL;
	
	page_remove(e->env_pgdir, va);
	return 0;
}

// Try to send 'value' to the target env 'envid'.
// Returns 0 on success where no page mapping occurs,
// 1 on success where a page mapping occurs, and < 0 on error.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env *target;
	int r;
	struct Page *p;
	if ((r = envid2env(envid, &target, 0)) < 0)
		return r;

	if (!target->env_ipc_recving)
		return -E_IPC_NOT_RECV;
	spin_lock(&target->env_lock);
	target->env_ipc_recving = 0;
	target->env_ipc_from = curenv->env_id;
	target->env_ipc_value = value;
	target->env_ipc_perm = 0;

	if (srcva) {
		if ((uintptr_t)srcva >= UTOP || (uintptr_t)srcva % PGSIZE) {
			spin_unlock(&target->env_lock);
			return -E_INVAL;
		}
	
		if (!(perm & PTE_U) || !(perm &PTE_P) || perm & ~PTE_USER) {
			spin_unlock(&target->env_lock);
			return -E_INVAL;
		}
		
		if ((p = page_lookup(curenv->env_pgdir, srcva, 0)) == NULL) {
			spin_unlock(&target->env_lock);
			return -E_INVAL;
		}

		/* Now it's safe to use srcva */
		if (target->env_ipc_dstva) {
			if ((r = page_insert(target->env_pgdir, p, target->env_ipc_dstva, perm)) < 0) {
				spin_unlock(&target->env_lock);
				return r;
			}
			target->env_ipc_perm = perm;
			target->env_status = ENV_RUNNABLE; /* Wake up the blocking recver */
			spin_unlock(&target->env_lock);
			return 1;
		}
	}

	target->env_status = ENV_RUNNABLE; /* Wake up the blocking recver */
	spin_unlock(&target->env_lock);
	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
// return 0 on success.
// Return < 0 on error.
static int
sys_ipc_recv(void *dstva)
{
	spin_lock(&curenv->env_lock);
	if (dstva) {
		if ((uintptr_t)dstva >= UTOP || (uintptr_t)dstva % PGSIZE) {
			spin_unlock(&curenv->env_lock);
			return -E_INVAL;
		}
		curenv->env_ipc_dstva = dstva;
	}
	assert(curenv->env_status == ENV_RUNNING);
	curenv->env_ipc_recving = 1;
	curenv->env_status = ENV_NOT_RUNNABLE; /* Go blocking(sleep) */
	curenv->env_tf.tf_regs.reg_eax = 0; /* Ensure syscall eventually return 0 */
	spin_unlock(&curenv->env_lock);
	sched_yield();
}

static int
sys_time_msec()
{
	return (int) time_msec();
}

static int
sys_nic_send(char *packet, int size)
{
	return e100_add_tcb(packet, size);
}

static int
sys_nic_recv(char *packet, int *size)
{
	user_mem_assert(curenv, packet, E100_MAX_PKT_SIZE, PTE_P | PTE_W);
	return e100_rem_rfd(packet, size);
}
// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	switch (syscallno) {
	case SYS_cputs:
		sys_cputs((char *)a1, a2);
		return 0;	/* Sys_cputs is of no interest in return-value */

	case SYS_cgetc:
		return sys_cgetc();

	case SYS_reboot:
		sys_reboot();
		return 0;

	case SYS_env_destroy:
		return sys_env_destroy(a1);

	case SYS_getenvid:
		return sys_getenvid();

	case SYS_yield:
		sys_yield();	/* Never return */

	case SYS_page_alloc:
		return sys_page_alloc((envid_t)a1, (void *)a2, (int)a3);

	case SYS_page_map:
		return sys_page_map((envid_t)a1, (void *)a2, (envid_t)a3, (void *)a4, (int)a5);

	case SYS_page_unmap:
		return sys_page_unmap((envid_t)a1, (void *)a2);

	case SYS_exofork:
		return sys_exofork();

	case SYS_env_set_status:
		return sys_env_set_status((envid_t)a1, (int)a2);

	case SYS_env_set_trapframe:
		return sys_env_set_trapframe((envid_t)a1, (struct Trapframe *)a2);

	case SYS_env_set_pgfault_upcall:
		return sys_env_set_pgfault_upcall((envid_t)a1, (void *)a2);

	case SYS_ipc_try_send:
		return sys_ipc_try_send((envid_t)a1, (uint32_t)a2, (void *)a3, (unsigned)a4);

	case SYS_ipc_recv:
		return sys_ipc_recv((void *)a1); /* Never return */

	case SYS_time_msec:
		return sys_time_msec();

	case SYS_nic_send:
		return sys_nic_send((char *)a1, (int)a2);

	case SYS_nic_recv:
		return sys_nic_recv((char *)a1, (int *)a2);

	default:
		panic("Unknown system call!");
	}
}
