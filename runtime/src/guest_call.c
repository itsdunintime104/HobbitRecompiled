/* Calls from host code into guest functions. See guest_call.h. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_call.h"
#include "host_funcs.h"

#define CB_STACK_COUNT ((GUEST_CB_STACK_END - GUEST_CB_STACK_BASE) / GUEST_CB_STACK_SIZE)

/* Stacks are handed out by bumping g_next_cb_stack. A stack whose thread has
 * exited goes on the free list and is reused before the bump pointer moves. */
static uint32_t g_next_cb_stack = GUEST_CB_STACK_BASE;
static uint32_t g_free_stacks[CB_STACK_COUNT];
static unsigned g_free_count;
static pthread_mutex_t g_stack_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_stack_key;
static pthread_once_t g_stack_key_once = PTHREAD_ONCE_INIT;

/* Key destructor: the thread is exiting, so only return the stack. The value
 * is base + 1 so it is never NULL. */
static void release_stack(void *value)
{
    uint32_t base = (uint32_t)(uintptr_t)value - 1u;
    pthread_mutex_lock(&g_stack_lock);
    if (g_free_count < CB_STACK_COUNT)
        g_free_stacks[g_free_count++] = base;
    pthread_mutex_unlock(&g_stack_lock);
}

static void make_stack_key(void)
{
    if (pthread_key_create(&g_stack_key, release_stack) != 0) {
        perror("guest_thread_attach: pthread_key_create");
        abort();
    }
}

void guest_thread_attach(void)
{
    if (g_esp != 0)
        return;
    pthread_once(&g_stack_key_once, make_stack_key);

    uint32_t base;
    int fresh = 0;
    pthread_mutex_lock(&g_stack_lock);
    if (g_free_count > 0) {
        base = g_free_stacks[--g_free_count];
    } else if (g_next_cb_stack < GUEST_CB_STACK_END) {
        base = g_next_cb_stack;
        g_next_cb_stack += GUEST_CB_STACK_SIZE;
        fresh = 1;
    } else {
        pthread_mutex_unlock(&g_stack_lock);
        fprintf(stderr, "guest_thread_attach: callback stack region full\n");
        abort();
    }
    pthread_mutex_unlock(&g_stack_lock);

    if (fresh && mprotect(guest_ptr(base), GUEST_CB_STACK_SIZE, PROT_READ | PROT_WRITE) != 0) {
        perror("guest_thread_attach: mprotect");
        abort();
    }
    pthread_setspecific(g_stack_key, (void *)(uintptr_t)(base + 1u));
    g_esp = base + GUEST_CB_STACK_SIZE;
    g_fp_control_word = 0x037F;
    g_mxcsr = 0x1F80;
}

uint32_t guest_call_cdecl(uint32_t va, int n, const uint32_t *args)
{
    guest_thread_attach();
    recomp_func_t fn = guest_resolve(va);
    if (fn == NULL)
        recomp_icall_fail_log(va);

    uint32_t saved = g_esp;
    uint32_t base = (saved - 4u * (uint32_t)n) & ~15u;
    for (int i = 0; i < n; i++)
        MEM32(base + 4u * (uint32_t)i) = args[i];
    g_esp = base;
    PUSH32(g_esp, GUEST_CALL_RET_SENTINEL);
    fn();
    g_esp = saved;
    return g_eax;
}
