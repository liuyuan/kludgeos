/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>

#include <kern/picirq.h>


###################################################################
# exceptions/interrupts
###################################################################

/* The TRAPHANDLER macro defines a globally-visible function for handling
 * a trap.  It pushes a trap number onto the stack, then jumps to _alltraps.
 * Use TRAPHANDLER for traps where the CPU automatically pushes an error code.
 */ 
#define TRAPHANDLER(name, num)						\
	.globl name;		/* define global symbol for 'name' */	\
	.type name, @function;	/* symbol type is function */		\
	.align 2;		/* align function definition */		\
	name:			/* function starts here */		\
	pushl $(num);							\
	jmp _alltraps

/* Use TRAPHANDLER_NOEC for traps where the CPU doesn't push an error code.
 * It pushes a 0 in place of the error code, so the trap frame has the same
 * format in either case.
 */
#define TRAPHANDLER_NOEC(name, num)					\
	.globl name;							\
	.type name, @function;						\
	.align 2;							\
	name:								\
	pushl $0;							\
	pushl $(num);							\
	jmp _alltraps

.text

/*
 * Lab 3: Your code here for generating entry points for the different traps.
 */
TRAPHANDLER_NOEC(v0, T_DIVIDE)
TRAPHANDLER_NOEC(v1, T_DEBUG)
TRAPHANDLER_NOEC(v2, T_NMI)
TRAPHANDLER_NOEC(v3, T_BRKPT)
TRAPHANDLER_NOEC(v4, T_OFLOW)
TRAPHANDLER_NOEC(v5, T_BOUND)
TRAPHANDLER_NOEC(v6, T_ILLOP)
TRAPHANDLER_NOEC(v7, T_DEVICE)
TRAPHANDLER(v8, T_DBLFLT)
TRAPHANDLER_NOEC(v9, T_COPROC)
TRAPHANDLER(v10, T_TSS)
TRAPHANDLER(v11, T_SEGNP)
TRAPHANDLER(v12, T_STACK)
TRAPHANDLER(v13, T_GPFLT)
TRAPHANDLER(v14, T_PGFLT)
TRAPHANDLER_NOEC(v15, T_RES)
TRAPHANDLER_NOEC(v16, T_FPERR)
TRAPHANDLER(v17, T_ALIGN)
TRAPHANDLER_NOEC(v18, T_MCHK)
TRAPHANDLER_NOEC(v19, T_SIMDERR)
TRAPHANDLER_NOEC(v32, 32)
TRAPHANDLER_NOEC(v33, IRQ_OFFSET + IRQ_KBD)
TRAPHANDLER_NOEC(v39, IRQ_OFFSET + IRQ_SPURIOUS)
TRAPHANDLER_NOEC(v43, IRQ_OFFSET + IRQ_NIC)
TRAPHANDLER_NOEC(v46, IRQ_OFFSET + IRQ_IDE)
TRAPHANDLER_NOEC(v48, T_SYSCALL)

/*
 * Lab 3: Your code here for _alltraps
 */
_alltraps:
	pushl %ds
	pushl %es
	pushal
	# Set up data segments
	movl $GD_KD, %eax
	movw %ax, %ds
	movw %ax, %es
	
	# Now We've built trap frame, then call trap(tf)
	pushl %esp
	call trap
	addl $4, %esp		# Clear up the stack frame
	
	# Undo the changes that we made before calling trap(tf)
	popal
	popl %es
	popl %ds
	addl $8, %esp
	# Now stack holds what iret expects
	iret
.data
.global vectors
vectors:
	.long v0
	.long v1
	.long v2
	.long v3
	.long v4
	.long v5
	.long v6
	.long v7
	.long v8
	.long v9
	.long v10
	.long v11
	.long v12
	.long v13
	.long v14
	.long v15
	.long v16
	.long v17
	.long v18
	.long v19
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long v32
	.long v33
	.long 0
	.long 0
	.long 0
	.long 0
	.long 0
	.long v39
	.long 0
	.long 0
	.long 0
	.long v43
	.long 0
	.long 0
	.long v46
	.long 0
	.long v48
