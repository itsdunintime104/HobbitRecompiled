#ifndef GUEST_HEAP_H
#define GUEST_HEAP_H

#include <stdint.h>

/* Guest heap: a dlmalloc 2.8.6 mspace over guest 0x20000000-0xDFFFFFFF.
 * Blocks are 16-byte aligned. Addresses are guest addresses; 0 means NULL.
 * Requires guest_mem_init(). Calls initialise the heap on first use. */
#define GUEST_HEAP_BASE 0x20000000u
#define GUEST_HEAP_END 0xE0000000u /* exclusive */

/* Make the heap range read/write and create the mspace. Returns 0 on
 * success. Safe to call more than once. */
int guest_heap_init(void);

/* Heap guard (opt-in, env RECOMP_HEAP_GUARD): every block gets a 16-byte
 * header and a 16-byte 0xFB tail; freed blocks are filled with 0xDD and
 * held in a quarantine. A bad block is reported with its allocation and
 * free sites. Value 1: a checker thread scans the quarantine every second
 * and the process aborts on the first finding. Value 2: no checker thread
 * (blocks are checked on eviction); each finding is logged once and the
 * process keeps running.
 * Thunks set g_heap_site to the guest return address around a heap call
 * (0 = runtime-internal call). */
extern __thread uint32_t g_heap_site;

/* The active RECOMP_HEAP_GUARD mode: 0 off, 1 or 2 as above. */
int guest_heap_guard_mode(void);

uint32_t guest_malloc(uint32_t n);
void guest_free(uint32_t a);
uint32_t guest_calloc(uint32_t n, uint32_t s);
uint32_t guest_realloc(uint32_t a, uint32_t n);

#endif
