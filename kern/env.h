/* See COPYRIGHT for copyright information. */

#ifndef JOS_KERN_ENV_H
#define JOS_KERN_ENV_H

#include <inc/env.h>
#include <kern/mp.h>

#ifndef JOS_MULTIENV
#define JOS_MULTIENV 1
#endif

extern struct Spinlock env_table_lock;
extern struct Env *envs;		// All environments
#define  curenv (cpus[cpu()].curenv)	// Current environment

LIST_HEAD(Env_list, Env);		// Declares 'struct Env_list'

void	env_init(void);
int	env_alloc(struct Env **e, envid_t parent_id);
void	env_free(struct Env *e);
void	env_create(uint8_t *binary, size_t size);
void	env_destroy(struct Env *e);	// Does not return if e == curenv

int	envid2env(envid_t envid, struct Env **env_store, bool checkperm);
// The following two functions do not return
void	env_run(struct Env *e) __attribute__((noreturn));
void	env_pop_tf(struct Trapframe *tf) __attribute__((noreturn));

// For the grading script
#define ENV_CREATE2(start, size)	{		\
	extern uint8_t start[], size[];			\
	env_create(start, (int)size);			\
}

#define ENV_CREATE(x)			{		\
	extern uint8_t _binary_obj_##x##_start[],	\
		_binary_obj_##x##_size[];		\
	env_create(_binary_obj_##x##_start,		\
		(int)_binary_obj_##x##_size);		\
}

#endif // !JOS_KERN_ENV_H
