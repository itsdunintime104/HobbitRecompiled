/* Host function addresses and guest code address resolution. See host_funcs.h. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "host_funcs.h"

/* Defined by the generated import_stubs.c (tools/hobbit/emit_import_stubs.py). */
struct import_entry {
    const char *name;
    recomp_func_t fn;
};
extern const struct import_entry g_import_table[];
extern const unsigned g_import_count;
int import_func_is_placeholder(recomp_func_t fn);

/* Empty defaults for programs that link without the generated file (the unit
 * tests); the generated definitions override them. */
__attribute__((weak)) const struct import_entry g_import_table[1];
__attribute__((weak)) const unsigned g_import_count = 0;
__attribute__((weak)) int import_func_is_placeholder(recomp_func_t fn)
{
    (void)fn;
    return 1;
}

static recomp_func_t g_fns[HOST_FUNC_MAX];
static uint32_t g_fn_count;
static pthread_mutex_t g_fn_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t host_func_va(recomp_func_t fn)
{
    pthread_mutex_lock(&g_fn_lock);
    uint32_t i = 0;
    while (i < g_fn_count && g_fns[i] != fn)
        i++;
    if (i == g_fn_count) {
        if (g_fn_count == HOST_FUNC_MAX) {
            fprintf(stderr, "host_func_va: table full (%u functions)\n", HOST_FUNC_MAX);
            abort();
        }
        g_fns[g_fn_count++] = fn;
    }
    pthread_mutex_unlock(&g_fn_lock);
    return HOST_FUNC_VA_BASE + i * HOST_FUNC_VA_STEP;
}

recomp_func_t recomp_lookup_kernel(uint32_t va)
{
    uint32_t off = va - HOST_FUNC_VA_BASE;
    if (va < HOST_FUNC_VA_BASE || off % HOST_FUNC_VA_STEP != 0)
        return NULL;
    uint32_t i = off / HOST_FUNC_VA_STEP;
    recomp_func_t fn = NULL;
    pthread_mutex_lock(&g_fn_lock);
    if (i < g_fn_count)
        fn = g_fns[i];
    pthread_mutex_unlock(&g_fn_lock);
    return fn;
}

recomp_func_t guest_resolve(uint32_t va)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (fn == NULL)
        fn = recomp_lookup(va);
    if (fn == NULL)
        fn = recomp_lookup_kernel(va);
    return fn;
}

recomp_func_t import_func_by_name(const char *macho_name)
{
    unsigned lo = 0, hi = g_import_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(macho_name, g_import_table[mid].name);
        if (c == 0)
            return g_import_table[mid].fn;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}
