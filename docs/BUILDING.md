# Building and development

`./setup.sh` does everything below for players. Use these steps when you work on the code.

## Contents

- [How it works](#how-it-works)
- [Build by hand](#build-by-hand)
- [Options](#options)
  - [The overlay](#the-overlay)
- [Tests](#tests)
- [Repository layout](#repository-layout)
- [Design notes](#design-notes)
- [Before a public release](#before-a-public-release)

## How it works

1. `tools/hobbit/analyze.py` reads the game's Mach-O binary and records its functions, imports and switch tables in `recon/`.
2. `tools/xrecomp/` translates every x86 function into C. It is a fork of [xboxrecomp](https://github.com/sp00nznet/xboxrecomp); local changes are listed in `tools/xrecomp/UPSTREAM.md`. The output goes to `generated.nosync/` (about 330 MB, never committed).
3. `runtime/` replaces the game's 378 macOS imports:
   - CoreFoundation and the OpenGL/CGL calls, over the host frameworks;
   - the Cocoa front end, replaced by SDL2;
   - CoreAudio, threads and files;
   - an offline stand-in for the Steam API.

   Guest memory reads use ordered arm64 loads, so threads see memory in x86 order.
4. clang compiles both into `build.nosync/hobbit`. At run time it loads the data segments of the original binary into guest memory, and reads the game's asset files from the Steam install.

| Check | Result |
|---|---|
| Functions translated | 61,636 of 61,636 |
| Untranslated instructions in reachable code | 0 (219 stop sites remain, all in data that the disassembler read as code) |
| Differential fuzzing against Unicorn (an x86 emulator) | 13,390 of 13,400 tested functions match; the other 10 differ only in the sign bit of a NaN |
| Test suite | 304 passed, 10 expected failures (known flag cases with no game sites) |

## Build by hand

```sh
python3 -m venv .venv
.venv/bin/pip install capstone unicorn pytest

mkdir -p orig.nosync
cp "$HOME/Library/Application Support/Steam/steamapps/common/LEGO - The Hobbit/LEGO The Hobbit.app/Contents/MacOS/LEGO The Hobbit" orig.nosync/hobbit_i386
shasum -a 256 orig.nosync/hobbit_i386   # must start with 01809d9d1d50b4d1

.venv/bin/python -m tools.xrecomp.run --analysis recon/analysis.json \
    --switch recon/switch_tables.json --bin orig.nosync/hobbit_i386 \
    --out generated.nosync --jobs 8
make -C build -j10
make -C build app   # "build.nosync/Hobbit Recompiled.app"
```

Run the binary directly:

```sh
build.nosync/hobbit orig.nosync/hobbit_i386
```

`make` tracks headers but not compiler flags. After you change a flag, delete `build.nosync/obj/*.o` before you build again. To build the heap allocator with its own checks, delete `build.nosync/obj/rt_dlmalloc.o` and run `make -C build -j10 HEAP_DEBUG=1`.

## Options

Set these as environment variables before `build.nosync/hobbit`.

| Variable | Effect |
|---|---|
| `HOBBIT_APP=<path>` | The game's `.app` bundle, if it is not in the default Steam folder |
| `HOBBIT_SAVE_DIR=<path>` | The save folder |
| `RECOMP_HUD=1` | Show the overlay at start (Cmd+F toggles it). See [The overlay](#the-overlay) |
| `RECOMP_FRAMELOG=<file>` | Write every frame time as CSV (`t_s,frame_ms`). The app does this only with `HOBBITRECOMP_FRAMELOG=1`, next to its run log. Plot with `.venv/bin/python -m tools.perf.plot_frames <file>` |
| `RECOMP_ASSUME_FOCUS=1` | Treat the window as focused. Use this for runs started by a script, which macOS does not give focus to |
| `RECOMP_HEAP_GUARD=1` | Check every guest heap block for overruns, use after free and double free; stop at the first finding. `2` logs each finding once and continues. Costs about 8% speed |
| `RECOMP_TRACE_OPENS=1` | Log every file open and its result (`2` adds the guest call chain) |
| `RECOMP_FRAME_DUMP=N` | Save the back buffer on frames N, 2N, 4N and so on (up to 8 files), as PPM, to `RECOMP_FRAME_DIR` (default: the current folder) |
| `RECOMP_RENDER_SIZE=WxH` | Render at this size (64 to 8192 per side), for example `3456x2234`. macOS scales it to the window. Overrides the in-game resolution setting. Unset: the size chosen in the game's Options, else the display size |
| `RECOMP_HANGWATCH=N` | Report threads that make no progress for N seconds |
| `RECOMP_LOCKWATCH=N` | Report mutexes held for N seconds |
| `RECOMP_STARTUP_VERBOSE=1` | Log each constructor and each unbound data import at startup |
| `RECOMP_DLSYM_VERBOSE=1` | Log every GL extension lookup, not only the failed ones |
| `RECOMP_NO_ALERT_UI=1` | Print game alerts to stderr instead of showing a dialog |

### The overlay

The text refreshes twice a second. Each frame only replays it, so the overlay costs almost no frame time.

| Line | Meaning |
|---|---|
| `FPS`, `1% LOW` | Frames per second over the last second; the frame rate at the 99th-percentile frame time of the last 240 frames |
| `AVG`, `P99`, `MAX` | Mean frame time over the last second; 99th-percentile and worst frame time of the last 240 frames |
| `OVER 33 MS` | Frames of the last 240 that took longer than 33.3 ms (below 30 FPS) |
| `GAME`, `SWAP` | Mean time per frame spent before the buffer swap (game, driver and overlay work) and inside the swap (waiting for the display) |
| `DRAWS`, `MAPS` | GL draw calls and buffer maps per frame |
| `RENDER`, `WINDOW` | The GL drawable size, the window size in points, and their ratio (1.00x: no Retina rendering) |
| `VSYNC`, `TSO`, `-O`, `HEAP GUARD` | Settings that change speed: swap interval, x86 memory ordering (`TSO=1`), the optimisation level (`make OPT=1` for a faster compile), and `RECOMP_HEAP_GUARD` |

When the game stops on an error, the last log lines name the cause and the guest address (`RECOMP: caller=...`).

## Tests

```sh
.venv/bin/python -m pytest -q tests
```

The suite takes about a minute. Tests that need the game binary find it at `orig.nosync/hobbit_i386`, or through `HOBBIT_BIN`.

The differential fuzzer runs real game functions on Unicorn and as translated C, and compares the results. A full run takes about 6 minutes on 8 cores:

```sh
.venv/bin/python -m tools.hobbit.fuzz_leaf --binary orig.nosync/hobbit_i386 --work /tmp/hobbit-fuzz
```

## Repository layout

| Path | Contents |
|---|---|
| `setup.sh` | One-command setup: checks, translation, build, app install |
| `tools/hobbit/` | Mach-O analysis, import signatures, code generators, the fuzzer |
| `tools/xrecomp/` | The x86-to-C translator (forked from xboxrecomp) |
| `tools/cghelper/` | The x86_64 helper that runs the game's Cg shader compiler |
| `tools/glcount/` | A GL call counter, loaded with `DYLD_INSERT_LIBRARIES` |
| `tools/package/` | The `.app` builder and the release check |
| `runtime/` | The replacements for macOS, Steam, OpenGL and audio, plus startup and the front end |
| `recon/` | Analysis data: functions, imports, switch tables, names |
| `build/Makefile` | Compiles `generated.nosync/` and `runtime/` into `build.nosync/` |
| `tests/` | Conformance tests against Unicorn, runtime and thunk tests |
| `docs/` | Design and debugging notes |

## Design notes

- `docs/STARTUP_REQS.md`, `docs/FERAL_LOADER.md`: startup order and the Feral entry point.
- `docs/FRONTEND.md`: the SDL2 front end and gamepad input.
- `docs/GL_CAPS.md`: the GL capability flags behind the Shader Model 3 alert.
- `docs/THUNK_AUDIT.md`: thunk return values checked against the game's call sites.

## Before a public release

Run `tools/package/release_check.sh`. It fails when the tracked files or any commit in the history hold game files, generated code, the Ghidra pseudocode in `docs/research/pseudocode/`, or home folder paths. Do not make this repository public until it passes. The pseudocode is in the git history, so publish from a fresh repository or rewritten history, not by changing this repository's visibility.
