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
 *   RECOMP_NO_CACHE=1     the audio graph pool and the Cg compile cache are off:
 *                         every graph is disposed and every program compiled.
 *                         For an A/B when a reused object is suspected.
 *
 * Failure logs in recomp_glue.c always print a "RECOMP:" line with the guest
 * return address, g_esp and the last 8 icall targets.
 *
 * UNSUPPORTED lines (always on, recomp_glue.c): a thunk met a guest value that
 * the real API handles but the thunk does not (an unknown property, command
 * or format, or a full fixed-size table). Each distinct message prints once
 * with the guest return address. Host-side code with no guest frame calls
 * diag_unsupported(__func__, 0, ...). */

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

/* Print "UNSUPPORTED <func>: <message> (guest return 0x%08X)" once per
 * distinct func + message. Defined in recomp_glue.c. */
void diag_unsupported(const char *func, uint32_t guest_ret, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* 1 when RECOMP_NO_CACHE=1, read once. Defined in recomp_glue.c. */
int diag_caches_off(void);

/* In a thunk, before RET: reports with the guest return address. */
#define THUNK_UNSUPPORTED(...) diag_unsupported(__func__, MEM32(g_esp), __VA_ARGS__)

#endif
