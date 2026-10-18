#!/bin/sh
# Pre-publication check: looks for game-derived or private material in tracked files.
# Run from anywhere; it checks the repository this script belongs to.
# Optional: ~/.config/hobbitrecomp/release_denylist.txt, one string per line.
# Only the matching file paths and line numbers are printed, never the strings.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

FAILS=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILS=$((FAILS + 1)); }
warn() { echo "WARN  $1"; }

if ! git ls-files > "$TMP/tracked" 2>/dev/null; then
    echo "release_check: not a git repository"
    exit 1
fi

# 1. Tracked paths that must not be published.
grep -E '^(docs/research/pseudocode/|orig\.nosync/|generated\.nosync/|build\.nosync/|ghidra\.nosync/)' "$TMP/tracked" > "$TMP/bad_dirs" || true
if [ -s "$TMP/bad_dirs" ]; then
    fail "tracked files in game-derived folders ($(wc -l < "$TMP/bad_dirs" | tr -d ' ') files, first 10 below)"
    head -n 10 "$TMP/bad_dirs" | sed 's/^/        /'
else
    pass "no tracked files in game-derived folders"
fi

# 2. Tracked game data, libraries and executables.
: > "$TMP/bad_bin"
while IFS= read -r f; do
    [ -f "$f" ] || continue
    case "$f" in
        *.DAT|*.dat|*.dylib) echo "$f" >> "$TMP/bad_bin"; continue ;;
    esac
    if file -b "$f" | grep -q 'Mach-O'; then
        echo "$f" >> "$TMP/bad_bin"
    fi
done < "$TMP/tracked"
if [ -s "$TMP/bad_bin" ]; then
    fail "tracked game data, dylib or Mach-O files"
    head -n 10 "$TMP/bad_bin" | sed 's/^/        /'
else
    pass "no tracked .DAT/.dat/.dylib/Mach-O files"
fi

# Text files only for the content checks below.
: > "$TMP/text"
while IFS= read -r f; do
    [ -f "$f" ] || continue
    if grep -Iq . "$f" 2>/dev/null; then
        echo "$f" >> "$TMP/text"
    fi
done < "$TMP/tracked"

# 3. Absolute home paths of a real user.
: > "$TMP/home_hits"
while IFS= read -r f; do
    grep -nE '/Users/[A-Za-z0-9._-]+' "$f" 2>/dev/null \
        | grep -vE '/Users/(\$USER|\$\{USER\}|<[^>]*>|you|user|username|name|yourname|Shared)([^A-Za-z0-9._-]|$)' \
        | cut -d: -f1 | sed "s|^|$f:|" >> "$TMP/home_hits" || true
done < "$TMP/text"
if [ -s "$TMP/home_hits" ]; then
    fail "absolute home paths in tracked text (file:line, first 10)"
    head -n 10 "$TMP/home_hits" | sed 's/^/        /'
else
    pass "no absolute home paths in tracked text"
fi

# 4. Local denylist.
DENY="$HOME/.config/hobbitrecomp/release_denylist.txt"
if [ -f "$DENY" ]; then
    : > "$TMP/deny_hits"
    while IFS= read -r f; do
        grep -nFif "$DENY" "$f" 2>/dev/null | cut -d: -f1 | sed "s|^|$f:|" >> "$TMP/deny_hits" || true
    done < "$TMP/text"
    if [ -s "$TMP/deny_hits" ]; then
        fail "denylist strings found (file:line, first 10)"
        head -n 10 "$TMP/deny_hits" | sed 's/^/        /'
    else
        pass "no denylist strings in tracked text"
    fi
else
    echo "SKIP  no local denylist at ~/.config/hobbitrecomp/release_denylist.txt"
fi

# 5. History.
git log --all --format=%h -- docs/research/pseudocode | head > "$TMP/hist"
if [ -s "$TMP/hist" ]; then
    fail "commits touched docs/research/pseudocode (first 10: $(tr '\n' ' ' < "$TMP/hist")). Rewrite the history or publish from a fresh repository."
else
    pass "no history for docs/research/pseudocode"
fi

# Every path ever committed in any branch, in game-derived folders or game data/library files.
git log --all --name-only --format= | sort -u \
    | grep -E '^(orig\.nosync/|generated\.nosync/|build\.nosync/|ghidra\.nosync/|docs/research/pseudocode/)|\.(DAT|dat|dylib)$' \
    > "$TMP/hist_paths" || true
if [ -s "$TMP/hist_paths" ]; then
    fail "history contains game-derived paths ($(wc -l < "$TMP/hist_paths" | tr -d ' ') paths, first 10 below)"
    head -n 10 "$TMP/hist_paths" | sed 's/^/        /'
else
    pass "no game-derived paths in history"
fi

# Added lines in history that hold an absolute home path of a real user.
# Prints commit and file only, never the line.
git log --all -p --no-color --format='@@commit %h' | awk '
    /^@@commit / { commit = $2; file = ""; next }
    /^\+\+\+ / { file = substr($0, 7); next }
    /^\+/ {
        rest = $0
        while (match(rest, /\/Users\/[A-Za-z0-9._-]+/)) {
            name = substr(rest, RSTART + 7, RLENGTH - 7)
            rest = substr(rest, RSTART + RLENGTH)
            if (name == "USER" || name == "Shared" || name == "you" || name == "user" || \
                name == "username" || name == "name" || name == "yourname") continue
            print commit " " file
            break
        }
    }' | sort -u > "$TMP/hist_homes"
if [ -s "$TMP/hist_homes" ]; then
    fail "absolute home paths in added lines of history ($(wc -l < "$TMP/hist_homes" | tr -d ' ') commit/file pairs, commit file, first 10 below)"
    head -n 10 "$TMP/hist_homes" | sed 's/^/        /'
else
    pass "no absolute home paths in history"
fi

# 6. recon/.
if [ -d recon ]; then
    pass "recon/ is present (addresses and names from the game binary; the owner decided to publish it, as the translator needs it)"
fi

echo
if [ "$FAILS" -gt 0 ]; then
    echo "release_check: $FAILS check(s) FAILED"
    exit 1
fi
echo "release_check: all checks passed"
