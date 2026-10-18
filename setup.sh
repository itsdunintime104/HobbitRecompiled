#!/bin/sh
# One-command setup: checks the machine, translates your own copy of the game,
# builds the native program and installs "Hobbit Recompiled.app".
# Usage: ./setup.sh [--check] [--force] [--no-install] [--help]
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

GAME_SHA_PREFIX=01809d9d1d50b4d1
DEFAULT_APP="$HOME/Library/Application Support/Steam/steamapps/common/LEGO - The Hobbit/LEGO The Hobbit.app"
STEAM_APP="${HOBBIT_APP:-$DEFAULT_APP}"
GAME_BIN="$STEAM_APP/Contents/MacOS/LEGO The Hobbit"
ORIG="$ROOT/orig.nosync/hobbit_i386"
GEN="$ROOT/generated.nosync"

CHECK_ONLY=0
FORCE=0
INSTALL=1

usage() {
    cat <<USAGE
Usage: ./setup.sh [options]

Builds "Hobbit Recompiled.app" from your own Steam copy of LEGO The Hobbit (build 84019).

Options:
  --check       Check the requirements only, then stop.
  --force       Translate again even if generated.nosync exists.
  --no-install  Build the app but do not copy it to ~/Applications.
  --help        Show this text.

Set HOBBIT_APP to the game's .app bundle if it is not in the default Steam folder.
USAGE
}

for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --force) FORCE=1 ;;
        --no-install) INSTALL=0 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "setup: unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

step() { echo "==> $*"; }
fail() { echo "setup: $*" >&2; exit 1; }

MISSING=0
ok() { printf 'OK       %s\n' "$1"; }
missing() { printf 'MISSING  %s\n         fix: %s\n' "$1" "$2"; MISSING=$((MISSING + 1)); }

# Print the first python3 on PATH that is 3.11 or newer (the default python3 may be older).
find_python() {
    for cand in python3 python3.14 python3.13 python3.12 python3.11; do
        if command -v "$cand" >/dev/null 2>&1 \
            && "$cand" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null; then
            command -v "$cand"
            return
        fi
    done
}

preflight() {
    step "1/7 Checking requirements"

    if [ "$(uname -s)" = Darwin ]; then ok "macOS"; else missing "macOS (this is $(uname -s))" "run this on a Mac"; fi

    if [ "$(uname -m)" = arm64 ]; then ok "Apple Silicon (arm64)"; else missing "Apple Silicon (this is $(uname -m))" "use a Mac with an M-series chip"; fi

    if xcode-select -p >/dev/null 2>&1; then ok "Xcode Command Line Tools"; else missing "Xcode Command Line Tools" "xcode-select --install"; fi

    # "brew --prefix sdl2" can print an alias path without lib/, so build the path from the Homebrew prefix.
    SDL_LIB=""
    if command -v brew >/dev/null 2>&1; then
        SDL_LIB="$(brew --prefix)/opt/sdl2/lib/libSDL2-2.0.0.dylib"
    fi
    if [ -n "$SDL_LIB" ] && [ -f "$SDL_LIB" ]; then
        ok "Homebrew SDL2"
    elif command -v brew >/dev/null 2>&1; then
        missing "Homebrew SDL2" "brew install sdl2"
    else
        missing "Homebrew (needed for SDL2)" "install Homebrew from https://brew.sh, then: brew install sdl2"
    fi

    PYTHON=$(find_python)
    if [ -n "$PYTHON" ]; then
        ok "Python 3.11 or newer ($PYTHON)"
    else
        missing "Python 3.11 or newer" "brew install python"
    fi

    if /usr/bin/pgrep oahd >/dev/null 2>&1 || arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
        ok "Rosetta 2"
    else
        missing "Rosetta 2" "softwareupdate --install-rosetta --agree-to-license"
    fi

    FREE_KB=$(df -k "$ROOT" | awk 'NR==2 {print $4}')
    if [ "${FREE_KB:-0}" -ge 2097152 ]; then
        ok "Free disk space (at least 2 GB)"
    else
        missing "Free disk space (at least 2 GB)" "free up disk space"
    fi

    if [ "$MISSING" -gt 0 ]; then
        echo "setup: $MISSING requirement(s) missing. Run the fix commands above, then run ./setup.sh again." >&2
        exit 1
    fi
}

locate_and_copy_game() {
    step "2/7 Locating the game"
    [ -f "$GAME_BIN" ] || fail "game binary not found: $GAME_BIN
Install LEGO The Hobbit from Steam, or set HOBBIT_APP to its .app bundle."
    echo "found $GAME_BIN"

    step "3/7 Copying and verifying the game binary"
    mkdir -p "$ROOT/orig.nosync"
    if [ -f "$ORIG" ] && cmp -s "$GAME_BIN" "$ORIG"; then
        echo "orig.nosync/hobbit_i386 is up to date"
    else
        cp "$GAME_BIN" "$ORIG"
        echo "copied to orig.nosync/hobbit_i386"
    fi
    SHA=$(shasum -a 256 "$ORIG" | awk '{print $1}')
    case "$SHA" in
        "$GAME_SHA_PREFIX"*) echo "sha256 matches (build 84019)" ;;
        *) fail "the game binary does not match (sha256 $SHA).
Only Steam build 84019 of LEGO The Hobbit (Mac) is supported. In Steam, check that the game is up to date and not on a beta branch." ;;
    esac
}

setup_venv() {
    step "4/7 Python environment"
    if [ -x "$ROOT/.venv/bin/python" ] && "$ROOT/.venv/bin/python" -c 'import capstone, unicorn, pytest' >/dev/null 2>&1; then
        echo ".venv is ready"
    else
        "$PYTHON" -m venv "$ROOT/.venv"
        "$ROOT/.venv/bin/python" -m pip install --quiet capstone unicorn pytest
        echo ".venv created"
    fi
}

translate() {
    step "5/7 Translating the game code"
    if [ -f "$GEN/lift_report.json" ] && [ "$FORCE" -eq 0 ]; then
        echo "generated.nosync is complete; skipping (use --force to translate again)"
        return
    fi
    NCPU=$(sysctl -n hw.ncpu)
    "$ROOT/.venv/bin/python" -m tools.xrecomp.run \
        --analysis recon/analysis.json --switch recon/switch_tables.json \
        --bin "$ORIG" --out "$GEN" --jobs "$NCPU"
}

build() {
    step "6/7 Building"
    NCPU=$(sysctl -n hw.ncpu)
    make -C build -j"$NCPU"
    make -C build app
}

install_app() {
    step "7/7 Installing"
    APP="$ROOT/build.nosync/Hobbit Recompiled.app"
    [ -d "$APP" ] || fail "build did not produce $APP"
    if [ "$INSTALL" -eq 1 ]; then
        DEST="$HOME/Applications/Hobbit Recompiled.app"
        mkdir -p "$HOME/Applications"
        rm -rf "$DEST"
        # -X: leave out extended attributes. A build folder under ~/Documents
        # carries file-sync attributes that fail codesign --strict.
        cp -RX "$APP" "$DEST"
        APP="$DEST"
        echo "installed $APP"
        # The app was called HobbitRecomp.app before. Saves and logs are outside it.
        OLD="$HOME/Applications/HobbitRecomp.app"
        if [ -d "$OLD" ]; then
            rm -rf "$OLD"
            echo "removed the old $OLD"
        fi
    else
        echo "built $APP (not installed)"
    fi
    echo
    echo "Start it by double-clicking $APP, or run: open \"$APP\""
    echo "Logs: $HOME/Library/Logs/HobbitRecomp/"
}

preflight
if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "All requirements are met."
    exit 0
fi
locate_and_copy_game
setup_venv
translate
build
install_app
