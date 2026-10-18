/* Heap thunk test: random guest_* calls, then malloc/free/strdup/new/delete
 * through the thunks with a guest stack. Prints "ok" and exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define LIVE 64
#define OPS 1000

void imp__malloc(void);
void imp__free(void);
void imp__strdup(void);
void imp___Znwm(void);
void imp___Znam(void);
void imp___ZdlPv(void);
void imp___ZdaPv(void);

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

static uint32_t g_rng = 12345;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static void check_block(uint32_t a, uint32_t n)
{
    check(a >= GUEST_HEAP_BASE && a + n <= GUEST_HEAP_END, "pointer inside heap range");
    check(a % 16 == 0, "pointer 16-aligned");
}

static void fill(uint32_t a, uint32_t n, uint8_t seed)
{
    uint8_t *p = guest_ptr(a);
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i);
}

static void verify(uint32_t a, uint32_t n, uint8_t seed)
{
    const uint8_t *p = guest_ptr(a);
    for (uint32_t i = 0; i < n; i++)
        check(p[i] == (uint8_t)(seed + i), "contents preserved");
}

static void test_random(void)
{
    uint32_t addr[LIVE] = {0};
    uint32_t size[LIVE] = {0};
    uint8_t seed[LIVE] = {0};

    for (int op = 0; op < OPS; op++) {
        int i = (int)(rnd() % LIVE);
        uint32_t n = 1 + rnd() % (rnd() % 8 == 0 ? 200000 : 600);
        if (addr[i] == 0) {
            addr[i] = guest_malloc(n);
            check(addr[i] != 0, "malloc returned a block");
            size[i] = n;
        } else if (rnd() % 2) {
            verify(addr[i], size[i], seed[i]);
            uint32_t keep = n < size[i] ? n : size[i];
            uint32_t a = guest_realloc(addr[i], n);
            check(a != 0, "realloc returned a block");
            check_block(a, n);
            verify(a, keep, seed[i]);
            addr[i] = a;
            size[i] = n;
        } else {
            verify(addr[i], size[i], seed[i]);
            guest_free(addr[i]);
            addr[i] = 0;
            continue;
        }
        check_block(addr[i], size[i]);
        seed[i] = (uint8_t)rnd();
        fill(addr[i], size[i], seed[i]);
    }
    for (int i = 0; i < LIVE; i++) {
        if (addr[i] != 0) {
            verify(addr[i], size[i], seed[i]);
            guest_free(addr[i]);
        }
    }

    uint32_t c = guest_calloc(100, 4);
    check(c != 0, "calloc returned a block");
    check_block(c, 400);
    for (int i = 0; i < 400; i++)
        check(((uint8_t *)guest_ptr(c))[i] == 0, "calloc zeroes");
    guest_free(c);
    check(guest_realloc(0, 32) != 0, "realloc(0, n) allocates");
    check(guest_malloc(0xFFFFFFFFu) == 0, "oversize malloc fails");
}

/* Call a thunk with up to one argument; returns eax. Checks the stack
 * protocol and the registers a thunk must preserve. */
static uint32_t call_thunk(void (*fn)(void), uint32_t arg)
{
    uint32_t sp = STACK_HI - 64;
    MEM32(sp + 4) = arg;
    MEM32(sp) = 0xDEADBEEF;
    g_esp = sp;
    g_ebx = 0x11111111;
    g_esi = 0x22222222;
    g_edi = 0x33333333;
    g_ebp = 0x44444444;
    fn();
    check(g_esp == sp + 4, "esp advanced by 4");
    check(g_ebx == 0x11111111 && g_esi == 0x22222222, "ebx/esi preserved");
    check(g_edi == 0x33333333 && g_ebp == 0x44444444, "edi/ebp preserved");
    return g_eax;
}

static void test_thunks(void)
{
    uint32_t p = call_thunk(imp__malloc, 100);
    check(p != 0, "imp__malloc");
    check_block(p, 100);
    memset(guest_ptr(p), 0xAB, 100);
    call_thunk(imp__free, p);

    uint32_t s = guest_malloc(32);
    strcpy(guest_ptr(s), "hobbit");
    uint32_t d = call_thunk(imp__strdup, s);
    check(d != 0 && d != s, "strdup returns a new block");
    check_block(d, 7);
    check(strcmp(guest_ptr(d), "hobbit") == 0, "strdup copies");
    strcpy(guest_ptr(s), "changed");
    check(strcmp(guest_ptr(d), "hobbit") == 0, "strdup copy is independent");
    call_thunk(imp__free, d);
    guest_free(s);

    uint32_t n1 = call_thunk(imp___Znwm, 48);
    uint32_t n2 = call_thunk(imp___Znam, 64);
    check(n1 != 0 && n2 != 0 && n1 != n2, "new / new[]");
    check_block(n1, 48);
    check_block(n2, 64);
    memset(guest_ptr(n1), 1, 48);
    memset(guest_ptr(n2), 2, 64);
    call_thunk(imp___ZdlPv, n1);
    call_thunk(imp___ZdaPv, n2);
    uint32_t n3 = call_thunk(imp___Znwm, 48);
    check(n3 != 0, "new after delete");
    call_thunk(imp___ZdlPv, n3);
}

int main(void)
{
    check(guest_mem_init() == 0, "guest_mem_init");
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    check(mprotect(guest_ptr(STACK_LO), STACK_HI - STACK_LO, PROT_READ | PROT_WRITE) == 0,
          "map stack");
    check(guest_heap_init() == 0, "guest_heap_init");
    check(guest_heap_init() == 0, "guest_heap_init is repeatable");
    test_random();
    test_thunks();
    puts("ok");
    return 0;
}
