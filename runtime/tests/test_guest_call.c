/* guest_call_cdecl: arguments, alignment, esp restore, and a host thread
 * that attaches its own guest stack. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_call.h"
#include "handles.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define GUEST_SUB 0x00401000u

static uint32_t g_entry_esp_mod16;

/* Guest function: returns arg0 - arg1, records entry alignment. */
static void guest_sub(void)
{
    g_entry_esp_mod16 = g_esp & 15u;
    CHECK(MEM32(g_esp) == GUEST_CALL_RET_SENTINEL);
    g_eax = MEM32(g_esp + 4) - MEM32(g_esp + 8);
}

recomp_func_t recomp_lookup(uint32_t va)
{
    return va == GUEST_SUB ? guest_sub : NULL;
}

static void *host_thread(void *p)
{
    (void)p;
    uint32_t args[2] = {100, 58};
    CHECK(g_esp == 0);
    CHECK(guest_call_cdecl(GUEST_SUB, 2, args) == 42);
    CHECK(g_esp >= GUEST_CB_STACK_BASE && g_esp <= GUEST_CB_STACK_END);
    CHECK(g_entry_esp_mod16 == 12);
    return NULL;
}

#define SEQ_THREADS 600
#define PAR_THREADS 4
#define MAX_DISTINCT 4

static uint32_t g_seq_stack[SEQ_THREADS];
static uint32_t g_seq_errno[SEQ_THREADS];

/* Attach, take an errno slot, record both, exit. */
static void *attach_thread(void *p)
{
    uint32_t *out = p;
    guest_thread_attach();
    CHECK(g_esp >= GUEST_CB_STACK_BASE && g_esp <= GUEST_CB_STACK_END);
    out[0] = (g_esp - 1u) & ~(GUEST_CB_STACK_SIZE - 1u);
    out[1] = guest_errno_slot();
    return NULL;
}

static unsigned count_distinct(const uint32_t *v, unsigned n)
{
    unsigned d = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = 0;
        while (j < i && v[j] != v[i])
            j++;
        if (j == i)
            d++;
    }
    return d;
}

static void test_sequential_threads(void)
{
    for (unsigned i = 0; i < SEQ_THREADS; i++) {
        uint32_t out[2];
        pthread_t t;
        CHECK(pthread_create(&t, NULL, attach_thread, out) == 0);
        CHECK(pthread_join(t, NULL) == 0);
        g_seq_stack[i] = out[0];
        g_seq_errno[i] = out[1];
    }
    CHECK(count_distinct(g_seq_stack, SEQ_THREADS) <= MAX_DISTINCT);
    CHECK(count_distinct(g_seq_errno, SEQ_THREADS) <= MAX_DISTINCT);
}

/* Each thread waits here until all PAR_THREADS have attached, so the stacks
 * and errno slots are held at the same time. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_all_in = PTHREAD_COND_INITIALIZER;
static unsigned g_arrived;

static void *concurrent_thread(void *p)
{
    attach_thread(p);
    pthread_mutex_lock(&g_lock);
    g_arrived++;
    pthread_cond_broadcast(&g_all_in);
    while (g_arrived < PAR_THREADS)
        pthread_cond_wait(&g_all_in, &g_lock);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void test_concurrent_threads(void)
{
    uint32_t out[PAR_THREADS][2];
    uint32_t stacks[PAR_THREADS], slots[PAR_THREADS];
    pthread_t t[PAR_THREADS];
    for (unsigned i = 0; i < PAR_THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, concurrent_thread, out[i]) == 0);
    for (unsigned i = 0; i < PAR_THREADS; i++)
        CHECK(pthread_join(t[i], NULL) == 0);
    for (unsigned i = 0; i < PAR_THREADS; i++) {
        stacks[i] = out[i][0];
        slots[i] = out[i][1];
    }
    CHECK(count_distinct(stacks, PAR_THREADS) == PAR_THREADS);
    CHECK(count_distinct(slots, PAR_THREADS) == PAR_THREADS);
}

int main(void)
{
    CHECK(guest_mem_init() == 0);
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    thunk_files_init();
    CHECK(mprotect(guest_ptr(0x00F00000u), 0x00080000u, PROT_READ | PROT_WRITE) == 0);

    g_esp = 0x00F7FFF4u; /* misaligned on purpose */
    uint32_t saved = g_esp;
    uint32_t args[2] = {7, 3};
    CHECK(guest_call_cdecl(GUEST_SUB, 2, args) == 4);
    CHECK(g_esp == saved);
    CHECK(g_entry_esp_mod16 == 12);

    pthread_t t;
    CHECK(pthread_create(&t, NULL, host_thread, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    test_sequential_threads();
    test_concurrent_threads();
    puts("ok");
    return 0;
}
