/* Unit test for the IOKit, keyboard-layout and socket thunks.
 * Prints "ok" and exits 0 when every check passes. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "guest_mem.h"
#include "handles.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define RET_MAGIC 0xDEADBEEFu

void imp__IOIteratorNext(void);
void imp__UCKeyTranslate(void);
void imp__socket(void);

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
            g_failures++; \
        } \
    } while (0)

/* Call a thunk with the given argument words and check the protocol: esp up
 * by 4, callee-saved registers unchanged. */
static uint32_t call(void (*fn)(void), const uint32_t *args, int nargs)
{
    uint32_t sp = STACK_TOP - 4u * (uint32_t)(nargs + 1);
    MEM32(sp) = RET_MAGIC;
    for (int i = 0; i < nargs; i++)
        MEM32(sp + 4 + 4 * (uint32_t)i) = args[i];
    g_esp = sp;
    g_ebx = 0x11111111;
    g_esi = 0x22222222;
    g_edi = 0x33333333;
    g_ebp = 0x44444444;
    fn();
    CHECK(g_esp == sp + 4, "esp %x, want %x", g_esp, sp + 4);
    CHECK(g_ebx == 0x11111111 && g_esi == 0x22222222 && g_edi == 0x33333333 &&
              g_ebp == 0x44444444,
          "callee-saved register changed");
    return g_eax;
}

static void test_iokit(void)
{
    uint32_t args[] = {0x1234};
    CHECK(call(imp__IOIteratorNext, args, 1) == 0, "IOIteratorNext != 0");
}

static void test_socket(void)
{
    MEM32(guest_errno_slot()) = 0;
    uint32_t args[] = {2, 1, 0};
    CHECK((int)call(imp__socket, args, 3) == -1, "socket != -1");
    CHECK(MEM32(guest_errno_slot()) == ENETDOWN, "errno %u, want ENETDOWN", MEM32(guest_errno_slot()));
}

static void test_keyboard(void)
{
    MEM32(SCRATCH + 0x40) = 7;
    uint32_t args[] = {0, 0, 0, 0, 0, 0, 0, 4, SCRATCH + 0x40, SCRATCH + 0x50};
    CHECK((int)call(imp__UCKeyTranslate, args, 10) == -1, "UCKeyTranslate != -1");
    CHECK(MEM32(SCRATCH + 0x40) == 0, "actualStringLength not zeroed");
}

/* The generated dispatch table defines this in the game build; no guest
 * functions exist in this test. */
recomp_func_t recomp_lookup(uint32_t va)
{
    (void)va;
    return NULL;
}

int main(void)
{
    if (guest_mem_init() != 0)
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    if (mprotect(guest_ptr(STACK_LO), STACK_HI - STACK_LO, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }
    thunk_files_init();
    test_iokit();
    test_socket();
    test_keyboard();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
