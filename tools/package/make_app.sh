#!/bin/sh
# Build "build.nosync/Hobbit Recompiled.app" from build.nosync/hobbit and cghelper.
# Paths are relative to the repo root (two levels above this script).
# The launcher runs the Steam copy of the game binary, so the app works without the repo.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/build.nosync"
OUT_APP="$BUILD/Hobbit Recompiled.app"
# Assemble and sign outside the repo: under ~/Documents the file provider adds
# Finder attributes to new folders at any moment, and codesign rejects them.
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/hobbit-app.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/Hobbit Recompiled.app"
# The launcher name must match CFBundleExecutable in Info.plist.in.
LAUNCHER_EXE=HobbitRecompiled
STEAM_APP="${HOBBIT_APP:-$HOME/Library/Application Support/Steam/steamapps/common/LEGO - The Hobbit/LEGO The Hobbit.app}"

for f in hobbit cghelper; do
    [ -x "$BUILD/$f" ] || { echo "make_app: missing $BUILD/$f (build first)" >&2; exit 1; }
done

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/hobbit" "$APP/Contents/MacOS/hobbit"
cp "$BUILD/cghelper" "$APP/Contents/MacOS/cghelper"
cp "$ROOT/tools/package/Info.plist.in" "$APP/Contents/Info.plist"

# Bundle SDL2 (and SDL3 below) so the app does not need Homebrew at run time.
# "brew --prefix sdl2" can print an alias path without lib/, so use the path hobbit links.
SDL_LINKED=$(otool -L "$APP/Contents/MacOS/hobbit" | awk '/libSDL2-2\.0\.0\.dylib/ {print $1; exit}')
[ -n "$SDL_LINKED" ] || { echo "make_app: hobbit does not link libSDL2-2.0.0.dylib" >&2; exit 1; }
SDL_SRC="$SDL_LINKED"
if [ ! -f "$SDL_SRC" ]; then
    SDL_SRC="$(brew --prefix)/opt/sdl2/lib/libSDL2-2.0.0.dylib"
fi
[ -f "$SDL_SRC" ] || { echo "make_app: SDL2 not found (run: brew install sdl2)" >&2; exit 1; }
SDL_NEW="@executable_path/../Frameworks/libSDL2-2.0.0.dylib"
mkdir -p "$APP/Contents/Frameworks"
cp "$SDL_SRC" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -id "$SDL_NEW" "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -change "$SDL_LINKED" "$SDL_NEW" "$APP/Contents/MacOS/hobbit"
codesign --force -s - "$APP/Contents/Frameworks/libSDL2-2.0.0.dylib"

# Homebrew's sdl2 is now sdl2-compat: SDL2 on top of SDL3, loaded at run time.
# It looks for SDL3 first as libSDL3.dylib next to itself, so bundle SDL3 there.
if grep -q 'Failed loading SDL3 library' "$SDL_SRC"; then
    SDL3_SRC="$(brew --prefix)/opt/sdl3/lib/libSDL3.0.dylib"
    [ -f "$SDL3_SRC" ] || { echo "make_app: SDL3 not found (run: brew install sdl3)" >&2; exit 1; }
    cp "$SDL3_SRC" "$APP/Contents/Frameworks/libSDL3.dylib"
    chmod u+w "$APP/Contents/Frameworks/libSDL3.dylib"
    install_name_tool -id "@loader_path/libSDL3.dylib" "$APP/Contents/Frameworks/libSDL3.dylib"
    codesign --force -s - "$APP/Contents/Frameworks/libSDL3.dylib"
fi

# Icon: copy the game's icon into the local build output (never committed).
ICON="$STEAM_APP/Contents/Resources/GameIcon.icns"
if [ ! -f "$ICON" ]; then
    ICON=$(ls "$STEAM_APP"/Contents/Resources/*.icns 2>/dev/null | grep -v DocIcon | head -n 1 || true)
fi
if [ -n "$ICON" ] && [ -f "$ICON" ]; then
    cp "$ICON" "$APP/Contents/Resources/AppIcon.icns"
    plutil -insert CFBundleIconFile -string AppIcon "$APP/Contents/Info.plist"
fi

# The launcher defaults to the game path in effect now; the path is single-quote-escaped.
DEFAULT_ESC=$(printf '%s' "$STEAM_APP" | sed "s/'/'\\\\''/g")
{
    echo '#!/bin/sh'
    echo "DEFAULT_HOBBIT_APP='$DEFAULT_ESC'"
    cat <<'LAUNCHER'
DIR=$(cd "$(dirname "$0")" && pwd)
HOBBIT_APP="${HOBBIT_APP:-$DEFAULT_HOBBIT_APP}"
export HOBBIT_APP
GAME="$HOBBIT_APP/Contents/MacOS/LEGO The Hobbit"
if [ ! -f "$GAME" ]; then
    MSG="LEGO The Hobbit was not found at: $HOBBIT_APP. Install it from Steam, or set HOBBIT_APP to its .app bundle."
    echo "Hobbit Recompiled: $MSG" >&2
    if [ "${HOBBITRECOMP_NO_GUI:-}" != 1 ]; then
        osascript - "$MSG" >/dev/null 2>&1 <<'APPLESCRIPT' || true
on run argv
    display alert "Hobbit Recompiled" message (item 1 of argv)
end run
APPLESCRIPT
    fi
    exit 1
fi
LOGDIR="$HOME/Library/Logs/HobbitRecomp"
mkdir -p "$LOGDIR"
# Keep the newest 20 logs; the run below adds one more.
ls -1t "$LOGDIR"/run-*.log 2>/dev/null | tail -n +20 | while IFS= read -r old; do rm -f "$old"; done
ls -1t "$LOGDIR"/frames-*.csv 2>/dev/null | tail -n +20 | while IFS= read -r old; do rm -f "$old"; done
STAMP=$(date +%Y-%m-%d_%H%M%S)
# Frame times go to a CSV next to the run log only when asked: HOBBITRECOMP_FRAMELOG=1,
# or RECOMP_FRAMELOG=<file> to choose the file.
if [ "${HOBBITRECOMP_FRAMELOG:-}" = 1 ]; then
    : "${RECOMP_FRAMELOG:=$LOGDIR/frames-$STAMP.csv}"
fi
[ -n "${RECOMP_FRAMELOG:-}" ] && export RECOMP_FRAMELOG
exec "$DIR/hobbit" "$GAME" >"$LOGDIR/run-$STAMP.log" 2>&1
LAUNCHER
} > "$APP/Contents/MacOS/$LAUNCHER_EXE"
chmod +x "$APP/Contents/MacOS/$LAUNCHER_EXE"

xattr -cr "$APP"
codesign --force --deep -s - "$APP"
rm -rf "$OUT_APP" "$BUILD/HobbitRecomp.app"   # the second is the bundle's old name
ditto "$APP" "$OUT_APP"
echo "built $OUT_APP"
