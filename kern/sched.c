/* Written by Liu Yuan */

#include <inc/assert.h>
#include <inc/spinlock.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>

/* Implement simple round-robin scheduling. */
void
sched_yield(void)
{
	
	static int pre = 0;	/* Env previously run */
	int i;
	/* Get an runnable 'env' to run in circular fashion */
	spin_lock(&env_table_lock);
	for (i = 1; i < NENV; i++) {
		pre = (pre + 1) % NENV ? (pre + 1) % NENV : 1;
		spin_lock(&envs[pre].env_lock);
		if (envs[pre].env_status == ENV_RUNNABLE) {
			spin_unlock(&env_table_lock);
			env_run(&envs[pre]);
		}
		spin_unlock(&envs[pre].env_lock);
	}
	
	/* Run the special idle environment when nothing else is runnable. */
	spin_unlock(&env_table_lock);
	while (envs[0].env_status != ENV_RUNNABLE);// cprintf("...cpu %x...\n", cpu());
	spin_lock(&envs[0].env_lock);
	env_run(&envs[0]);
}
