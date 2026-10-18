/* Host functions that the guest calls through a pointer: data-import
 * function pointers, CFBundleGetFunctionPointerForName results, fake vtables.
 *
 * Each registered function gets a fake guest code address in
 * 0xFE000000-0xFEFFFFFF. RECOMP_ICALL_IS_CODE always allows that range, and
 * RECOMP_ICALL sends it to recomp_lookup_kernel, which host_funcs.c defines.
 * The function is called like any thunk: cdecl arguments at g_esp+4, return
 * address at g_esp, and it must do g_esp += 4. */
#ifndef HOST_FUNCS_H
#define HOST_FUNCS_H

#include <stdint.h>
#include "recomp_types.h"

#define HOST_FUNC_VA_BASE 0xFE000000u
#define HOST_FUNC_VA_STEP 0x10u
#define HOST_FUNC_MAX 4096u

/* Register fn (idempotent: the same fn gives the same address) and return
 * its fake guest address. Thread-safe. Aborts when the table is full. */
uint32_t host_func_va(recomp_func_t fn);

/* The import thunk for a Mach-O symbol name such as "_glClear", or NULL.
 * Covers every name in imports.h; a name with only the weak placeholder
 * returns that placeholder. */
recomp_func_t import_func_by_name(const char *macho_name);

/* Resolve a guest code address: manual table, generated dispatch, then the
 * host function range. Returns NULL when none matches. */
recomp_func_t guest_resolve(uint32_t va);

#endif
