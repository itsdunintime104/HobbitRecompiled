/* Heap guard test: one case per run, chosen by argv[1] (ok, overrun, uaf,
 * uaf_evict, double, off). With RECOMP_HEAP_GUARD=1 the failing cases must
 * abort inside the guard; with =2 they log once and exit 0; "ok" and "off"
 * print "ok" and exit 0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"

#define ALLOC_SITE 0x1000u
#define FREE_SITE 0x2000u

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

static int ok_bytes(uint32_t a, uint32_t n, uint8_t seed)
{
    const uint8_t *p = guest_ptr(a);
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != (uint8_t)(seed + i))
            return 0;
    }
    return 1;
}

static void case_ok(void)
{
    uint32_t a = guest_malloc(100);
    check(a != 0 && a % 16 == 0, "malloc aligned");
    for (uint32_t i = 0; i < 100; i++)
        ((uint8_t *)guest_ptr(a))[i] = (uint8_t)(7 + i);
    uint32_t b = guest_realloc(a, 300);
    check(b != 0 && b % 16 == 0 && ok_bytes(b, 100, 7), "realloc grow keeps data");
    uint32_t c = guest_realloc(b, 50);
    check(c != 0 && c % 16 == 0 && ok_bytes(c, 50, 7), "realloc shrink keeps data");
    uint32_t z = guest_calloc(25, 4);
    check(z != 0 && z % 16 == 0, "calloc aligned");
    for (uint32_t i = 0; i < 100; i++)
        check(((uint8_t *)guest_ptr(z))[i] == 0, "calloc zeroes");
    check(guest_calloc(0x10000, 0x10000) == 0, "calloc overflow returns 0");
    uint32_t r = guest_realloc(0, 0);
    check(r != 0 && r % 16 == 0, "realloc(0, 0) allocates");
    uint32_t m = guest_malloc(0);
    check(m != 0 && m % 16 == 0, "malloc(0) allocates");
    guest_free(c);
    guest_free(z);
    guest_free(r);
    guest_free(m);
    guest_free(0);
}

/* Each bad action runs twice with the same sites: abort mode stops at the
 * first, continue mode must report it once. */
static void case_overrun(void)
{
    for (int k = 0; k < 2; k++) {
        g_heap_site = ALLOC_SITE;
        uint32_t a = guest_malloc(20);
        g_heap_site = FREE_SITE;
        check(a != 0, "malloc");
        ((uint8_t *)guest_ptr(a))[20] = 0x55;
        guest_free(a);
    }
}

static void case_uaf(void)
{
    g_heap_site = ALLOC_SITE;
    uint32_t a = guest_malloc(20);
    g_heap_site = FREE_SITE;
    check(a != 0, "malloc");
    guest_free(a);
    ((uint8_t *)guest_ptr(a))[8] = 0x66;
    sleep(2);
}

/* Free two victims, write into both, then free enough small blocks to push
 * them out of the quarantine (65536 entries), which checks them. */
static void case_uaf_evict(void)
{
    uint32_t v[2];
    for (int k = 0; k < 2; k++) {
        g_heap_site = ALLOC_SITE;
        v[k] = guest_malloc(20);
        g_heap_site = FREE_SITE;
        check(v[k] != 0, "malloc");
        guest_free(v[k]);
        ((uint8_t *)guest_ptr(v[k]))[8] = 0x66;
    }
    for (uint32_t k = 0; k < 65536 + 2; k++)
        guest_free(guest_malloc(1));
}

static void case_double(void)
{
    for (int k = 0; k < 2; k++) {
        g_heap_site = ALLOC_SITE;
        uint32_t a = guest_malloc(20);
        g_heap_site = FREE_SITE;
        check(a != 0, "malloc");
        guest_free(a);
        guest_free(a);
    }
}

int main(int argc, char **argv)
{
    check(argc == 2, "usage: test_heap_guard <case>");
    check(guest_mem_init() == 0, "guest_mem_init");
    check(guest_heap_init() == 0, "guest_heap_init");
    const char *c = argv[1];
    if (strcmp(c, "ok") == 0)
        case_ok();
    else if (strcmp(c, "overrun") == 0 || strcmp(c, "off") == 0)
        case_overrun();
    else if (strcmp(c, "uaf") == 0)
        case_uaf();
    else if (strcmp(c, "uaf_evict") == 0)
        case_uaf_evict();
    else if (strcmp(c, "double") == 0)
        case_double();
    else
        check(0, "unknown case");
    puts("ok");
    return 0;
}
