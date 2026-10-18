/* Diagnostics test. Run by tests/test_diag.py as: test_diag <opens|lock|fail|unsupported|caches> [path]. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_threads.h"
#include "host_funcs.h"
#include "handles.h"
#include "diag.h"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

void imp__fopen(void);

#define G_MUTEX 0x00F00000u
#define G_PATH 0x00F00100u
#define G_MODE 0x00F00300u
#define MAIN_TOP 0x00F70000u
#define WORKER_TOP 0x00F60000u

recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

/* Call a thunk the way lifted code does: args at esp+4, return address pushed. */
static uint32_t call(void (*f)(void), int n, const uint32_t *args)
{
    uint32_t sp = g_esp;
    for (int i = n - 1; i >= 0; i--)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, 0x12345678u);
    f();
    g_esp = sp;
    return g_eax;
}

#define CALL(f, ...) call(f, (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

static void put_str(uint32_t addr, const char *s)
{
    memcpy(guest_ptr(addr), s, strlen(s) + 1);
}

static void test_opens(const char *path)
{
    put_str(G_PATH, path);
    put_str(G_MODE, "r");
    CHECK(CALL(imp__fopen, G_PATH, G_MODE) != 0);
    put_str(G_PATH, "/nonexistent-diag-test-path");
    CHECK(CALL(imp__fopen, G_PATH, G_MODE) == 0);
    puts("ok");
}

static void *blocked_locker(void *p)
{
    (void)p;
    g_esp = WORKER_TOP;
    CHECK(CALL(imp__pthread_mutex_lock, G_MUTEX) == 0);
    CHECK(CALL(imp__pthread_mutex_unlock, G_MUTEX) == 0);
    return NULL;
}

static void test_lock(void)
{
    MEM32(G_MUTEX) = GT_MUTEX_SIG_INIT;
    CHECK(CALL(imp__pthread_mutex_lock, G_MUTEX) == 0);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, blocked_locker, NULL) == 0);
    sleep(2); /* RECOMP_LOCKWATCH=1 warns after 1 s */
    CHECK(CALL(imp__pthread_mutex_unlock, G_MUTEX) == 0);
    pthread_join(th, NULL);
    puts("ok");
}

static void test_fail(void)
{
    g_icall_trace[0] = 0x00401000u;
    g_icall_trace[1] = 0x00402000u;
    g_icall_trace_idx = 2;
    PUSH32(g_esp, 0xCAFEBABEu);
    recomp_icall_fail_log(0x00001234u); /* exits with status 3 */
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    CHECK(guest_mem_init() == 0);
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    CHECK(mprotect(guest_ptr(0x00F00000u), 0x80000u, PROT_READ | PROT_WRITE) == 0);
    thunk_files_init();
    g_esp = MAIN_TOP;
    if (strcmp(argv[1], "opens") == 0 && argc > 2)
        test_opens(argv[2]);
    else if (strcmp(argv[1], "lock") == 0)
        test_lock();
    else if (strcmp(argv[1], "fail") == 0)
        test_fail();
    else if (strcmp(argv[1], "unsupported") == 0) {
        diag_unsupported("f", 0x1234u, "value %d", 7);
        diag_unsupported("f", 0x1234u, "value %d", 7);
        diag_unsupported("f", 0x5678u, "value %d", 8);
    } else if (strcmp(argv[1], "caches") == 0)
        printf("caches_off=%d\n", diag_caches_off());
    else
        return 2;
    return 0;
}
