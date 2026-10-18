#include "guest_mem.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>

#define WINDOW_SIZE (1ull << 32)
#define GUARD_SIZE (64ull * 1024)

uint8_t *g_guest_base;

static sigjmp_buf g_probe_env;
static volatile sig_atomic_t g_probe_armed;

int guest_mem_init(void)
{
    if (g_guest_base != NULL)
        return 0;

    /* Reserve enough to hold a 4 GB aligned window plus the guard wherever
     * the kernel places the mapping. */
    size_t total = (size_t)(2 * WINDOW_SIZE + GUARD_SIZE);
    uint8_t *raw = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) {
        perror("mmap reserve");
        return -1;
    }

    uintptr_t start = (uintptr_t)raw;
    uintptr_t aligned = (start + (WINDOW_SIZE - 1)) & ~(uintptr_t)(WINDOW_SIZE - 1);
    uintptr_t window_end = aligned + WINDOW_SIZE + GUARD_SIZE;
    uintptr_t raw_end = start + total;

    if (aligned > start && munmap(raw, aligned - start) != 0) {
        perror("munmap head");
        return -1;
    }
    if (raw_end > window_end && munmap((void *)window_end, raw_end - window_end) != 0) {
        perror("munmap tail");
        return -1;
    }

    g_guest_base = (uint8_t *)aligned;
    return 0;
}

static void fault_handler(int sig)
{
    (void)sig;
    if (g_probe_armed)
        siglongjmp(g_probe_env, 1);
    signal(sig, SIG_DFL);
}

int guest_mem_probe(uint32_t a, uint8_t *out)
{
    struct sigaction sa;
    struct sigaction old_segv;
    struct sigaction old_bus;
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    int rc = 0;
    g_probe_armed = 1;
    if (sigsetjmp(g_probe_env, 1) == 0)
        *out = *(volatile uint8_t *)guest_ptr(a);
    else
        rc = -1;
    g_probe_armed = 0;

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return rc;
}

void guest_mem_dump(uint32_t a, uint32_t n)
{
    const volatile uint8_t *p = guest_ptr(a);
    for (uint32_t i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}
