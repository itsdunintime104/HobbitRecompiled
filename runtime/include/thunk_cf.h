#ifndef THUNK_CF_H
#define THUNK_CF_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>

/* Bridge between host CF objects and guest CF references (thunk_cf.c). */

/* Guest handle for host object p, or 0 if p is NULL. Takes no reference: the
 * guest owns whatever reference the caller already holds. */
uint32_t cf_to_guest(CFTypeRef p);

/* Host object for guest reference v (a CF handle or a constant string), or
 * NULL if v is 0. Aborts on any other value. */
CFTypeRef cf_from_guest(uint32_t v);

/* Release one reference that the guest owns, as the guest CFRelease does.
 * For host thunks of APIs that consume a reference. */
void cf_release(CFTypeRef p);

#endif
