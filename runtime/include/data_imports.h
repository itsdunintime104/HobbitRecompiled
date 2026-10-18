/* Data imports: global variables the game takes from system libraries
 * (___stack_chk_guard, _kCFAllocatorDefault, _NSApp...). The image's
 * non-lazy pointer slots (__nl_symbol_ptr, __got) must hold the GUEST
 * ADDRESS of each variable.
 *
 * Each group (CF, front end...) registers its variables from an init hook.
 * Startup then calls data_imports_bind_all(), which fills every slot whose
 * symbol was registered and logs the ones that were not. */
#ifndef DATA_IMPORTS_H
#define DATA_IMPORTS_H

#include <stdint.h>

/* slot_value is what the non-lazy pointer slot must hold: normally the guest
 * address of a guest copy of the variable; for a function referenced by
 * address, host_func_va() of its thunk. A later call for the same name wins. */
void data_import_register(const char *macho_name, uint32_t slot_value);

/* Guest address of n zeroed bytes of static guest storage, 16-byte aligned,
 * for variables that groups register. Never freed. */
uint32_t data_import_alloc(uint32_t n);

/* Group init hooks. data_imports.c defines a weak empty version of each;
 * the group's own file overrides it. */
void data_imports_core_init(void);     /* data_imports.c: libSystem, libstdc++ */
void thunk_cf_data_init(void);         /* thunk_cf.c */
void frontend_data_init(void);         /* front end */

/* Call every hook, then bind all non-lazy slots of the loaded image.
 * Returns the number of slots left unbound. */
int data_imports_bind_all(void);

#endif
