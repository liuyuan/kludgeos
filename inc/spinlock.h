/* Written by Liu Yuan
 * Apr 9, 2009
 */
#ifndef JOS_KERN_SPINLOCK_H
#define JOS_KERN_SPINLOCK_H
#include <inc/types.h>
#include <inc/atomic.h>

struct Spinlock {
	atomic_t locked;
};

static inline void spin_lock(struct Spinlock *s)__attribute__((always_inline));
static inline void spin_unlock(struct Spinlock *s)__attribute__((always_inline));
static inline int  spin_locked(struct Spinlock *s)__attribute__((always_inline));
static inline void spin_init(struct Spinlock *s)__attribute__((always_inline));

#define SPIN_LOCK_UNLOCKED { ATOMIC_INIT(0) }

void
spin_lock(struct Spinlock *s)
{
	asm volatile(
		"\n1:\t"
		"mov	$1, %%eax \n\t"
		"lock; xchg %0, %%eax \n\t"
		"cmp	$0, %%eax \n\t"
		"je	3f \n\t"
		"2:\t	pause \n\t"
		"cmp	$0, %0 \n\t"
		"jne	2b \n\t"
		"jmp	1b \n\t"
		"3:\t\n"
		: "=m" (s->locked.counter)
		:
		: "%eax", "memory", "cc"
		);
}

void
spin_unlock(struct Spinlock *s)
{
	atomic_set(&s->locked, 0);
}

int
spin_locked(struct Spinlock *s)
{
	return atomic_read(&s->locked);
}

void
spin_init(struct Spinlock *s)
{
	atomic_set(&s->locked, 0);
}

#endif
