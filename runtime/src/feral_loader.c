/* Native FeralEntry: the part of Feral's Content.loader that the game needs.
 *
 * Guest main (0x001D7E20) builds a CFDictionary {argc: &argc, argv: argv,
 * NuMain: 0x006F6CE0} with string keys and raw values, loads
 * PlugIns/Content.loader and returns FeralEntry(dict). In the bundle,
 * FeralEntry -> feral_tt_main stores NuMain in _gTTMain and starts the Feral
 * app shell; its WinMain (0xB970) then runs
 *     dispatch_sync(main queue, ^{ gTTMain(0, NULL); }); return 0;
 * so the game runs on the main thread with no arguments. This file keeps that
 * call and drops the shell (launcher window, preferences, Steam, receipts),
 * which the game does not depend on. Evidence: docs/FERAL_LOADER.md. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "feral_loader.h"
#include "guest_call.h"
#include "guest_mem.h"
#include "host_funcs.h"
#include "recomp_types.h"

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define RET(v) do { g_eax = (uint32_t)(v); g_esp += 4; } while (0)

/* The game's constant CFString "NuMain", the key guest main stores the
 * game-main pointer under (__cfstring at 0x016876D4, bytes at 0x01170E4F). */
#define GUEST_CFSTR_NUMAIN 0x016876D4u

/* int FeralEntry(CFDictionaryRef dict), cdecl. */
static void feral_entry(void)
{
    uint32_t dict = ARG(0);
    recomp_func_t get = import_func_by_name("_CFDictionaryGetValue");
    uint32_t numain = 0;
    if (dict != 0 && get != NULL) {
        uint32_t args[2] = { dict, GUEST_CFSTR_NUMAIN };
        numain = guest_call_cdecl(host_func_va(get), 2, args);
    }
    if (numain == 0) {
        /* The bundle's FeralEntry has no game to run either; guest main
         * treats -1 as a failed load. */
        fprintf(stderr, "feral_loader: no NuMain in the FeralEntry dictionary\n");
        RET(0xFFFFFFFFu);
        return;
    }
    fprintf(stderr, "feral_loader: FeralEntry calls NuMain 0x%08x(0, NULL)\n", numain);
    uint32_t args[2] = { 0, 0 };
    (void)guest_call_cdecl(numain, 2, args);
    /* WinMain discards NuMain's result and returns 0 (0xB99F). */
    RET(0);
}

uint32_t feral_loader_lookup(const char *name)
{
    if (name != NULL && strcmp(name, "FeralEntry") == 0)
        return host_func_va(feral_entry);
    return 0;
}
