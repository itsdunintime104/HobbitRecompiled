# Feral loader contract

The game binary does not contain its own startup. Guest main loads Feral's
`PlugIns/Content.loader` bundle and hands control to it. The bundle is the
Feral app shell. It calls back into the game once. We replace it with
`runtime/src/feral_loader.c`.

Sources: the game binary `hobbit_i386` (stripped), and the loader
`LEGO The Hobbit.app/Contents/PlugIns/Content.loader/Contents/MacOS/Content`
(i386, 7 MB, with 11063 local symbols, so names below come from `nm -C`).
Disassembly came from otool and capstone. The loader was never run.

## 1. How the game calls the loader

Guest main is `int main(int argc, char **argv)` at `0x001D7E20` (PIC base
`esi = 0x001D7E2E`):

| Address | Call | Arguments |
|---|---|---|
| 0x1D7E60 | `CFDictionaryCreateMutable` | `*kCFAllocatorDefault` (slot 0x11D9D7C), 0, `&kCFCopyStringDictionaryKeyCallBacks` (slot 0x11D9D80), value callbacks NULL |
| 0x1D7E7E | `CFDictionarySetValue` | key CFSTR `"argc"` (0x016876B4), value `&argc` (pointer into the guest stack frame) |
| 0x1D7E94 | `CFDictionarySetValue` | key CFSTR `"argv"` (0x016876C4), value `argv` |
| 0x1D7EB0 | `CFDictionarySetValue` | key CFSTR `"NuMain"` (0x016876D4), value `*(0x11D9D64)` = `0x006F6CE0` |
| 0x1D7EB5 | `CFBundleGetMainBundle`, `CFBundleCopyBuiltInPlugInsURL` | |
| 0x1D7EEA | `CFURLCreateCopyAppendingPathComponent` | NULL, plugins URL, CFSTR `"Content.loader"` (0x016876E4), true |
| 0x1D7F07 | `CFBundleCreate` | allocator, URL |
| 0x1D7F1D | `CFBundleGetFunctionPointerForName` | bundle, CFSTR `"FeralEntry"` (0x016876F4) |
| 0x1D7F31 | `FeralEntry(dict)` | one argument: the dictionary |

Guest main returns FeralEntry's result. Any failure above returns -1.
The dictionary is the whole contract: string keys, raw values (value callbacks
NULL). There is no function table and no struct of callbacks. The only game
pointer is `NuMain` (0x006F6CE0, `FUN_006f6ce0`), with signature
`int NuMain(int argc, char **argv)`. This is the only
`CFBundleGetFunctionPointerForName` call site in the game.

## 2. What FeralEntry and MainEntry do

- `_FeralEntry` (0xBA00) jumps to `feral_tt_main(CFDictionaryRef)` (0x18E450).
- `_MainEntry` (0xB9E0) calls `feral_tt_main(NULL)`. With NULL, the loader
  itself looks up a symbol in the main bundle. The game never uses this path.

`feral_tt_main`:

1. It reads `NuMain` into `_gTTMain` (0x4A34C4), and reads `argc` (dereferenced)
   and `argv`. It also adds the NSProcessInfo arguments to `_gTTArgs`
   (0x4A34B8, a `std::vector<char*>`) after `FeralGame::FilterCommandLine`.
2. It calls `FeralGame::Create()` (0xBA10) once. This allocates
   `CLEGOHobbitGame` (0xEC4 bytes, `CMacGame` base, vtable 0x47CEC8) and calls
   `SetSafeDiscMode(6)`.
3. If `vtbl+0x14` `IsSandBoxed()` is true and `vtbl+0x18` (the receipt check) is
   false, it returns 173. This is the App Store receipt exit code.
4. Otherwise it returns `vtbl+0x08` `CMacGame::Run()` (0x1626D0).

`CMacGame::Run` is the Feral shell. It does NSApplication setup, the menu bar,
preferences, localisation, the options/launcher window, copy protection,
GameCenter, `CMacGame::InitSteam`, Sparkle, the MacDoze Win32 layer (124
`MacDoze::` symbols) and the game window controllers. It runs `WinMain`
through `CHThread::RunWinMain`.

`WinMain` (0xB970) is the only reader of `_gTTMain`. The xref scan found
references only in `feral_tt_main` and a static initializer:

```
if (_gTTMain) dispatch_sync(&_dispatch_main_q, ^{ _gTTMain(0, NULL); });
return 0;
```

The block is at 0xB9B0. It pushes 0 and 0, so the game runs on the main
thread with `argc = 0` and `argv = NULL`. NuMain's result is discarded.

## 3. Services the game uses through the loader

None were found. The loader returns only an int to the game. The game imports
nothing from it:

- The game's Steam imports (`SteamAPI_Init`, `SteamUserStats` and 8 more) come
  from the `Vapor` framework, not from the loader. The loader's own `SteamAPI_*`
  exports serve the shell.
- Achievements and Steam go through Vapor. Crash reporting, receipts, Sparkle
  and GameCenter belong to the shell. The game does not call them back.

Not confirmed:
- what the game's `dlopen`/`dlsym` imports load;
- whether the game relies on side effects of the shell, for example a current
  directory, a created NSApplication, or Steam already initialised.

## 4. Decision: (A) native replacement

| | (A) native FeralEntry | (B) recompile the bundle |
|---|---|---|
| Contract | one dictionary read, one guest call | the same, plus the entire shell |
| Size | about 50 lines of C | 7 MB, 11063 functions, a second image with its own relocations |
| Dependencies | the existing CF thunk | Objective-C runtime messages, AppKit, WebKit, GameKit, AVFoundation, StoreKit, Sparkle, MacDoze, a C++ runtime and exceptions |
| Gain | none lost that the game uses | launcher UI and receipt checks that we do not want |

`runtime/src/feral_loader.c` provides `feral_entry` (host function). It
reads `NuMain` from the dictionary through the `CFDictionaryGetValue`
thunk, using the game's own `"NuMain"` key, and calls `NuMain(0, NULL)` on the
calling (main) thread. Then it returns 0, as WinMain does. Launcher defaults
apply: no options window, default settings, and no shell Steam check. Steam
stays with the Vapor imports. `feral_loader_lookup("FeralEntry")` returns
its fake address. Other names return 0. MainEntry is not needed.

CF lane hook, in `imp__CFBundleGetFunctionPointerForName`, before the
import lookup:

```
va = feral_loader_lookup(buf + 1);
```

with `#include "feral_loader.h"`, and the import lookup guarded by
`va == 0`.
