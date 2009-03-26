// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>
#include <kern/pmap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>
#include <kern/env.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display informatin about the kernel call frame",mon_backtrace },
	{ "showmappings", "Display the physical page mappings", mon_showmappings },
	{ "setperm", "Set the permission of the page", mon_setperm },
	{ "clrperm", "Clear the permission of the page", mon_clrperm },
	{ "continue", "Continue the execution", mon_continue },
	{ "step", "Single-step one instruction", mon_step },
};
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))

unsigned read_eip();

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start %08x (virt)  %08x (phys)\n", _start, _start - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		(end-_start+1023)/1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	uint32_t *ebp = (uint32_t *)read_ebp();
	struct Eipdebuginfo info;

	cprintf("Stack backtrace:\n");
	while (ebp != 0) {
		cprintf("  ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n",
			ebp, *(ebp + 1),*(ebp + 2), *(ebp + 3),
			*(ebp + 4), *(ebp + 5), *(ebp + 6));

		debuginfo_eip((uintptr_t)*(ebp + 1), &info);
		cprintf("\t%s:%d: %.*s\n", info.eip_file, info.eip_line,
			info.eip_fn_namelen, info.eip_fn_name);
		ebp = (uint32_t *)*ebp;
	}
	return 0;
}

int
mon_showmappings(int argc, char **argv, struct Trapframe *tf)
{
	pte_t *p;
	int i;
	uintptr_t from, to;
	from = strtol(argv[1], NULL, 0);
	to = strtol(argv[2], NULL, 0);
	if (argc != 3 || from > to) {
		cprintf("Usage:\t showmappings addr_from(low) addr_to(high)\n");
		return 0;
	}	
	cprintf("Physical page mappings:\n");
	for (i = 0; i <= (to - from) / PGSIZE; i++) {
		if ((p = pgdir_walk(KADDR(rcr3()), (void *)(from + PGSIZE * i), 0)) != NULL)
			cprintf("\tva:%08p pa:%08p permbits:%d\n", from + PGSIZE * i,
				PTE_ADDR(*p), *p & 0xfff);
		else
			cprintf("\taddress [%08p, %08p) has no mapping.\n", from + PGSIZE * i,
				from + PGSIZE * i + PGSIZE);
	}
	return 0;
}

void static
altperm(int argc, char **argv, struct Trapframe *tf, int indicator)
{
	pte_t *p;
	uintptr_t a;
	int n;
	a = strtol(argv[1], NULL, 0);
	n = strtol(argv[2], NULL, 0);
	if (argc !=3) {
		if (indicator == 1)
			cprintf("Usage:\t setperm address num_bit\n");
		else
			cprintf("Usage:\t clrperm address num_bit\n");
		return;
	}
	if ((p = pgdir_walk(KADDR(rcr3()), (void *)a, 0)) == NULL) {
		cprintf("The address %p doesn't map any page.\n", a);
		return;
	}
	if ( n > 11) {
		cprintf("Invalid bit %d.\n", n);
		return;
	}
	/* Now we have a valid address and the number of bit*/
	if (indicator == 1)
		*p |= 1 << n;	/* Set */
	else
		*p &= ~(1 << n); /* Clear */
	return;
}

int
mon_setperm(int argc, char **argv, struct Trapframe *tf)
{
	altperm(argc, argv, tf, 1);
	return 0;
}

int 
mon_clrperm(int argc, char **argv, struct Trapframe *tf)
{
	altperm(argc, argv, tf, 0);
	return 0;
}

int 
mon_continue(int argc, char **argv, struct Trapframe *tf)
{
	int ef;
	ef = read_eflags();
	if (tf->tf_eflags & 0x100)
		tf->tf_eflags &= ~(ef | 1 << 8); /* Clear TF to disable single-step mode */
	env_pop_tf(tf);
}

int
mon_step(int argc, char **argv, struct Trapframe *tf)
{
	int ef;
	ef = read_eflags();
	if (!(tf->tf_eflags & 0x100))
		tf->tf_eflags |= (ef | 1 << 8); /* Set TF to enable single-step mode */
	env_pop_tf(tf);
}
/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}

// return EIP of caller.
// does not work if inlined.
// putting at the end of the file seems to prevent inlining.
unsigned
read_eip()
{
	uint32_t callerpc;
	__asm __volatile("movl 4(%%ebp), %0" : "=r" (callerpc));
	return callerpc;
}
