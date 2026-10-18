/* The text form the translator emits for a function that uses xmm0 and xmm1:
 * the xmm registers live in locals, and are written back at every call and at
 * return. Checks the call round trip, the return write-back, and that a
 * register the function does not use is left alone. */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>

ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompThreadState g_thread_state;
void recomp_divide_error(uint32_t va) { (void)va; }

static RecompXmm seen_by_callee;

/* A plain function: it sees the registers only through the thread state. */
static void callee(void)
{
    seen_by_callee = g_xmm0;
    g_xmm1 = XMM_ZERO();
    g_xmm1.q[0] = 0x2222;
}

#undef xmm0
#define xmm0 _xmm0
#undef xmm1
#define xmm1 _xmm1
#undef RECOMP_XMM_STORE
#define RECOMP_XMM_STORE() (g_xmm0 = _xmm0, g_xmm1 = _xmm1)
#undef RECOMP_XMM_LOAD
#define RECOMP_XMM_LOAD() (_xmm0 = g_xmm0, _xmm1 = g_xmm1)
static uint64_t func_a(void)
{
    RECOMP_MEM_OFFSET_LOCAL;
    RECOMP_REGS_LOCAL;
    RECOMP_XMM_LOCAL(0);
    RECOMP_XMM_LOCAL(1);
    xmm0 = XMM_ZERO();
    xmm0.q[0] = 0x1111;
    RECOMP_REGS_STORE(); callee(); RECOMP_REGS_LOAD();
    uint64_t seen = xmm1.q[0];
    xmm0 = XMM_ZERO();
    xmm0.q[0] = seen + 0x100;
    return seen;
}
#undef xmm0
#define xmm0 g_xmm0
#undef xmm1
#define xmm1 g_xmm1
#undef RECOMP_XMM_STORE
#define RECOMP_XMM_STORE() ((void)0)
#undef RECOMP_XMM_LOAD
#define RECOMP_XMM_LOAD() ((void)0)

#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s\n", #cond); return 1; } } while (0)

int main(void)
{
    g_xmm0 = XMM_ZERO();
    g_xmm0.q[0] = 0xAAAA;
    g_xmm5 = XMM_ZERO();
    g_xmm5.q[0] = 0x5555;

    uint64_t seen = func_a();

    CHECK(seen_by_callee.q[0] == 0x1111);  /* callee saw A's xmm0 */
    CHECK(seen == 0x2222);                 /* A saw callee's xmm1 */
    CHECK(g_xmm0.q[0] == 0x2322);          /* return wrote A's final xmm0 back */
    CHECK(g_xmm5.q[0] == 0x5555);          /* a register A does not use is unchanged */
    printf("ok\n");
    return 0;
}
