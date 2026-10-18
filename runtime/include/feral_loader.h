/* Native replacement for Feral's PlugIns/Content.loader bundle.
 * Contract and evidence: docs/FERAL_LOADER.md. */
#ifndef FERAL_LOADER_H
#define FERAL_LOADER_H

#include <stdint.h>

/* The fake guest address of the native FeralEntry for "FeralEntry", else 0.
 * CFBundleGetFunctionPointerForName calls this before the import lookup. */
uint32_t feral_loader_lookup(const char *name);

#endif
