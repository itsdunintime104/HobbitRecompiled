/* Link-time definitions the generated code and recomp_types.h declare but do
 * not define: register globals, dispatch lookups and the missing-import hook.
 * M2 only links; nothing here runs guest code. */
#define RECOMP_GENERATED_CODE
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "recomp_types.h"
#include "guest_mem.h"
#include "diag.h"

/* Every per-thread register; g_eax and the rest name its fields. */
RECOMP_TLS RecompThreadState g_thread_state;

ptrdiff_t g_xbox_mem_offset;
int g_force_return;
uint32_t g_xbox_code_lo;
uint32_t g_xbox_code_hi;

volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
volatile uint64_t g_icall_guard_hits;
volatile uint64_t g_icall_guard_misses;

/* Default for test binaries that link host_funcs.c without the SDL front end;
 * frontend_sdl.c has the real, strong definition. */
__attribute__((weak)) recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return NULL;
}

/* Newest first. ICALL_TRACE_SIZE is a power of two (recomp_types.h). */
void recomp_icall_tail(char *buf, size_t size, unsigned n)
{
    uint32_t idx = g_icall_trace_idx;
    size_t used = 0;
    buf[0] = '\0';
    if (n > ICALL_TRACE_SIZE)
        n = ICALL_TRACE_SIZE;
    for (unsigned i = 0; i < n && used < size; i++) {
        uint32_t v = g_icall_trace[(idx - 1u - i) & (ICALL_TRACE_SIZE - 1)];
        int w = snprintf(buf + used, size - used, "%s0x%08X", i ? " " : "", v);
        if (w < 0 || (size_t)w >= size - used)
            break;
        used += (size_t)w;
    }
}

/* One "RECOMP:" line for every fatal path: the guest return address on top of
 * the stack (the caller's next instruction; the call is the one before it),
 * g_esp and the last 8 indirect call targets. */
static void recomp_caller_log(void)
{
    uint32_t ret = 0;
    uint8_t b;
    char tail[ICALL_TRACE_SIZE * 12];
    if (guest_mem_probe(g_esp, &b) == 0 && guest_mem_probe(g_esp + 3u, &b) == 0)
        memcpy(&ret, guest_ptr(g_esp), sizeof ret);
    recomp_icall_tail(tail, sizeof tail, 8);
    fprintf(stderr, "RECOMP: caller=0x%08X esp=0x%08X icalls=[%s]\n", ret, g_esp, tail);
}

int diag_caches_off(void)
{
    static atomic_int state = -1; /* -1 = unread */
    int v = atomic_load(&state);
    if (v < 0) {
        const char *e = getenv("RECOMP_NO_CACHE");
        v = e != NULL && strcmp(e, "1") == 0;
        atomic_store(&state, v);
    }
    return v;
}

/* Open-addressed set of FNV-1a hashes of "func\0message"; 0 marks an empty
 * slot. When the set is full every call prints. */
void diag_unsupported(const char *func, uint32_t guest_ret, const char *fmt, ...)
{
    enum { SEEN_SLOTS = 1024 };
    static uint64_t seen[SEEN_SLOTS];
    static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    uint64_t h = 14695981039346656037ull;
    for (const char *p = func; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h *= 1099511628211ull; /* the 0 separator byte: xor with 0 is a no-op */
    for (const char *p = msg; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ull;
    if (h == 0)
        h = 1;

    int first = 1;
    pthread_mutex_lock(&seen_lock);
    for (unsigned i = 0; i < SEEN_SLOTS; i++) {
        uint64_t *slot = &seen[(h + i) % SEEN_SLOTS];
        if (*slot == h) {
            first = 0;
            break;
        }
        if (*slot == 0) {
            *slot = h;
            break;
        }
    }
    pthread_mutex_unlock(&seen_lock);
    if (first)
        fprintf(stderr, "UNSUPPORTED %s: %s (guest return 0x%08X)\n", func, msg, guest_ret);
}

void recomp_missing_import(const char *name)
{
    fprintf(stderr, "missing import: %s\n", name);
    recomp_caller_log();
    exit(3);
}

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "indirect call to unresolved VA 0x%08X\n", va);
    recomp_caller_log();
    exit(3);
}

void recomp_icall_not_code_log(uint32_t va)
{
    fprintf(stderr, "indirect call to non-code VA 0x%08X\n", va);
    recomp_caller_log();
    exit(3);
}

void recomp_unimpl(const char *text, uint32_t va)
{
    fprintf(stderr, "unimplemented at 0x%08X: %s\n", va, text);
    recomp_caller_log();
    /* _exit, not exit: exit() runs the guest's atexit destructors, which
     * touch state the stopped thread left half-updated and fault, so the
     * crash report then names the destructor instead of this stop. */
    fflush(NULL);
    _exit(3);
}

void recomp_divide_error(uint32_t va)
{
    fprintf(stderr, "RECOMP: divide error at 0x%08X\n", va);
    recomp_caller_log();
    abort();
}

uint32_t xbox_PortIn(uint32_t port, int width)
{
    fprintf(stderr, "port in 0x%X width %d not implemented\n", port, width);
    exit(3);
}

void xbox_PortOut(uint32_t port, uint32_t value, int width)
{
    fprintf(stderr, "port out 0x%X value 0x%X width %d not implemented\n",
            port, value, width);
    exit(3);
}

#ifndef RECOMP_GLUE_NO_MAIN
/* Thunk unit tests compile with -DRECOMP_GLUE_NO_MAIN and supply their own main. */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image>\n", argv[0]);
        return 2;
    }
    if (guest_mem_init() != 0)
        return 1;
    if (guest_load_image(argv[1]) != 0)
        return 1;
    puts("loaded");
    return 0;
}
#endif
