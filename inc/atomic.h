/* Written by Liu Yuan
 * Apr 9, 2009
 */
#ifndef JOS_KERN_ATOMIC_H
#define JOS_KERN_ATOMIC_H


#ifdef 	JOS_SMP
#define LOCK "lock ; "
#else
#define LOCK ""
#endif

typedef struct { 
	volatile uint32_t counter;
}atomic_t;

#define ATOMIC_INIT(i)		{ (i) }	
#define atomic_read(v)		( (v)->counter )
#define atomic_set(v,i)		(((v)->counter) = (i))

static inline void atomic_inc(atomic_t *v)__attribute__((always_inline));
static inline void atomic_dec(atomic_t *v)__attribute__((always_inline));
static inline int atomic_dec_and_test(atomic_t *v)__attribute__((always_inline));

void
atomic_inc(atomic_t *v)
{
	asm volatile(
		LOCK "incl %0"
		: "+m" (v->counter)
		:
		: "cc");/* CC means condition code rigister(eflags in x86) */
}

void
atomic_dec(atomic_t *v)
{
	asm volatile(
		LOCK "decl %0"
		: "+m" (v->counter)
		:
		: "cc");
}

/* Return true if result is zero. */
int
atomic_dec_and_test(atomic_t *v)
{
	unsigned char c;
	asm volatile(
		LOCK "decl %0; sete %1"
		: "+m" (v->counter), "=qm" (c)
		:
		: "cc");
	return c != 0;
}

#endif
