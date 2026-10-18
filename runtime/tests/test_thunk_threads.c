/* Thunk test: pthread and OSAtomic. Run by tests/test_thunk_threads.py. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_threads.h"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Guest layout inside the mapped 0x00F00000-0x01000000 window. */
#define G_M1 0x00F00000u      /* statically initialised mutex */
#define G_COUNTER 0x00F00040u
#define G_COND 0x00F00080u
#define G_M2 0x00F000C0u
#define G_FLAG 0x00F00100u
#define G_MATTR 0x00F00140u
#define G_M3 0x00F00180u      /* recursive mutex */
#define G_KEY 0x00F001C0u
#define G_THREAD 0x00F00200u
#define G_CELL 0x00F00240u
#define G_CELL2 0x00F00280u
#define G_ATTR 0x00F002C0u
#define G_M4 0x00F00300u      /* copy of a recursive mutex, no host entry */
#define G_M5 0x00F00340u      /* plain mutex */
#define G_M6 0x00F00380u      /* copy of the plain mutex, no host entry */
#define STACK_TOP(i) (0x00F80000u + (uint32_t)(i) * 0x10000u)
#define MAIN_TOP 0x01000000u

#define VA_START 0x00401000u
#define VA_DTOR 0x00401100u
#define N_WORKERS 4
#define N_ITERS 100000u

/* Call a thunk the way lifted code does: args at esp+4, then the return
 * address push. Checks the ret and the callee-saved registers. */
static uint32_t call(void (*f)(void), int n, const uint32_t *args)
{
    uint32_t sp = g_esp;
    for (int i = n - 1; i >= 0; i--)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, 0x12345678u);
    uint32_t before = g_esp;
    g_ebx = 0xB0B0B0B0u;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ebp = 0xEBEBEBEBu;
    f();
    CHECK(g_esp == before + 4);
    CHECK(g_ebx == 0xB0B0B0B0u && g_esi == 0x51515151u);
    CHECK(g_edi == 0xD1D1D1D1u && g_ebp == 0xEBEBEBEBu);
    g_esp = sp;
    return g_eax;
}

#define CALL(f, ...) call(f, (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})
#define CALL0(f) call(f, 0, NULL)

/* ---- Minimal recomp_lookup: two guest functions ------------------------ */

static uint32_t g_seen_arg, g_seen_dtor_arg, g_start_esp_ok;

static void guest_start(void)
{
    g_start_esp_ok = MEM32(g_esp) == GT_RET_SENTINEL && (g_esp & 15u) == 12u &&
                     g_esp >= GT_STACK_BASE;
    g_seen_arg = MEM32(g_esp + 4);
    uint32_t k = MEM32(G_KEY);
    CHECK(CALL(imp__pthread_getspecific, k) == 0); /* fresh thread */
    CHECK(CALL(imp__pthread_setspecific, k, 0x55u) == 0);
    g_eax = g_seen_arg ^ 0xA5A5u;
    g_esp += 4;
}

static void guest_dtor(void)
{
    g_seen_dtor_arg = MEM32(g_esp + 4);
    g_esp += 4;
}

recomp_func_t recomp_lookup(uint32_t va)
{
    if (va == VA_START)
        return guest_start;
    if (va == VA_DTOR)
        return guest_dtor;
    return NULL;
}

/* ---- Tests -------------------------------------------------------------- */

static void *counter_worker(void *p)
{
    g_esp = STACK_TOP((uintptr_t)p);
    for (uint32_t i = 0; i < N_ITERS; i++) {
        CHECK(CALL(imp__pthread_mutex_lock, G_M1) == 0);
        MEM32(G_COUNTER) = MEM32(G_COUNTER) + 1;
        CHECK(CALL(imp__pthread_mutex_unlock, G_M1) == 0);
    }
    return NULL;
}

static void test_mutex(void)
{
    MEM32(G_M1) = GT_MUTEX_SIG_INIT;
    MEM32(G_COUNTER) = 0;
    pthread_t th[N_WORKERS];
    for (uintptr_t i = 0; i < N_WORKERS; i++)
        CHECK(pthread_create(&th[i], NULL, counter_worker, (void *)i) == 0);
    for (int i = 0; i < N_WORKERS; i++)
        pthread_join(th[i], NULL);
    CHECK(MEM32(G_COUNTER) == N_WORKERS * N_ITERS);
    CHECK(MEM32(G_M1) == GT_MUTEX_SIG);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M1) == 0);
    CHECK(CALL(imp__pthread_mutex_lock, G_M1) == EINVAL);

    /* Memory reused: a static initializer over a live (locked) entry gives a
     * fresh unlocked mutex, of the type the new initializer means. */
    MEM32(G_M1) = GT_MUTEX_SIG_INIT;
    CHECK(CALL(imp__pthread_mutex_lock, G_M1) == 0);
    MEM32(G_M1) = GT_MUTEX_SIG_INIT;
    CHECK(CALL(imp__pthread_mutex_trylock, G_M1) == 0);
    CHECK(MEM32(G_M1) == GT_MUTEX_SIG);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M1) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M1) == 0);
    MEM32(G_M1) = GT_MUTEX_SIG_INIT;
    CHECK(CALL(imp__pthread_mutex_lock, G_M1) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M1) == 0);
    MEM32(G_M1) = GT_RECURSIVE_MUTEX_SIG_INIT;
    CHECK(CALL(imp__pthread_mutex_lock, G_M1) == 0);
    CHECK(CALL(imp__pthread_mutex_trylock, G_M1) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M1) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M1) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M1) == 0);

    /* Type recorded by mutexattr_settype reaches the host mutex. */
    CHECK(CALL(imp__pthread_mutexattr_init, G_MATTR) == 0);
    CHECK(CALL(imp__pthread_mutexattr_settype, G_MATTR, PTHREAD_MUTEX_RECURSIVE) == 0);
    CHECK(CALL(imp__pthread_mutex_init, G_M3, G_MATTR) == 0);
    CHECK(CALL(imp__pthread_mutexattr_destroy_UNIX2003, G_MATTR) == 0);
    CHECK(CALL(imp__pthread_mutex_lock, G_M3) == 0);
    CHECK(CALL(imp__pthread_mutex_trylock, G_M3) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M3) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M3) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M3) == 0);

    /* A struct copy of an initialised mutex has no host entry; the rebuilt
     * entry keeps the type recorded in the guest bytes. */
    CHECK(CALL(imp__pthread_mutexattr_init, G_MATTR) == 0);
    CHECK(CALL(imp__pthread_mutexattr_settype, G_MATTR, PTHREAD_MUTEX_RECURSIVE) == 0);
    CHECK(CALL(imp__pthread_mutex_init, G_M3, G_MATTR) == 0);
    CHECK(CALL(imp__pthread_mutexattr_destroy_UNIX2003, G_MATTR) == 0);
    memcpy(guest_ptr(G_M4), guest_ptr(G_M3), 44);
    CHECK(CALL(imp__pthread_mutex_lock, G_M4) == 0);
    CHECK(CALL(imp__pthread_mutex_trylock, G_M4) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M4) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M4) == 0);
    CHECK(CALL(imp__pthread_mutex_init, G_M5, 0) == 0);
    memcpy(guest_ptr(G_M6), guest_ptr(G_M5), 44);
    CHECK(CALL(imp__pthread_mutex_lock, G_M6) == 0);
    CHECK(CALL(imp__pthread_mutex_trylock, G_M6) == EBUSY);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M6) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M3) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M4) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M5) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M6) == 0);
    printf("mutex ok: counter=%u\n", MEM32(G_COUNTER));
}

static void *signaller(void *p)
{
    (void)p;
    g_esp = STACK_TOP(N_WORKERS);
    CHECK(CALL(imp__pthread_mutex_lock, G_M2) == 0);
    MEM32(G_FLAG) = 1;
    CHECK(CALL(imp__pthread_cond_signal, G_COND) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_M2) == 0);
    return NULL;
}

static void test_cond(void)
{
    MEM32(G_FLAG) = 0;
    CHECK(CALL(imp__pthread_mutex_init, G_M2, 0) == 0);
    CHECK(CALL(imp__pthread_cond_init_UNIX2003, G_COND, 0) == 0);
    CHECK(CALL(imp__pthread_mutex_lock, G_M2) == 0);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, signaller, NULL) == 0);
    int waits = 0;
    while (MEM32(G_FLAG) == 0) {
        CHECK(CALL(imp__pthread_cond_wait_UNIX2003, G_COND, G_M2) == 0);
        waits++;
    }
    CHECK(CALL(imp__pthread_mutex_unlock, G_M2) == 0);
    pthread_join(th, NULL);
    /* Memory reused: a static initializer over a live entry. */
    MEM32(G_COND) = GT_COND_SIG_INIT;
    CHECK(CALL(imp__pthread_cond_signal, G_COND) == 0);
    CHECK(MEM32(G_COND) == GT_COND_SIG);
    MEM32(G_COND) = GT_COND_SIG_INIT;
    CHECK(CALL(imp__pthread_cond_broadcast, G_COND) == 0);
    CHECK(MEM32(G_COND) == GT_COND_SIG);
    CHECK(CALL(imp__pthread_cond_destroy, G_COND) == 0);
    CHECK(CALL(imp__pthread_mutex_destroy, G_M2) == 0);
    printf("cond ok: waits=%d\n", waits);
}

static void test_create(void)
{
    CHECK(CALL(imp__pthread_key_create, G_KEY, VA_DTOR) == 0);
    uint32_t k = MEM32(G_KEY);
    CHECK(CALL(imp__pthread_setspecific, k, 0x1234u) == 0);
    CHECK(CALL(imp__pthread_getspecific, k) == 0x1234u);

    CHECK(CALL(imp__pthread_attr_init, G_ATTR) == 0);
    CHECK(CALL(imp__pthread_attr_setstacksize, G_ATTR, 0x200000u) == 0);
    CHECK(CALL(imp__pthread_create, G_THREAD, G_ATTR, VA_START, 0xC0FFEEu) == 0);
    CHECK(CALL(imp__pthread_attr_destroy, G_ATTR) == 0);
    uint32_t h = MEM32(G_THREAD);
    CHECK(CALL(imp__pthread_setschedparam, h, 0, 0) == 0);
    uint32_t ret = 0;
    CHECK(guest_thread_wait(h, &ret) == 0);
    CHECK(g_start_esp_ok);
    CHECK(g_seen_arg == 0xC0FFEEu);
    CHECK(ret == (0xC0FFEEu ^ 0xA5A5u));
    CHECK(g_seen_dtor_arg == 0x55u);
    CHECK(CALL(imp__pthread_getspecific, k) == 0x1234u); /* main thread unchanged */
    printf("create ok: handle=%u ret=0x%X dtor_arg=0x%X\n", h, ret, g_seen_dtor_arg);
}

static void test_atomics(void)
{
    MEM32(G_CELL) = 5;
    CHECK(CALL(imp__OSAtomicCompareAndSwap32Barrier, 5, 7, G_CELL) == 1);
    CHECK(MEM32(G_CELL) == 7);
    CHECK(CALL(imp__OSAtomicCompareAndSwap32Barrier, 5, 9, G_CELL) == 0);
    CHECK(MEM32(G_CELL) == 7);
    CHECK(CALL(imp__OSAtomicAdd32Barrier, 3, G_CELL) == 10);
    CHECK(CALL(imp__OSAtomicAdd32Barrier, (uint32_t)-10, G_CELL) == 0);
    CHECK(MEM32(G_CELL) == 0);
    MEM32(G_CELL2) = 0;
    CHECK(CALL(imp__OSAtomicCompareAndSwapPtrBarrier, 0, 0x00F01234u, G_CELL2) == 1);
    CHECK(MEM32(G_CELL2) == 0x00F01234u);
    printf("atomics ok\n");
}

int main(void)
{
    CHECK(guest_mem_init() == 0);
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    CHECK(mprotect(guest_ptr(0x00F00000u), 0x00100000u, PROT_READ | PROT_WRITE) == 0);
    g_esp = MAIN_TOP;
    test_mutex();
    test_cond();
    test_create();
    test_atomics();
    puts("all ok");
    return 0;
}
