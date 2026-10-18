#ifndef GUEST_THREADS_H
#define GUEST_THREADS_H

#include <stdint.h>

/* pthread and OSAtomic thunks (runtime/src/thunk_threads.c).
 *
 * Guest objects keep their i386 layout in guest memory. Mutexes and condition
 * variables are mapped to host objects through a side table keyed by guest
 * address, guarded by one host lock. The guest object holds only a Darwin
 * signature word, so a guest pointer never leads to host memory.
 *
 * Attribute objects (pthread_attr_t, pthread_mutexattr_t) hold no host state,
 * so their fields are stored inline in the guest object: word 0 is the Darwin
 * signature, word 1 the stack size or mutex type.
 *
 * Memory ordering: every host pthread call is a full barrier, so guest data
 * guarded by a guest mutex is safe on arm64 without fences in lifted code.
 * OSAtomic*Barrier thunks use seq_cst C11 atomics on guest addresses; the
 * guest address must be 4-byte aligned (arm64 atomics fault otherwise).
 *
 * Guest threads: each pthread_create gets a host thread and a guest stack in
 * the 1 MB slot 0xE0000000 + n * 1 MB. The lowest 16 KB of each slot stays
 * unmapped as a guard. The guest pthread_t is a small integer handle
 * (1, 2, ...), so a guest dereference of it faults in guest page 0. */

/* Darwin i386 object signatures (word 0 of the guest object). */
#define GT_MUTEX_SIG_INIT 0x32AAABA7u            /* PTHREAD_MUTEX_INITIALIZER */
#define GT_ERRORCHECK_MUTEX_SIG_INIT 0x32AAABA1u /* PTHREAD_ERRORCHECK_MUTEX_INITIALIZER */
#define GT_RECURSIVE_MUTEX_SIG_INIT 0x32AAABA2u  /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER */
#define GT_MUTEX_SIG 0x4D555458u                 /* initialised mutex */
#define GT_COND_SIG_INIT 0x3CB0B1BBu             /* PTHREAD_COND_INITIALIZER */
#define GT_COND_SIG 0x434F4E44u                  /* initialised cond */
#define GT_MUTEXATTR_SIG 0x4D545841u
#define GT_ATTR_SIG 0x54484441u

#define GT_STACK_BASE 0xE0000000u
#define GT_STACK_STEP 0x00100000u
#define GT_STACK_GUARD 0x4000u
#define GT_MAX_THREADS 256u
/* Return address pushed under a thread start routine or key destructor. */
#define GT_RET_SENTINEL 0xFFFFF000u

/* Wait until guest thread `handle` has returned from its start routine and
 * its key destructors have run. Stores the start routine's g_eax in *ret.
 * Returns 0, or EINVAL for an unknown handle. Host-side API for tests and
 * shutdown; the game imports no pthread_join. */
int guest_thread_wait(uint32_t handle, uint32_t *ret);

/* Thunks: cdecl, arguments at g_esp + 4, result in g_eax. */
void imp__pthread_attr_init(void);
void imp__pthread_attr_destroy(void);
void imp__pthread_attr_setstacksize(void);
void imp__pthread_mutexattr_init(void);
void imp__pthread_mutexattr_settype(void);
void imp__pthread_mutexattr_destroy_UNIX2003(void);
void imp__pthread_mutex_init(void);
void imp__pthread_mutex_destroy(void);
void imp__pthread_mutex_lock(void);
void imp__pthread_mutex_trylock(void);
void imp__pthread_mutex_unlock(void);
void imp__pthread_cond_init_UNIX2003(void);
void imp__pthread_cond_destroy(void);
void imp__pthread_cond_signal(void);
void imp__pthread_cond_broadcast(void);
void imp__pthread_cond_wait_UNIX2003(void);
void imp__pthread_create(void);
void imp__pthread_setschedparam(void);
void imp__pthread_key_create(void);
void imp__pthread_getspecific(void);
void imp__pthread_setspecific(void);
void imp__OSAtomicAdd32Barrier(void);
void imp__OSAtomicCompareAndSwap32Barrier(void);
void imp__OSAtomicCompareAndSwapPtrBarrier(void);

#endif
