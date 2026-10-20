# Startup requirements collected from the thunk slices

The startup driver (the code that runs before the guest `main`) must do these, in this order:

1. Call `guest_mem_init()` and `guest_load_image()`.
2. Set `g_xbox_mem_offset = g_guest_base`. `MEM32` and the other memory macros add this offset. `recomp_glue.c` leaves it at 0, so every thunk would read the wrong stack (T2).
3. Call `thunk_files_init()`. It maps the static guest area at GUEST_STATIC_BASE (derived per game: the first free 64 KB above the image; The Hobbit: 0x019D0000) (0x4000 bytes) and creates the stdin, stdout and stderr handles (T3).
4. Bind the non-lazy pointers for the data imports `___stdinp`, `___stdoutp` and `___stderrp` to `GUEST_STDINP_ADDR`, `GUEST_STDOUTP_ADDR` and `GUEST_STDERRP_ADDR` from `handles.h` (T3).
5. Before the first allocation: the guest heap at 0x20000000–0xDFFFFFFF is created on first use, through `pthread_once` (T2).
6. At guest exit, call `thunk_rt_run_atexit()` so that the destructors registered through `___cxa_atexit` run in reverse order (T5).
7. Set the main thread's `g_fp_control_word = 0x037F` and `g_mxcsr = 0x1F80`, the Darwin i386 defaults. New guest threads already start with these values (T6).
8. Keep the main guest stack and any scratch memory out of GUEST_STATIC_BASE (derived per game: the first free 64 KB above the image; The Hobbit: 0x019D0000) to GUEST_STATIC_BASE + 0x4000 (T3).

Known gaps:

- `%Lf` (x87 long double) is not supported in the printf thunks (T4).
- `fcntl` failures set host errno only, not the guest errno slot (T4).
- `getpwuid` and `gethostbyname` return 0 with ENOSYS (T3).
- The `tm_zone` table holds 16 names. When it is full, `tm_zone` is 0 (T3).
- `__Znwm` and `__Znam` abort when the heap is exhausted. The guest has no exception path (T2).
- The vendored dlmalloc 2.8.6 is licensed MIT-0 (relicensed in 2023). It is at `engine/runtime/src/dlmalloc.c`.
- The `SteamUser`, `SteamApps` and other Steam interface accessors return 0. If the game dereferences one, it needs a fake interface object (T5).
- `dlopen` and `dlsym` return 0. GL extension lookups may need real values (T5).
