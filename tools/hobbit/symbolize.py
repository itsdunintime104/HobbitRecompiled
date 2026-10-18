"""Rewrite guest addresses in a run log into function names.

Usage: some_command | python -m tools.hobbit.symbolize [--recon recon]

Each `0x` number of 6 to 8 hex digits that falls inside a function is followed by
`[name]` or `[name+0xOFF]` (OFF is the distance from the function start). Other numbers
stay as they are.

The containing function is the one with the largest start at or below the address, if the
address is inside the code (before the end of the last Ghidra function). The name comes from
the first of these that has one:
1. names_strings.json, confidence "string"
2. names_saga.json name
3. ghidra_functions.json name that is not `FUN_...`
4. names_strings.json, confidence "class" (shown as `Class::*`)
5. names_saga.json file label (`file:line`)
Otherwise the lifted symbol name `sub_XXXXXXXX` is shown.
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
import sys
from pathlib import Path
from typing import Any, NamedTuple

ADDRESS_RE = re.compile(r"0x[0-9A-Fa-f]{6,8}(?![0-9A-Fa-f])")
DEFAULT_RECON = Path(__file__).resolve().parents[2] / "recon"
GHIDRA_UNNAMED_PREFIX = "FUN_"
CONFIDENCE_STRING = "string"
CONFIDENCE_CLASS = "class"


class Symbols(NamedTuple):
    starts: list[int]  # sorted function starts
    code_end: int  # first address past the last known function
    strings: dict[int, dict[str, Any]]  # names_strings.json by function start
    saga: dict[int, dict[str, Any]]  # names_saga.json by function start
    ghidra: dict[int, str]  # ghidra_functions.json names that are not FUN_


def _by_address(table: dict[str, dict[str, Any]]) -> dict[int, dict[str, Any]]:
    return {int(key, 16): value for key, value in table.items()}


def load_symbols(recon: Path) -> Symbols:
    starts = sorted(
        int(line, 16) for line in (recon / "function_starts.txt").read_text().split()
    )
    ghidra_funcs = json.loads((recon / "ghidra_functions.json").read_text())
    return Symbols(
        starts=starts,
        code_end=max(f["addr"] + f["size"] for f in ghidra_funcs),
        strings=_by_address(json.loads((recon / "names_strings.json").read_text())),
        saga=_by_address(json.loads((recon / "names_saga.json").read_text())),
        ghidra={
            f["addr"]: f["name"]
            for f in ghidra_funcs
            if not f["name"].startswith(GHIDRA_UNNAMED_PREFIX)
        },
    )


def best_name(symbols: Symbols, start: int) -> str:
    """The best name or label for the function at `start`."""
    strings = symbols.strings.get(start)
    saga = symbols.saga.get(start)
    if strings and strings["confidence"] == CONFIDENCE_STRING:
        return strings["name"]
    if saga and saga.get("name"):
        return saga["name"]
    if start in symbols.ghidra:
        return symbols.ghidra[start]
    if strings and strings["confidence"] == CONFIDENCE_CLASS:
        return f"{strings['name']}::*"
    if saga:
        return f"{saga['file']}:{saga['line']}"
    return f"sub_{start:08X}"


def resolve(symbols: Symbols, va: int) -> str | None:
    """`name` or `name+0xOFF` for the function that holds `va`; None if no function does."""
    index = bisect.bisect_right(symbols.starts, va) - 1
    if index < 0 or va >= symbols.code_end:
        return None
    start = symbols.starts[index]
    name = best_name(symbols, start)
    return name if va == start else f"{name}+0x{va - start:X}"


def symbolize_text(symbols: Symbols, text: str) -> str:
    def rewrite(match: re.Match[str]) -> str:
        label = resolve(symbols, int(match[0], 16))
        return match[0] if label is None else f"{match[0]} [{label}]"

    return ADDRESS_RE.sub(rewrite, text)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recon", type=Path, default=DEFAULT_RECON)
    args = parser.parse_args()
    symbols = load_symbols(args.recon)
    for line in sys.stdin:
        sys.stdout.write(symbolize_text(symbols, line))


if __name__ == "__main__":
    main()
