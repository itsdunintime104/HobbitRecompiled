/* Heap thunks: malloc, free, strdup and the C++ operator new/delete. Only
 * the imports listed in recon/import_sigs.json are provided (calloc,
 * realloc, valloc and malloc_size are not imported by the game). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_heap.h"
#include "guest_mem.h"
#include "recomp_types.h"

#define ARG(i) MEM32(g_esp + 4 + 4 * (i))

void imp__malloc(void)
{
    g_heap_site = MEM32(g_esp);
    g_eax = guest_malloc(ARG(0));
    g_heap_site = 0;
    g_esp += 4;
}

void imp__free(void)
{
    g_heap_site = MEM32(g_esp);
    guest_free(ARG(0));
    g_heap_site = 0;
    g_esp += 4;
}

void imp__strdup(void)
{
    uint32_t src = ARG(0);
    uint32_t dst = 0;
    if (src != 0) {
        size_t len = strlen(guest_ptr(src)) + 1;
        g_heap_site = MEM32(g_esp);
        dst = guest_malloc((uint32_t)len);
        g_heap_site = 0;
        if (dst != 0)
            memcpy(guest_ptr(dst), guest_ptr(src), len);
    }
    g_eax = dst;
    g_esp += 4;
}

/* Operator new does not return on failure: the guest has no exception
 * unwinding through a thunk. */
static void new_thunk(void)
{
    uint32_t n = ARG(0);
    g_heap_site = MEM32(g_esp);
    uint32_t a = guest_malloc(n);
    g_heap_site = 0;
    if (a == 0) {
        fprintf(stderr, "operator new: out of guest heap (%u bytes)\n", n);
        abort();
    }
    g_eax = a;
    g_esp += 4;
}

void imp___Znwm(void) { new_thunk(); }
void imp___Znam(void) { new_thunk(); }
void imp___ZdlPv(void) { imp__free(); }
void imp___ZdaPv(void) { imp__free(); }
