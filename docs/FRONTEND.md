# Front end: SDL2 in place of the Objective-C block

The game's front end (block 0x3db000–0x3e2000, 128 lifted functions, all in `generated.nosync/hobbit_0025.c`) builds its window, GL context and event loop from four ObjC classes. SDL2 2.32 replaces it. One SDL window and one legacy 2.1 GL context serve every game window.

Code: `runtime/src/frontend_sdl.c`, `runtime/src/frontend_keymap.c`, `runtime/include/frontend_sdl.h`, `runtime/include/frontend_rename.h`.

## (a) Functions the rest of the game calls

36 block functions have callers outside the block (scan of `RECOMP_ABI_CALL` sites in `generated.nosync`). 19 of them reach `objc_msgSend` and are replaced. The other 17 stay lifted.

| VA | Callers | What it does | Native version |
|---|---|---|---|
| 3dcc60, 3dccb0 | 1fb150, 1fb3c0 | create and drain the main autorelease pool | no-op |
| 3dd420 | 2a5650 | one-time app setup: NSApplication, delegate, clear key buffers, two IOPM no-sleep assertions, start time | SDL_Init, SDL_DisableScreenSaver, clear keys, gettimeofday |
| 3df9c0 | 2a4e20 | main display ID and mode, retina flag, screen size into 0x161b2f0/0x161b2f4 | SDL desktop mode; retina 0 |
| 3de630(int *w, int *h) | 2a5650, 2a5960 | main screen size | SDL desktop size |
| 3dcd20(int *w, int *h) | 316a50 | active GL view frame | SDL_GetWindowSize |
| 3dff70(int *w, int *h) | 2a4eb0 | view frame in backing pixels | SDL_GL_GetDrawableSize |
| 3de020(title, x, y, w, h, a6, a7, a8) | 2a5650 | windowed NSWindow + OpenGLView; returns the window index | show the SDL window, return 0 |
| 3dd5e0 | 2a5650 | fullscreen window on the main display, mode switch, hide cursor | FULLSCREEN_DESKTOP, hide cursor |
| 3dfb70 | 3171b0 | switch to fullscreen | SDL_SetWindowFullscreen |
| 3dcdf0(idx, w, h) | 2a5960 | set view frame, clear both buffers black, viewport, raise | same with SDL and host GL |
| 3df250(idx, x, y, w, h) | 2a5960 | set window frame, show | SDL_SetWindowSize, show, raise |
| 3df4e0(idx) | 2a5960 | order front, activate app | SDL_RaiseWindow |
| 3de7f0(idx) | 25 sites | make the GL context current | SDL_GL_MakeCurrent |
| 3de850(idx) | 2a62d0 | swap buffers | SDL_GL_SwapWindow |
| 3df0a0(idx) | 3171b0 | window has focus | SDL_WINDOW_INPUT_FOCUS |
| 3de8b0 | 1d7e00 | per-frame event pump; returns the window count | SDL_PollEvent loop (section c) |
| 3df570 | 2121a0 | home directory with a trailing `/`, in buffer 0x171c338 | `$HOME` |
| 3df8f0(title, msg) | 2a5270 | NSRunAlertPanel | SDL_ShowSimpleMessageBox |

Kept lifted: 3db010, 3db030, 3dba80, 3dbab0, 3dbad0 (Steam); 3dbed0, 3df780, 3df8d0, 3dff50, 3e1f40 (no platform calls); 3df640, 3df6d0, 3df830 (CG fade and capture only, served by the thunks in section d); 3dc040, 3dc1c0, 3dc730, 3dc8b0 (keyboard layout and locale through TIS, UCKeyTranslate and CFLocale; their only ObjC use is an NSAutoreleasePool, see section d).

Not confirmed: the meaning of 3de020 arguments a6–a8 (they go to `initWithFrame:shareContext:...`) and of the 3dd5e0 arguments. Both functions log them. The 3dff70 receiver is read from the stret call; it is assumed to be the active view.

## (b) How callers reach the native functions

Direct calls in generated code are plain C calls to `sub_XXXXXXXX`, not dispatch lookups. So registering in `recomp_lookup_manual` alone would not catch them. Two parts:

1. `frontend_rename.h` is force-included only when compiling `hobbit_0025.c`. It renames the 19 lifted definitions to `lifted_sub_*`. Every direct caller and `hobbit_dispatch.c` then bind to the native `sub_*` in `frontend_sdl.c`. Generated code is not edited. A plain build fails with 19 duplicate symbols, which proves the set is exact.
2. `recomp_lookup_manual` (frontend_sdl.c) maps the same 19 guest addresses for indirect calls. The stub in `recomp_glue.c` is removed.

Guest state the native code keeps up to date: window count 0x182a37c (1 after open), window table 0x171c29c, display ID 0x182a388, retina flag 0x182a398 (0), screen size 0x161b2f0/0x161b2f4, focus byte 0x161b49c.

## (c) Input

The game does not read keyboard or mouse through IOKit. The front end writes engine state directly:

- Keyboard: two 0x100-byte key arrays at 0x171be9c, current index at 0x171be98. Each frame 3de8b0 copies the current array to the other and flips the index (3dec60), then sets bytes on key down and clears them on key up. The index is the PC set-1 scan code: the modifier cases in 3ded90 are exactly 0x1d, 0x2a, 0x36, 0x38, 0x9d, 0xb8, 0xdb, 0xdc. `frontend_keymap.c` maps SDL scancodes to set-1 codes. Cmd+Q and SDL_QUIT set the quit byte at `*(0x11db948)`. A focus change calls the lifted 3df0e0(0), which clears the modifier keys.
- Mouse: the OpenGLView handlers write through pointer slots. `*(0x161b6a0)`, `*(0x161b6a4)` hold x and y in view coordinates (origin bottom left, so y = height − SDL y). `*(0x161b69c)` holds button bits (1 left, 2 right, 4 other). `*(0x161b6a8)` holds the last wheel deltaY as a float.
- Gamepad: SDL_INIT_GAMECONTROLLER is on, but no state is fed yet. The game reads pads through IOKit HID (callers 112f9f5, 1130070, 112947a, 112f739). The proposed hook is to replace the HID enumeration and value-read function the engine calls with SDL_GameController reads. That is a later step.

Reference: the saga SDL host layer (opensagadev/saga, a 2013 build of the same engine), `src/host/platform/keyboard.cpp` and `mouse.cpp`. It confirmed that the key index is the set-1 code. It was read only; no saga code is in this repo. `frontend_keymap.c` was written from the public set-1 table. An attempt to adapt the saga table was refused by the permission system.

## (d) Import thunks

All are in `frontend_sdl.c`.

- `_objc_msgSend`: allows only `+[NSAutoreleasePool alloc]` and then `-init` and `-drain` on that pool (for the kept 3dc1c0, 3dc730 and 3dc8b0). Any other message prints the selector, the receiver and the guest return address, then aborts. `_stret`, `_fpret` and `_msgSendSuper` always print and abort.
- `_NSLog` prints the constant format string. `_NSRunAlertPanel` prints and returns 1. `_NSHomeDirectory` aborts (only the replaced 3df570 called it).
- CGDisplay: main ID 1; capture, release, set mode and fades succeed at once; `CopyDisplayMode` returns a token; `ModeGetWidth/Height` return the desktop size; `GetRefreshRate` pushes the SDL rate on x87; `CopyAllDisplayModes` returns a host CFArray holding the host desktop mode; `ModeCopyPixelEncoding` returns a new host CFString `--------RRRRRRRRGGGGGGGGBBBBBBBB` (32-bit direct); hide and show cursor go to SDL_ShowCursor; `CGShieldingWindowLevel` returns 2147483628; `CGWindowLevelForKey` returns 0.
- CGL: every pixel format, context and renderer object is one token for the single SDL context. `CGLCreateContext` creates the SDL window hidden if it does not exist yet, so renderer probing before the window opens has a current context. `SetCurrentContext` maps to SDL_GL_MakeCurrent, `FlushDrawable` to SDL_GL_SwapWindow, and `SetParameter(kCGLCPSwapInterval)` to SDL_GL_SetSwapInterval. `QueryRendererInfo` and `DescribeRenderer` report one accelerated renderer with 1024 MB.

Not checked: the callers outside the block of `CGDisplayModeCopyPixelEncoding` and of the CGL renderer queries (2a4eb0). A NULL pixel encoding could fail there.

## (e) Data imports

`frontend_data_init()` registers `_NSApp`, `_NSDefaultRunLoopMode`, `_NSWindowWillCloseNotification` and `_NSViewGlobalFrameDidChangeNotification`, each as a zeroed guest word. Only replaced code read them.

## Gamepad

Code: `runtime/src/frontend_pad.c`. Overrides: `recon/native_overrides.txt`.

**Hook choice.** The HID Utilities library (0x1128000-0x1130216, chunk 0122) builds device and element lists through IOKit. Replacing 1130070 or 112f9f5 would mean faking those lists and the IOHIDDeviceInterface calls. The game's own pad layer is one level up, in chunk 0024, and uses the library only through device records (pRecDevice). Five functions are replaced:

| VA | Role |
|---|---|
| 3d18a0(out[], max) | List pads (usage page 1, usages 5, 4, 8), set the claimed byte, return the count. Called from 1afa79. |
| 3d19c0(out[], max) | Hot-plug scan: pads not yet claimed. Called from 3d85d6. |
| 3d1a80(dev) | True for Xbox-type pads (vendor 0x162E/0xBEEF, or 0x45E with product 0x28E, 0x28F, 0x291, 0x719). Native returns 1. |
| 3d1ae0(dev) | Release. Native frees the slot of an unplugged pad. |
| 3d1af0(state, dev) | Fill the 0x110-byte pad state; return 0 if the pad is gone. Called from 3d6e60. |

3d6e60 (dead zones, Xbox quirks, the game's output struct) and everything above it stay lifted. 3d1c00 (the HID element walk) is no longer called.

Direct calls inside chunk 0024 bind to the native functions because the lifter emits the five lifted definitions as weak symbols (VAs listed in `recon/native_overrides.txt`) and `frontend_pad.c` defines them strong.

**Device record** (guest memory, 0x470 bytes, 8 slots): +0x114 vendor, +0x118 product, +0x220 name (0x40), +0x424 usage (5), +0x428 usage page (1), +0x440 axis count (6), +0x444 button count (17), +0x448 hat count (1), +0x458 gone, +0x459 claimed. Every SDL game controller reports vendor 0x45E, product 0x28E, so the game takes its Xbox branch. Controllers are opened on SDL_CONTROLLERDEVICEADDED (an event watch queues the event; the next pad call opens it) and closed on SDL_CONTROLLERDEVICEREMOVED. Pads already present at first use are opened by a scan.

**Pad state** (read from 3d1c00 and 3d6e60):

| Offset | Content | Values |
|---|---|---|
| +0x00 | X (left stick x) | int32, -32768..32767 |
| +0x04 | Y (left stick y) | same; sign unconfirmed, SDL's kept (negative up) |
| +0x08 | Z (left trigger) | 0..32767 |
| +0x0C, +0x10 | Rx, Ry (right stick) | as X, Y |
| +0x14 | Rz (right trigger) | 0..32767 |
| +0x18, +0x1C | extra axes | 0, not mapped |
| +0x20..+0x2C | 4 hats | centidegrees, N 0, E 9000, S 18000, W 27000, neutral -1; the D-pad is hat 0 |
| +0x30..+0xAF | buttons | 0 or 0xFF, one byte per button |
| +0xB0..+0x10C | unused | 0 |

3d6e60 turns a hat value into directions with `hat / 4500`.

**Buttons** (byte index: SDL button): 0-1 unused, 2 D-pad up, 3 down, 4 left, 5 right, 6 Start, 7 Back, 8 left stick, 9 right stick, 10 right shoulder, 11 left shoulder, 12 Guide, 13 A, 14 B, 15 X, 16 Y. The game binds actions by byte index through `Contents/Resources/ControllerMappings.txt` (`PADDIGITAL,n` reads byte n), and this order is its entry for 045e:028e. The PS3 entry in the same file matches the DualShock 3 HID order, which confirms that n is the 0-based byte. The D-pad is also reported as hat 0. The earlier order (A, B, X, Y in bytes 0-3) left the face buttons unbound.

Not checked: the stick Y sign, the button order against the game's default bindings, any run with a connected controller.

## Build

- CFLAGS: add `-I/opt/homebrew/Cellar/sdl2/2.32.10/include/SDL2 -D_THREAD_SAFE` (from `sdl2-config --cflags`).
- LDFLAGS: add `-L/opt/homebrew/Cellar/sdl2/2.32.10/lib -lSDL2`.
- For `hobbit_0025.o` only: `$(OBJ)/gen_hobbit_0025.o: CFLAGS += -include $(ROOT)/runtime/include/frontend_rename.h`.

## Known gaps

- SDL video must run on the host main thread. Not checked: whether startup runs the guest entry there.
- The first run stops before window creation: `CFBundleGetFunctionPointerForName("FeralEntry")` returns 0 and the process exits 255. That is in the CF thunks, not the front end.
