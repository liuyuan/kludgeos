#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>
#include <inc/spinlock.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/time.h>
#include <kern/dev/e100.h>
#include <kern/mp.h>

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}


void
idt_init(void)
{
	extern struct Segdesc gdt[];
	extern uintptr_t vectors[];
	int i;
	/* All are interrupt gate because our kernel can't be interrupted */
	if (cpu() == mp_bcpu()) {
		for (i = 0; i < 52; i++)
			SETGATE(idt[i], 0, GD_KT, vectors[i], 0);
		SETGATE(idt[T_BRKPT], 0, GD_KT, vectors[T_BRKPT], 3); /* Reset INT3 privilege */
		SETGATE(idt[T_SYSCALL], 0, GD_KT, vectors[T_SYSCALL], 3); /* SYSCALL */
		// Setup a TSS so that we get the right stack
		// when we trap to the kernel.
		cpus[cpu()].ts.ts_esp0 = KSTACKTOP;
		cpus[cpu()].ts.ts_ss0 = GD_KD;

		gdt[GD_TSS >> 3] = SEG16(STS_T32A, (uint32_t) (&cpus[cpu()].ts),
					 sizeof(struct Taskstate), 0);
		gdt[GD_TSS >> 3].sd_s = 0;
		} else {
		/* CPUs share the same address space. So to avoid kstack overflow the layoutt is below:
		 *  
		 * +----------+       <---- KSTACKTOP
		 * | KSTACK   | CPU 0 (KSTKSIZE)
		 * +----------+
		 * | Invalid  |	      (PGSIZE)
		 * +----------+
		 * | KSTACK   | CPU 1
		 * +----------+
		 * | Invalid  |
		 * +----------+
		 * |	      | ....
		 * |	      |
		 */
		cpus[cpu()].ts.ts_esp0 = KSTACKTOP - (KSTKSIZE + PGSIZE) * cpu();
		cpus[cpu()].ts.ts_ss0 = GD_KD;

		cpus[cpu()].gdt[5] = SEG16(STS_T32A, (uint32_t) (&cpus[cpu()].ts),
						     sizeof(struct Taskstate), 0);
		cpus[cpu()].gdt[5].sd_s = 0;
	}
	ltr(GD_TSS);
	asm volatile("lidt idt_pd");
	cprintf("CPU %x: Kernel stack: %p ~ %p # %p ~ %p\n",cpu(), 
		cpus[cpu()].ts.ts_esp0, cpus[cpu()].ts.ts_esp0 - KSTKSIZE,
		vpt[PPN(cpus[cpu()].ts.ts_esp0) - 1], vpt[PPN(cpus[cpu()].ts.ts_esp0 - KSTKSIZE + 1)]);
}	

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p\n", tf);
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	cprintf("  err  0x%08x\n", tf->tf_err);
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	cprintf("  esp  0x%08x\n", tf->tf_esp);
	cprintf("  ss   0x----%04x\n", tf->tf_ss);
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
	if (tf->tf_trapno == T_NMI) {
		//panic("NOT IMPLEMENTED");
	}

	if (tf->tf_trapno == T_TLBFLUSH) {
		spin_lock(&tlb_lock);
		assert(tlb_va);
		tlb_invalidate(curenv->env_pgdir, tlb_va);
		spin_unlock(&tlb_lock);
		lapic_eoi();
		return;
	}

	if (tf->tf_trapno == T_HALT) {
		//panic("NOT IMPLEMENTED");
		lapic_eoi();
		return;
	}

	if (tf->tf_trapno == IRQ_OFFSET + IRQ_ERROR) {
		lapic_eoi();
		return;
	}

	if (tf->tf_trapno == T_PGFLT) {
		page_fault_handler(tf);
		return;
	}

	if (tf->tf_trapno == T_BRKPT || tf->tf_trapno == T_DEBUG)
		while (1) 
			monitor(tf);
	if (tf->tf_trapno == T_SYSCALL) {
		tf->tf_regs.reg_eax = syscall(tf->tf_regs.reg_eax, tf->tf_regs.reg_edx, tf->tf_regs.reg_ecx, 
					      tf->tf_regs.reg_ebx,tf->tf_regs.reg_edi, tf->tf_regs.reg_esi);
		return;
	}

	if (tf->tf_trapno == IRQ_OFFSET + IRQ_TIMER){
		if (cpu() == mp_bcpu())
			time_tick();
		lapic_eoi();
		assert(curenv->env_status == ENV_RUNNING);
		spin_lock(&curenv->env_lock);
		curenv->env_status = ENV_RUNNABLE;
		spin_unlock(&curenv->env_lock);
		sched_yield();
	}

	// Handle spurious interupts
	// The hardware sometimes raises these because of noise on the
	// IRQ line or other reasons. We don't care.
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_SPURIOUS) {
		cprintf("Spurious interrupt on irq 7\n");
		//print_trapframe(tf);
		lapic_eoi();
		return;
	}
	
	if (tf->tf_trapno == IRQ_OFFSET + IRQ_NIC) {
		e100_intr();
		lapic_eoi();
		return;
	}

	if (tf->tf_trapno == IRQ_OFFSET + IRQ_KBD) {
		kbd_intr();
		lapic_eoi();
		return;
	}
	// Unexpected trap: The user process or the kernel has a bug.
	print_trapframe(tf);
	if (tf->tf_cs == GD_KT)
		panic("unhandled trap in kernel");
	else {
		env_destroy(curenv);
		return;
	}
}

void
trap(struct Trapframe *tf)
{
	if ((tf->tf_cs & 3) == 3) {
		// Trapped from user mode.
		assert(curenv);
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}
	
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNING) {
		curenv->env_runs++;
		env_pop_tf(&curenv->env_tf);
	} else {
		spin_lock(&curenv->env_lock);
		curenv->env_status = ENV_RUNNABLE;
		spin_unlock(&curenv->env_lock);
		sched_yield();
	}
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;
	struct Page *p;
	struct UTrapframe *utf;
	fault_va = rcr2();

	if (!(tf->tf_cs & 0x3))
		panic("*bug in kernel*.Faulting at %x, EIP:%x\n", fault_va, tf->tf_eip);

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// The trap handler needs one word of scratch space at the top of the
	// trap-time stack in order to return.  In the non-recursive case, we
	// don't have to worry about this because the top of the regular user
	// stack is free.  In the recursive case, this means we have to leave
	// an extra word between the current top of the exception stack and
	// the new stack frame because the exception stack _is_ the trap-time
	// stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack, or the exception stack overflows,
	// then destroy the environment that caused the fault.
	if (curenv->env_pgfault_upcall) {
		/* Check if exception stack is accessible */
		user_mem_assert(curenv, (void *)(UXSTACKTOP - 1), 1, 0);

		/* Determine whether the pgfault handler itself causees it or not */
		if (tf->tf_esp < UXSTACKTOP && tf->tf_esp >= UXSTACKTOP - PGSIZE) {
			/* tf_esp already indicate the trap-time stack , so now
			 * we're in recursive case(pgfault handler caused it) */

			/* Check if pgfault handler is accessible or at user space */
			user_mem_assert(curenv, (void *)tf->tf_eip, 1, 0);

			if ((uintptr_t)(utf = (struct UTrapframe *)(tf->tf_esp - sizeof(*utf) - 4)) <
			    UXSTACKTOP - PGSIZE)
				/* The exception stack overflowed */
				goto destroy;
		} else
			/* None-recursive */
			utf = (struct UTrapframe *)(UXSTACKTOP - sizeof(*utf));

		/* Now it's safe to handle pgfault and set up utf */
		utf->utf_fault_va = fault_va;
		utf->utf_err = tf->tf_err;
		utf->utf_regs = tf->tf_regs;
		utf->utf_eip = tf->tf_eip;
		utf->utf_eflags = tf->tf_eflags;
		utf->utf_esp = tf->tf_esp;

		/* Change the control flow and switch stack in the user space */
		tf->tf_eip = (uintptr_t)curenv->env_pgfault_upcall;
		tf->tf_esp = (uintptr_t)utf;
		assert(curenv->env_status == ENV_RUNNING);
		curenv->env_runs++;
		env_pop_tf(&curenv->env_tf);
	}
destroy:
	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	//print_trapframe(tf);
	env_destroy(curenv);
}

