#ifndef HANDLES_H
#define HANDLES_H

#include <stdint.h>

/* Handles stand in for host objects (FILE*, DIR*, CF objects, ...) that the
 * guest holds as 32-bit values. A handle is HANDLE_BASE + table index. That
 * range is never mapped, so a guest that dereferences a handle faults. */

#define HANDLE_BASE 0xFF000000u
#define HANDLE_MAX 65536u

enum handle_kind {
    HANDLE_NONE = 0,
    HANDLE_FILE,
    HANDLE_DIR,
    HANDLE_CF,
    HANDLE_IOKIT,
    HANDLE_DLOPEN,
    HANDLE_AUDIO,
};

/* Store host and return its handle, or 0 if the table is full. host must not
 * be NULL. */
uint32_t handle_put(void *host, enum handle_kind kind);

/* Return the host object for h, or NULL if h is not a live handle of that kind. */
void *handle_get(uint32_t h, enum handle_kind kind);

/* Free h. Returns the host object, or NULL if h is not a live handle of that
 * kind. Indexes are reused round robin, so a stale handle is detected until
 * the table wraps. */
void *handle_release(uint32_t h, enum handle_kind kind);

/* Static guest area. thunk_files_init() maps it read/write. */
#define GUEST_STATIC_BASE 0x00F80000u
#define GUEST_STATIC_SIZE 0x4000u

/* Data imports: each is a guest u32 slot that holds a FILE handle for the
 * host stdin, stdout or stderr. The startup code binds the non-lazy pointer
 * of ___stdinp, ___stdoutp and ___stderrp to these addresses. */
#define GUEST_STDINP_ADDR (GUEST_STATIC_BASE + 0x00u)
#define GUEST_STDOUTP_ADDR (GUEST_STATIC_BASE + 0x04u)
#define GUEST_STDERRP_ADDR (GUEST_STATIC_BASE + 0x08u)

/* Map the static guest area and fill the std stream slots. Needs
 * guest_mem_init(). Safe to call more than once. */
void thunk_files_init(void);

/* Guest address of the calling thread's errno slot (a guest u32). */
uint32_t guest_errno_slot(void);

/* Store a host errno value in the calling thread's guest errno slot. Darwin
 * errno numbers are the same on the host and in the i386 guest. */
void guest_set_errno(int host_errno);

#endif
