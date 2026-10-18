/* Five checks for the auto-generated thunks in runtime/src/thunk_auto.c.
 * Each call sets up a guest stack, runs the thunk, then checks the result,
 * that esp went up by 4 and that ebx, esi, edi and ebp are unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "guest_mem.h"
#include "recomp_types.h"

extern void imp__strlen(void);
extern void imp__memcpy(void);
extern void imp__strchr(void);
extern void imp__ldexp(void);
extern void imp__glEnable(void);

#define WINDOW_LO 0x00F00000u
#define WINDOW_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define STR_A 0x00F10000u
#define BUF_B 0x00F20000u

static int failures;

#define CHECK(name, cond) \
    do { \
        int ok_ = (cond); \
        printf("%s: %s\n", ok_ ? "PASS" : "FAIL", name); \
        if (!ok_) \
            failures++; \
    } while (0)

/* Call thunk with the given 32-bit argument words (a 64-bit argument takes two
 * words, low first). Returns 1 if the register contract held. */
static int call(void (*thunk)(void), const uint32_t *words, int n)
{
    g_ebx = 0x11111111u;
    g_esi = 0x22222222u;
    g_edi = 0x33333333u;
    g_ebp = 0x44444444u;
    g_esp = STACK_TOP;
    for (int i = 0; i < n; i++)
        MEM32(g_esp + 4 + 4 * i) = words[i];
    MEM32(g_esp) = 0xDEAD0000u; /* dummy return address */
    thunk();
    return g_esp == STACK_TOP + 4 && g_ebx == 0x11111111u && g_esi == 0x22222222u &&
           g_edi == 0x33333333u && g_ebp == 0x44444444u;
}

static void put_string(uint32_t addr, const char *s)
{
    memcpy(guest_ptr(addr), s, strlen(s) + 1);
}

int main(void)
{
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    if (mprotect(guest_ptr(WINDOW_LO), WINDOW_HI - WINDOW_LO, PROT_READ | PROT_WRITE) != 0)
        return 2;

    put_string(STR_A, "hello");

    /* 1: strlen */
    uint32_t w1[] = {STR_A};
    int ok = call(imp__strlen, w1, 1);
    CHECK("strlen", ok && g_eax == 5);

    /* 2: memcpy returns the destination as a guest address */
    uint32_t w2[] = {BUF_B, STR_A, 6};
    ok = call(imp__memcpy, w2, 3);
    CHECK("memcpy", ok && g_eax == BUF_B && memcmp(guest_ptr(BUF_B), "hello", 6) == 0);

    /* 3: strchr finds a guest address, and returns 0 for NULL */
    uint32_t w3[] = {STR_A, 'l'};
    ok = call(imp__strchr, w3, 2);
    int found = ok && g_eax == STR_A + 2;
    uint32_t w3b[] = {STR_A, 'z'};
    ok = call(imp__strchr, w3b, 2);
    CHECK("strchr", found && ok && g_eax == 0);

    /* 4: ldexp(3.0, 4) = 48.0 is pushed onto the x87 stack */
    double x = 3.0;
    uint32_t w4[3];
    memcpy(w4, &x, 8);
    w4[2] = 4;
    g_fp_top = 0;
    ok = call(imp__ldexp, w4, 3);
    CHECK("ldexp", ok && g_fp_top == 7 && g_fp_stack[g_fp_top] == 48.0);

    /* 5: a GL thunk links (no context, so it is not called) */
    void (*gl)(void) = imp__glEnable;
    CHECK("glEnable compiled", gl != NULL);

    return failures ? 1 : 0;
}
