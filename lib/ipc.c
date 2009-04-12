// User-level IPC library routines

#include <inc/lib.h>
// Receive a value via IPC and return it.
// If 'pg' is nonnull, then any page sent by the sender will be mapped at
//	that address.
// If 'fromenv' is nonnull, then store the IPC sender's envid in *fromenv.
// If 'perm' is nonnull, then store the IPC sender's page permission in *perm
//	(this is nonzero iff a page was successfully transferred to 'pg').
// If the system call fails, then store 0 in *fromenv and *perm (if
//	they're nonnull) and return the error.
int32_t
ipc_recv(envid_t *from_env_store, void *pg, int *perm_store)
{
	int r;
	/* NOTE:Hum, I use null to indicate 'no page' -_-. Might be not the
	 * right value, but it works
	 */
	if ((r = sys_ipc_recv(pg)) < 0) {
		*from_env_store = 0;
		*perm_store= 0;
		return r;
	}
	if (from_env_store)
		*from_env_store = env->env_ipc_from;
	if (perm_store)
		*perm_store = env->env_ipc_perm;

	return env->env_ipc_value;
}

// Send 'val' (and 'pg' with 'perm', assuming 'pg' is nonnull) to 'toenv'.
// This function keeps trying until it succeeds.
// It should panic() on any error other than -E_IPC_NOT_RECV.
void
ipc_send(envid_t to_env, uint32_t val, void *pg, int perm)
{
	int r;
	while (( r = sys_ipc_try_send(to_env, val, pg, perm)) == -E_IPC_NOT_RECV) {
		sys_yield();
	}
	if (r < 0)
		panic("icp_send %e", r);
}

