// Simple implementation of cprintf console output for the kernel,
// based on printfmt() and the kernel console's cputchar().

/* Modified by Liu Yuan for SMP */
#include <inc/types.h>
#include <inc/stdio.h>
#include <inc/stdarg.h>
#include <inc/spinlock.h>

static struct Spinlock console_lock = SPIN_LOCK_UNLOCKED;

static void
putch(int ch, int *cnt)
{
	cputchar(ch);
	*cnt++;
}

int
vcprintf(const char *fmt, va_list ap)
{
	int cnt = 0;

	vprintfmt((void*)putch, &cnt, fmt, ap);
	return cnt;
}

int
cprintf(const char *fmt, ...)
{
	va_list ap;
	int cnt;
	spin_lock(&console_lock);
	va_start(ap, fmt);
	cnt = vcprintf(fmt, ap);
	va_end(ap);
	spin_unlock(&console_lock);
	return cnt;
}

