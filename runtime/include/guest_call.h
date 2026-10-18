/* Calls from host code into guest functions: callbacks such as CF applier
 * functions, audio render callbacks and qsort comparators.
 *
 * Guest address map above the heap:
 *   0xE0000000-0xF0000000  pthread_create stacks (thunk_threads.c)
 *   0xF0000000-0xFD000000  stacks for host threads that call into the guest
 *   0xFD000000-0xFE000000  main guest stack (startup.c)
 *   0xFF000000-            handle values, never mapped (handles.h)
 */
#ifndef GUEST_CALL_H
#define GUEST_CALL_H

#include <stdint.h>

#define GUEST_CB_STACK_BASE 0xF0000000u
#define GUEST_CB_STACK_END 0xFD000000u
#define GUEST_CB_STACK_SIZE 0x00100000u
#define GUEST_CALL_RET_SENTINEL 0xFFFFF000u

/* Give this host thread a guest stack and reset FPU state, unless it already
 * has one (g_esp != 0). Audio and other host-created threads call this first;
 * guest_call_cdecl calls it on demand. Aborts when the region is full. */
void guest_thread_attach(void);

/* Call the cdecl guest function at va with n 32-bit arguments, on this
 * thread's guest stack, below the current g_esp. Entry esp is 12 mod 16, as
 * the i386 Darwin ABI requires. g_esp is restored afterwards. Returns g_eax;
 * g_edx and the x87 stack hold any wider result. */
uint32_t guest_call_cdecl(uint32_t va, int n, const uint32_t *args);

#endif
