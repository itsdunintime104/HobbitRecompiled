#ifndef GUEST_MEM_H
#define GUEST_MEM_H

#include <stdint.h>

/* The guest address window: 4 GB (+64 KB guard) reserved at a 4 GB aligned
 * host address. Guest address a lives at g_guest_base + (uint32_t)a.
 * Guest memory is never executable. */

/* Host pages are 16 KB on arm64 macOS, so the first host page of the window
 * (guest 0x0-0x3FFF) stays PROT_NONE. This covers __PAGEZERO and the first
 * 12 KB of __TEXT (Mach-O header, load commands). Those 12 KB are kept in a
 * side buffer. */
#define GUEST_LOW_RESERVED 0x4000u
#define GUEST_HEADER_BASE 0x1000u
#define GUEST_HEADER_SIZE 0x3000u

extern uint8_t *g_guest_base;

static inline void *guest_ptr(uint32_t a)
{
    return g_guest_base + (uint64_t)(uint32_t)a;
}

/* Reserve the window. Returns 0 on success. */
int guest_mem_init(void);

/* Map the Mach-O segments listed in gen_segments.h from the file at path.
 * Returns 0 on success. Requires guest_mem_init(). */
int guest_load_image(const char *path);

/* Print n bytes at guest address a as hex, then a newline. */
void guest_mem_dump(uint32_t a, uint32_t n);

/* Read one guest byte without crashing. Returns 0 and stores the byte in
 * *out, or returns -1 if the access faults. */
int guest_mem_probe(uint32_t a, uint8_t *out);

/* Guest 0x1000-0x3FFF as read from the file (GUEST_HEADER_SIZE bytes). */
const uint8_t *guest_image_header(void);

#endif
