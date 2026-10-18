#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>
#include <stdint.h>

/* Run-time diagnostics, all off unless an environment variable is set.
 *
 *   RECOMP_TRACE_OPENS=1  thunk_files.c logs "OPEN <path> <op> -> ok|errno=N"
 *                         for fopen, opendir, stat, mkdir and chdir.
 *   RECOMP_HANGWATCH=N    every N seconds, one line per registered guest
 *                         thread (host thread id, g_esp) plus the last icall
 *                         targets; "HANG?" when a thread's g_esp has not
 *                         changed over 3 ticks.
 *   RECOMP_LOCKWATCH=N    pthread_mutex_lock polls and prints
 *                         "STUCK-LOCK guest_mutex=0x.. owner_thread=.." once
 *                         after N seconds, then keeps waiting.
 *   RECOMP_SLOWFRAME_MS=N  slowframe.c prints the main thread's sampled stacks for each frame over N ms.
 *
 * Failure logs in recomp_glue.c always print a "RECOMP:" line with the guest
 * return address, g_esp and the last 8 icall targets. */

/* Call after data_imports_bind_all() on the main thread. Registers the
 * calling thread and starts the hangwatch thread when RECOMP_HANGWATCH is
 * set. */
void diag_init(void);

/* Register the calling guest thread (its g_esp is read live by hangwatch).
 * thunk_threads.c calls both from guest_thread_main; they are weak
 * references there, so a test binary without diag.c still links. */
void diag_thread_register(void);
void diag_thread_unregister(void);

/* Host thread id of the caller, as printed by hangwatch and lockwatch. */
uint64_t diag_host_tid(void);

/* Write "0x.. 0x.. ..." for the last n (<= ICALL_TRACE_SIZE) icall targets,
 * newest first, into buf. Defined in recomp_glue.c. */
void recomp_icall_tail(char *buf, size_t size, unsigned n);

#endif
