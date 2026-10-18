/* Leftover libSystem thunks: __stack_chk_fail, vsnprintf, vsprintf,
 * glGetString, clearerr, fileno, getc and qsort. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "guest_heap.h"
#include "guest_mem.h"
#include "handles.h"
#include "host_funcs.h"
#include "recomp_types.h"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define RET(v) do { g_eax = (uint32_t)(v); g_esp += 4; } while (0)

/* thunk_printf.c */
uint32_t printf_format_from(uint32_t fmt_guest, uint32_t args_guest, char *out, size_t cap);

/* ---- __stack_chk_fail ---------------------------------------------------- */

void imp____stack_chk_fail(void)
{
    fprintf(stderr, "stack check failed, guest return address 0x%08x\n", MEM32(g_esp));
    abort();
}

/* ---- vsnprintf, vsprintf ------------------------------------------------- */

/* The i386 va_list is a 4-byte guest pointer to the argument area. */
void imp__vsnprintf(void)
{
    uint32_t size = ARG(1);
    char *dst = size ? guest_ptr(ARG(0)) : NULL;
    RET(printf_format_from(ARG(2), ARG(3), dst, size));
}

void imp__vsprintf(void)
{
    RET(printf_format_from(ARG(1), ARG(2), guest_ptr(ARG(0)), SIZE_MAX));
}

/* ---- glGetString --------------------------------------------------------- */

#define GL_STRING_SLOTS 16

static pthread_mutex_t g_gl_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t name; /* GL enum; 0 marks a free slot */
    uint32_t addr; /* guest copy of the string */
} g_gl_strings[GL_STRING_SLOTS];

/* The host string is copied into the guest heap once per enum value. */
void imp__glGetString(void)
{
    uint32_t name = ARG(0);
    /* macOS libGL dereferences a null context and crashes, so check first. */
    const char *s = CGLGetCurrentContext() ? (const char *)glGetString(name) : NULL;
    if (s == NULL) {
        RET(0);
        return;
    }
    uint32_t addr = 0;
    int table_full = 1;
    pthread_mutex_lock(&g_gl_lock);
    for (int i = 0; i < GL_STRING_SLOTS; i++) {
        if (g_gl_strings[i].name == name) {
            addr = g_gl_strings[i].addr;
            table_full = 0;
            break;
        }
        if (g_gl_strings[i].name == 0) {
            table_full = 0;
            uint32_t n = (uint32_t)strlen(s) + 1;
            addr = guest_malloc(n);
            if (addr != 0) {
                memcpy(guest_ptr(addr), s, n);
                g_gl_strings[i].name = name;
                g_gl_strings[i].addr = addr;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_gl_lock);
    if (table_full)
        THUNK_UNSUPPORTED("string table full (name 0x%x)", name);
    RET(addr);
}

/* ---- FILE* handles ------------------------------------------------------- */

void imp__clearerr(void)
{
    FILE *f = handle_get(ARG(0), HANDLE_FILE);
    if (f != NULL)
        clearerr(f);
    g_esp += 4;
}

void imp__fileno(void)
{
    FILE *f = handle_get(ARG(0), HANDLE_FILE);
    RET(f != NULL ? fileno(f) : -1);
}

void imp__getc(void)
{
    FILE *f = handle_get(ARG(0), HANDLE_FILE);
    RET(f != NULL ? getc(f) : EOF);
}

/* ---- qsort --------------------------------------------------------------- */

/* Call the guest comparator with two guest element addresses and return its
 * int32 result. The new frame sits below the thunk's frame with the first
 * argument 16-byte aligned, so the callee enters with esp 12 mod 16 as the
 * i386 Darwin ABI requires. */
static int32_t guest_compare(uint32_t compar, uint32_t a, uint32_t b)
{
    uint32_t saved_esp = g_esp;
    uint32_t base = (saved_esp - 32u) & ~15u;
    MEM32(base) = a;
    MEM32(base + 4) = b;
    MEM32(base - 4) = 0; /* dummy return address */
    g_esp = base - 4;
    recomp_func_t fn = guest_resolve(compar);
    if (fn == NULL)
        recomp_icall_fail_log(compar);
    fn();
    int32_t r = (int32_t)g_eax;
    g_esp = saved_esp;
    return r;
}

/* Bottom-up merge sort over guest memory. Runs ping-pong between the array
 * and a guest scratch buffer, so every comparison sees guest addresses. */
void imp__qsort(void)
{
    uint32_t base = ARG(0), nel = ARG(1), width = ARG(2), compar = ARG(3);
    if (nel < 2 || width == 0) {
        g_esp += 4;
        return;
    }
    uint64_t total = (uint64_t)nel * width;
    uint32_t tmp = total <= UINT32_MAX ? guest_malloc((uint32_t)total) : 0;
    if (tmp == 0) {
        fprintf(stderr, "qsort: cannot allocate %llu bytes of guest scratch\n",
                (unsigned long long)total);
        abort();
    }
    uint32_t src = base, dst = tmp;
    for (uint64_t run = 1; run < nel; run *= 2) {
        for (uint64_t lo = 0; lo < nel; lo += 2 * run) {
            uint64_t mid = lo + run < nel ? lo + run : nel;
            uint64_t hi = lo + 2 * run < nel ? lo + 2 * run : nel;
            uint64_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                uint32_t left = src + (uint32_t)(i * width);
                uint32_t right = src + (uint32_t)(j * width);
                uint32_t take = guest_compare(compar, left, right) <= 0 ? left : right;
                memcpy(guest_ptr(dst + (uint32_t)(k++ * width)), guest_ptr(take), width);
                if (take == left)
                    i++;
                else
                    j++;
            }
            if (i < mid)
                memcpy(guest_ptr(dst + (uint32_t)(k * width)),
                       guest_ptr(src + (uint32_t)(i * width)), (size_t)(mid - i) * width);
            else if (j < hi)
                memcpy(guest_ptr(dst + (uint32_t)(k * width)),
                       guest_ptr(src + (uint32_t)(j * width)), (size_t)(hi - j) * width);
        }
        uint32_t t = src;
        src = dst;
        dst = t;
    }
    if (src != base)
        memcpy(guest_ptr(base), guest_ptr(src), (size_t)total);
    guest_free(tmp);
    g_esp += 4;
}
