/* Unit test for the leftover libSystem thunks (T7).
 * Prints "ok" and exits 0 when every check passes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "guest_mem.h"
#include "guest_heap.h"
#include "handles.h"
#include "recomp_types.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP 0x00FF0000u
#define SCRATCH 0x00F10000u
#define RET_MAGIC 0xDEADBEEFu
#define VA_CMP 0x00401000u

void imp____stack_chk_fail(void);
void imp__vsnprintf(void);
void imp__vsprintf(void);
void imp__glGetString(void);
void imp__clearerr(void);
void imp__fileno(void);
void imp__getc(void);
void imp__qsort(void);

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

/* Guest comparator: unsigned 32-bit compare of the two pointed-to values. */
static int g_cmp_calls, g_cmp_bad_esp;

static void guest_cmp_u32(void)
{
    if ((g_esp & 15u) != 12u)
        g_cmp_bad_esp++;
    uint32_t a = MEM32(MEM32(g_esp + 4));
    uint32_t b = MEM32(MEM32(g_esp + 8));
    g_eax = a < b ? (uint32_t)-1 : a > b ? 1u : 0u;
    g_cmp_calls++;
}

recomp_func_t recomp_lookup(uint32_t va)
{
    return va == VA_CMP ? guest_cmp_u32 : NULL;
}

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

static void test_vsnprintf(void)
{
    uint32_t fmt = SCRATCH, str = SCRATCH + 0x40, buf = SCRATCH + 0x80, va = SCRATCH + 0x200;
    strcpy(guest_ptr(fmt), "%d|%s|%.1f");
    strcpy(guest_ptr(str), "guest");
    double d = 2.5;
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    MEM32(va) = (uint32_t)-42;
    MEM32(va + 4) = str;
    MEM32(va + 8) = (uint32_t)bits;
    MEM32(va + 12) = (uint32_t)(bits >> 32);

    uint32_t args[] = {buf, 64, fmt, va};
    uint32_t ret = call(imp__vsnprintf, args, 4);
    CHECK(strcmp(guest_ptr(buf), "-42|guest|2.5") == 0, "vsnprintf got \"%s\"",
          (char *)guest_ptr(buf));
    CHECK(ret == 13, "vsnprintf returned %u", ret);

    uint32_t small[] = {buf, 5, fmt, va};
    ret = call(imp__vsnprintf, small, 4);
    CHECK(strcmp(guest_ptr(buf), "-42|") == 0, "truncated got \"%s\"", (char *)guest_ptr(buf));
    CHECK(ret == 13, "truncated vsnprintf returned %u", ret);

    uint32_t plain[] = {buf + 0x40, fmt, va};
    ret = call(imp__vsprintf, plain, 3);
    CHECK(strcmp(guest_ptr(buf + 0x40), "-42|guest|2.5") == 0, "vsprintf got \"%s\"",
          (char *)guest_ptr(buf + 0x40));
    CHECK(ret == 13, "vsprintf returned %u", ret);
}

static void test_files(void)
{
    uint32_t h = handle_put(stdout, HANDLE_FILE);
    uint32_t args[] = {h};
    CHECK(call(imp__fileno, args, 1) == 1, "fileno(stdout) != 1");
    uint32_t bad[] = {0x1234};
    CHECK((int)call(imp__fileno, bad, 1) == -1, "fileno(bad) != -1");

    FILE *f = tmpfile();
    fputc('A', f);
    rewind(f);
    uint32_t fh = handle_put(f, HANDLE_FILE);
    uint32_t fa[] = {fh};
    CHECK(call(imp__getc, fa, 1) == 'A', "getc did not return 'A'");
    CHECK((int)call(imp__getc, fa, 1) == EOF, "getc did not return EOF");
    CHECK(feof(f), "feof not set");
    call(imp__clearerr, fa, 1);
    CHECK(!feof(f), "clearerr did not clear EOF");
    fclose(f);
}

static void test_qsort(void)
{
    uint32_t base = guest_malloc(50 * 4);
    uint32_t want[50];
    srand(12345);
    for (int i = 0; i < 50; i++) {
        want[i] = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        MEM32(base + 4u * (uint32_t)i) = want[i];
    }
    uint32_t args[] = {base, 50, 4, VA_CMP};
    call(imp__qsort, args, 4);
    for (int i = 0; i < 50; i++)
        for (int j = i + 1; j < 50; j++)
            if (want[j] < want[i]) {
                uint32_t t = want[i];
                want[i] = want[j];
                want[j] = t;
            }
    int ok = 1;
    for (int i = 0; i < 50; i++)
        ok &= MEM32(base + 4u * (uint32_t)i) == want[i];
    CHECK(ok, "qsort result is not sorted");
    CHECK(g_cmp_calls > 0, "comparator never called");
    CHECK(g_cmp_bad_esp == 0, "comparator entered with misaligned esp");
    guest_free(base);
}

static void test_gl(void)
{
    uint32_t args[] = {0x1F00}; /* GL_VENDOR */
    CHECK(call(imp__glGetString, args, 1) == 0, "glGetString without a context != 0");
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
    if (guest_heap_init() != 0)
        return 2;
    /* __stack_chk_fail aborts, so it is only linked and referenced here. */
    void (*volatile chk)(void) = imp____stack_chk_fail;
    CHECK(chk != NULL, "imp____stack_chk_fail missing");
    test_vsnprintf();
    test_files();
    test_qsort();
    test_gl();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
