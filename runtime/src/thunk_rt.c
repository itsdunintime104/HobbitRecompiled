/* Thunks T5: libgcc 64-bit helpers, C++ runtime support, Steam no-ops and
 * the dlopen/dlsym stubs. Protocol: docs/PLAN.md, "M3 thunk contract".
 * Arguments are read through guest_ptr so this file does not depend on
 * g_xbox_mem_offset being set. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "thunk_gl.h"
#include "steam_fake.h"

int import_func_is_placeholder(recomp_func_t fn);

/* Calls every function registered through ___cxa_atexit, newest first. */
void thunk_rt_run_atexit(void);

/* Read the n-th 32-bit cdecl argument (0-based) of the current call. */
static uint32_t arg32(unsigned n)
{
    uint32_t v;
    memcpy(&v, guest_ptr(g_esp + 4 + 4 * n), sizeof v);
    return v;
}

/* Read the 64-bit argument that starts at byte offset off from the first argument. */
static uint64_t arg64(unsigned off)
{
    uint64_t v;
    memcpy(&v, guest_ptr(g_esp + 4 + off), sizeof v);
    return v;
}

static void ret64(uint64_t v)
{
    g_eax = (uint32_t)v;
    g_edx = (uint32_t)(v >> 32);
    g_esp += 4;
}

static void ret32(uint32_t v)
{
    g_eax = v;
    g_esp += 4;
}

static void ret_void(void)
{
    g_esp += 4;
}

static void rt_die(const char *msg) __attribute__((noreturn));

static void rt_die(const char *msg)
{
    fprintf(stderr, "thunk_rt: %s\n", msg);
    abort();
}

/* ---- libgcc 64-bit helpers: two i64 arguments, result in edx:eax ---- */

static void check_divisor(uint64_t b)
{
    if (b == 0)
        rt_die("64-bit division by zero in guest code");
}

void imp____divdi3(void)
{
    int64_t a = (int64_t)arg64(0), b = (int64_t)arg64(8);
    check_divisor((uint64_t)b);
    /* INT64_MIN / -1 overflows; x86 libgcc yields INT64_MIN. */
    ret64(b == -1 ? 0 - (uint64_t)a : (uint64_t)(a / b));
}

void imp____moddi3(void)
{
    int64_t a = (int64_t)arg64(0), b = (int64_t)arg64(8);
    check_divisor((uint64_t)b);
    ret64(b == -1 ? 0 : (uint64_t)(a % b));
}

void imp____udivdi3(void)
{
    uint64_t a = arg64(0), b = arg64(8);
    check_divisor(b);
    ret64(a / b);
}

void imp____umoddi3(void)
{
    uint64_t a = arg64(0), b = arg64(8);
    check_divisor(b);
    ret64(a % b);
}

/* Floating point to u64; out-of-range values saturate. */
static uint64_t to_u64(double d)
{
    if (!(d > 0.0))
        return 0;
    if (d >= 18446744073709551616.0)
        return UINT64_MAX;
    return (uint64_t)d;
}

void imp____fixunsdfdi(void)
{
    uint64_t bits = arg64(0);
    double d;
    memcpy(&d, &bits, sizeof d);
    ret64(to_u64(d));
}

void imp____fixunssfdi(void)
{
    uint32_t bits = arg32(0);
    float f;
    memcpy(&f, &bits, sizeof f);
    ret64(to_u64((double)f));
}

/* ---- C++ static-init guards ----
 * The guard is a 64-bit guest word. Byte 0 is "done". Byte 1 is "being
 * initialised by some thread". Both are touched with host atomics. */

static uint8_t *guard_bytes(void)
{
    return (uint8_t *)guest_ptr(arg32(0));
}

void imp____cxa_guard_acquire(void)
{
    uint8_t *g = guard_bytes();
    for (;;) {
        if (__atomic_load_n(&g[0], __ATOMIC_ACQUIRE)) {
            ret32(0);
            return;
        }
        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(&g[1], &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            /* Another thread may have finished between the two checks. */
            if (__atomic_load_n(&g[0], __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&g[1], 0, __ATOMIC_RELEASE);
                ret32(0);
            } else {
                ret32(1);
            }
            return;
        }
        sched_yield();
    }
}

void imp____cxa_guard_release(void)
{
    uint8_t *g = guard_bytes();
    __atomic_store_n(&g[0], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g[1], 0, __ATOMIC_RELEASE);
    ret_void();
}

void imp____cxa_guard_abort(void)
{
    uint8_t *g = guard_bytes();
    __atomic_store_n(&g[1], 0, __ATOMIC_RELEASE);
    ret_void();
}

/* ---- atexit list ---- */

struct atexit_entry {
    uint32_t fn_va;
    uint32_t arg;
};

static pthread_mutex_t g_atexit_lock = PTHREAD_MUTEX_INITIALIZER;
static struct atexit_entry *g_atexit_list;
static size_t g_atexit_count, g_atexit_cap;

/* int __cxa_atexit(void (*fn)(void *), void *arg, void *dso_handle) */
void imp____cxa_atexit(void)
{
    uint32_t fn = arg32(0), arg = arg32(1);
    pthread_mutex_lock(&g_atexit_lock);
    if (g_atexit_count == g_atexit_cap) {
        size_t cap = g_atexit_cap ? g_atexit_cap * 2 : 16;
        struct atexit_entry *list = realloc(g_atexit_list, cap * sizeof *list);
        if (list == NULL)
            rt_die("out of memory in __cxa_atexit");
        g_atexit_list = list;
        g_atexit_cap = cap;
    }
    g_atexit_list[g_atexit_count++] = (struct atexit_entry){fn, arg};
    pthread_mutex_unlock(&g_atexit_lock);
    ret32(0);
}

void thunk_rt_run_atexit(void)
{
    for (;;) {
        pthread_mutex_lock(&g_atexit_lock);
        if (g_atexit_count == 0) {
            pthread_mutex_unlock(&g_atexit_lock);
            return;
        }
        struct atexit_entry e = g_atexit_list[--g_atexit_count];
        pthread_mutex_unlock(&g_atexit_lock);

        recomp_func_t fn = guest_resolve(e.fn_va);
        if (fn == NULL) {
            fprintf(stderr, "thunk_rt: atexit function 0x%08X is not recompiled\n", e.fn_va);
            abort();
        }
        uint32_t esp0 = g_esp;
        g_esp -= 4;
        memcpy(guest_ptr(g_esp), &e.arg, sizeof e.arg);
        g_esp -= 4;
        memset(guest_ptr(g_esp), 0, 4); /* return address; the callee pops it with its ret */
        fn();
        g_esp = esp0;
    }
}

/* ---- C++ runtime entries that must never run ---- */

void imp____cxa_pure_virtual(void)
{
    rt_die("pure virtual function called in guest code");
}

void imp___Unwind_Resume(void)
{
    rt_die("_Unwind_Resume called: guest C++ exception unwinding is not supported");
}

void imp____gxx_personality_v0(void)
{
    rt_die("__gxx_personality_v0 called: only the unwinder calls it, which is not supported");
}

/* std::ios_base::Init::Init() and ~Init(): `this` is the only argument.
 * Nothing needs initialising, so both only return. */
void imp___ZNSt8ios_base4InitC1Ev(void)
{
    ret_void();
}

void imp___ZNSt8ios_base4InitD1Ev(void)
{
    ret_void();
}

/* ---- Steam API (Vapor dylib): the game runs offline without Steam ----
 * All entries are cdecl. Interface accessors return fake guest interface
 * objects (steam_fake.c): Steam running, user offline, cloud off, no DLC. */

void imp__SteamAPI_Init(void)
{
    ret32(1);
}

void imp__SteamAPI_RestartAppIfNecessary(void)
{
    ret32(0);
}

void imp__SteamAPI_RegisterCallback(void)
{
    ret32(0);
}

void imp__SteamAPI_RunCallbacks(void)
{
    ret32(0);
}

void imp__SteamAPI_UnregisterCallback(void)
{
    ret32(0);
}

void imp__SteamApps(void)
{
    ret32(steam_fake_iface("SteamApps"));
}

void imp__SteamRemoteStorage(void)
{
    ret32(steam_fake_iface("SteamRemoteStorage"));
}

void imp__SteamUser(void)
{
    ret32(steam_fake_iface("SteamUser"));
}

void imp__SteamUserStats(void)
{
    ret32(steam_fake_iface("SteamUserStats"));
}

void imp__SteamUtils(void)
{
    ret32(steam_fake_iface("SteamUtils"));
}

/* ---- dlopen / dlsym ----
 * The game dlopens the OpenGL framework and dlsyms its GL extension entry
 * points. dlopen returns one fixed handle. dlsym looks the name up in the
 * hand-written GL table (thunk_gl.c), then among the import thunks, and
 * returns the fake guest address of the thunk, or 0. Each name is logged once. */

#define DLOPEN_HANDLE 0xFF00FFFEu
#define DLSYM_LOGGED_MAX 4096

void imp__dlopen(void)
{
    ret32(DLOPEN_HANDLE);
}

/* Failed lookups always print; the ~700 successful ones only with
 * RECOMP_DLSYM_VERBOSE=1, so they do not bury the rest of the log. */
static void dlsym_log_once(const char *name, uint32_t va)
{
    static int verbose = -1;
    if (verbose < 0) {
        const char *v = getenv("RECOMP_DLSYM_VERBOSE");
        verbose = v != NULL && v[0] == '1';
    }
    if (va != 0 && !verbose)
        return;
    static char *logged[DLSYM_LOGGED_MAX];
    static unsigned nlogged;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < nlogged && strcmp(logged[i], name) != 0)
        i++;
    if (i == nlogged && nlogged < DLSYM_LOGGED_MAX) {
        logged[nlogged++] = strdup(name);
        fprintf(stderr, "thunk_rt: dlsym %s -> 0x%08x\n", name, va);
    }
    pthread_mutex_unlock(&lock);
}

void imp__dlsym(void)
{
    uint32_t name_va = arg32(1);
    uint32_t va = 0;
    if (name_va != 0) {
        const char *name = guest_ptr(name_va);
        recomp_func_t fn = gl_ext_lookup(name);
        if (fn == NULL) {
            char macho[256];
            snprintf(macho, sizeof macho, "_%s", name);
            fn = import_func_by_name(macho);
            if (fn != NULL && import_func_is_placeholder(fn))
                fn = NULL;
        }
        if (fn != NULL)
            va = host_func_va(fn);
        dlsym_log_once(name, va);
    }
    ret32(va);
}
