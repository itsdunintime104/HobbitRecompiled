/* Unit test for runtime/src/thunk_rt.c: calls the thunks on a guest stack. */
#define RECOMP_GENERATED_CODE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "recomp_types.h"
#include "guest_mem.h"

#define STACK_LO 0x00F00000u
#define STACK_HI 0x01000000u
#define STACK_TOP (STACK_HI - 0x100u)
#define GUARD_ADDR 0x00F10000u

void imp____divdi3(void);
void imp____udivdi3(void);
void imp____moddi3(void);
void imp____umoddi3(void);
void imp____fixunsdfdi(void);
void imp____fixunssfdi(void);
void imp____cxa_guard_acquire(void);
void imp____cxa_guard_release(void);
void imp____cxa_guard_abort(void);
void imp____cxa_atexit(void);
void imp___ZNSt8ios_base4InitC1Ev(void);
void imp__SteamAPI_Init(void);
void imp__SteamAPI_RestartAppIfNecessary(void);
void imp__SteamAPI_RunCallbacks(void);
void imp__SteamUser(void);
void imp__dlopen(void);
void imp__dlsym(void);
void thunk_rt_run_atexit(void);

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL line %d: ", __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        g_failures++; \
    } \
} while (0)

/* The atexit test needs a fake recompiled function. */
static uint32_t g_atexit_seen[8];
static int g_atexit_n;

static void fake_atexit_fn(void)
{
    /* cdecl: [esp] = return address, [esp+4] = argument. The callee pops the return. */
    g_atexit_seen[g_atexit_n++] = MEM32(g_esp + 4);
    g_esp += 4;
}

recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x1000u ? fake_atexit_fn : NULL;
}

/* Lay out the arguments (little endian words) and a dummy return address. */
static void setup_call(const uint32_t *words, int n)
{
    g_esp = STACK_TOP;
    for (int i = 0; i < n; i++)
        MEM32(g_esp + 4 + 4 * i) = words[i];
    MEM32(g_esp) = 0xDEADBEEFu;
    g_ebx = 0x11111111u;
    g_esi = 0x22222222u;
    g_edi = 0x33333333u;
    g_ebp = 0x44444444u;
}

static void check_protocol(const char *name)
{
    CHECK(g_esp == STACK_TOP + 4, "%s: esp moved by %d, expected 4", name, (int)(g_esp - STACK_TOP));
    CHECK(g_ebx == 0x11111111u && g_esi == 0x22222222u && g_edi == 0x33333333u &&
          g_ebp == 0x44444444u, "%s: callee-saved register changed", name);
}

static uint64_t call64(void (*fn)(void), uint64_t a, uint64_t b, const char *name)
{
    uint32_t w[4] = {(uint32_t)a, (uint32_t)(a >> 32), (uint32_t)b, (uint32_t)(b >> 32)};
    setup_call(w, 4);
    fn();
    check_protocol(name);
    return ((uint64_t)g_edx << 32) | g_eax;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint64_t next_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void test_divmod(void)
{
    for (int i = 0; i < 20; i++) {
        uint64_t ua = next_rand(), ub = next_rand();
        /* Vary operand sizes so quotients are not always 0 or 1. */
        ua >>= (next_rand() % 40);
        ub >>= (next_rand() % 60);
        if (ub == 0)
            ub = 7;
        int64_t sa = (int64_t)ua, sb = (int64_t)ub;
        if (i % 2)
            sa = -sa;
        if (i % 3 == 0)
            sb = -sb;
        if (sb == 0)
            sb = -3;

        CHECK(call64(imp____divdi3, (uint64_t)sa, (uint64_t)sb, "divdi3") == (uint64_t)(sa / sb),
              "divdi3 %lld / %lld", (long long)sa, (long long)sb);
        CHECK(call64(imp____moddi3, (uint64_t)sa, (uint64_t)sb, "moddi3") == (uint64_t)(sa % sb),
              "moddi3 %lld %% %lld", (long long)sa, (long long)sb);
        CHECK(call64(imp____udivdi3, ua, ub, "udivdi3") == ua / ub,
              "udivdi3 %llu / %llu", (unsigned long long)ua, (unsigned long long)ub);
        CHECK(call64(imp____umoddi3, ua, ub, "umoddi3") == ua % ub,
              "umoddi3 %llu %% %llu", (unsigned long long)ua, (unsigned long long)ub);
    }
    CHECK(call64(imp____divdi3, (uint64_t)INT64_MIN, (uint64_t)-1, "divdi3 min") == (uint64_t)INT64_MIN,
          "divdi3 INT64_MIN / -1");
    CHECK(call64(imp____moddi3, (uint64_t)INT64_MIN, (uint64_t)-1, "moddi3 min") == 0,
          "moddi3 INT64_MIN %% -1");
}

static void test_fixuns(void)
{
    double d = 12345678901234.75;
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    uint32_t w[2] = {(uint32_t)bits, (uint32_t)(bits >> 32)};
    setup_call(w, 2);
    imp____fixunsdfdi();
    check_protocol("fixunsdfdi");
    CHECK((((uint64_t)g_edx << 32) | g_eax) == 12345678901234ull, "fixunsdfdi");

    float f = 4294967296.0f * 3.0f;
    uint32_t fw;
    memcpy(&fw, &f, sizeof fw);
    setup_call(&fw, 1);
    imp____fixunssfdi();
    check_protocol("fixunssfdi");
    CHECK((((uint64_t)g_edx << 32) | g_eax) == 3ull << 32, "fixunssfdi");
}

static uint32_t call_guard(void (*fn)(void), const char *name)
{
    uint32_t w[1] = {GUARD_ADDR};
    setup_call(w, 1);
    fn();
    check_protocol(name);
    return g_eax;
}

static void test_guard(void)
{
    memset(guest_ptr(GUARD_ADDR), 0, 8);
    CHECK(call_guard(imp____cxa_guard_acquire, "acquire 1") == 1, "first acquire must return 1");
    call_guard(imp____cxa_guard_release, "release");
    CHECK(*(uint8_t *)guest_ptr(GUARD_ADDR) == 1, "guard byte 0 not set after release");
    CHECK(call_guard(imp____cxa_guard_acquire, "acquire 2") == 0, "second acquire must return 0");

    /* abort: the guard goes back to "not done" and can be acquired again. */
    memset(guest_ptr(GUARD_ADDR), 0, 8);
    CHECK(call_guard(imp____cxa_guard_acquire, "acquire 3") == 1, "acquire before abort");
    call_guard(imp____cxa_guard_abort, "abort");
    CHECK(call_guard(imp____cxa_guard_acquire, "acquire 4") == 1, "acquire after abort");
}

static void test_atexit(void)
{
    for (uint32_t i = 1; i <= 3; i++) {
        uint32_t w[3] = {0x1000u, 0xA0u + i, 0};
        setup_call(w, 3);
        imp____cxa_atexit();
        check_protocol("cxa_atexit");
        CHECK(g_eax == 0, "cxa_atexit result %u", g_eax);
    }
    g_esp = STACK_TOP;
    thunk_rt_run_atexit();
    CHECK(g_esp == STACK_TOP, "run_atexit changed esp");
    CHECK(g_atexit_n == 3 && g_atexit_seen[0] == 0xA3 && g_atexit_seen[1] == 0xA2 &&
          g_atexit_seen[2] == 0xA1, "atexit order: n=%d %x %x %x", g_atexit_n,
          g_atexit_seen[0], g_atexit_seen[1], g_atexit_seen[2]);
    thunk_rt_run_atexit();
    CHECK(g_atexit_n == 3, "list not empty after run");
}

static uint32_t call0(void (*fn)(void), const char *name)
{
    g_eax = 0xCCCCCCCCu;
    setup_call(NULL, 0);
    fn();
    check_protocol(name);
    return g_eax;
}

static void test_steam_and_misc(void)
{
    CHECK(call0(imp__SteamAPI_Init, "SteamAPI_Init") == 1, "SteamAPI_Init must return 1");
    CHECK(call0(imp__SteamAPI_RestartAppIfNecessary, "Restart") == 0, "RestartAppIfNecessary");
    CHECK(call0(imp__SteamAPI_RunCallbacks, "RunCallbacks") == 0, "RunCallbacks");
    CHECK(call0(imp__SteamUser, "SteamUser") != 0, "SteamUser returns the fake interface");
    CHECK(call0(imp__dlopen, "dlopen") != 0, "dlopen returns a handle");
    uint32_t null_name[2] = {0xFF00FFFEu, 0};
    setup_call(null_name, 2);
    imp__dlsym();
    check_protocol("dlsym");
    CHECK(g_eax == 0, "dlsym with a NULL name must return 0");
    call0(imp___ZNSt8ios_base4InitC1Ev, "ios_base::Init");
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
    test_divmod();
    test_fixuns();
    test_guard();
    test_atexit();
    test_steam_and_misc();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("thunk_rt ok");
    return 0;
}
