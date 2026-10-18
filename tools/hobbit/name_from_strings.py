"""Name stripped functions from the `Class::Member` strings the binary holds.

Usage: python -m tools.hobbit.name_from_strings <analysis.json> <binary> <switch_tables.json>
           -o <names_strings.json>

A candidate string is a whole C string shaped like `NuFoo::Bar` (the engine passes these to
asserts, pool registration and logging), or a format string that starts with such a name and
then `(` or `:` (`NuFoo::Bar: failed %d`). Function references to the strings come from
name_from_saga (push imm32, mov reg imm32, PIC lea).

The last component says what the string is:
- a data member (`m_x`, `s_x`, `mX`, `sX`): the function that refers to it works on `Class`.
  The function gets a class label (confidence "class"); the label may repeat across functions.
- anything else is a function name. The function gets that name (confidence "string") only if
  it is the one function that refers to the string and it refers to no other such name.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

from tools.hobbit import disasm
from tools.hobbit.name_from_saga import map_to_functions, read_cstrings

NAME_PATTERN = r"[A-Za-z_]\w*(?:::[A-Za-z_~]\w*)+"
WHOLE_RE = re.compile(NAME_PATTERN + r"\Z")
PREFIX_RE = re.compile(r"(" + NAME_PATTERN + r")[(:]")
DATA_MEMBER_RE = re.compile(r"(?:[ms]_|sm_|[ms][A-Z])")
CONFIDENCE_STRING = "string"
CONFIDENCE_CLASS = "class"


def candidate_name(text: str) -> str | None:
    """The `Class::Member` name a string carries, or None."""
    if WHOLE_RE.match(text):
        return text
    prefixed = PREFIX_RE.match(text)
    return prefixed[1] if prefixed else None


def split_member(name: str) -> tuple[str, str]:
    """(class, last component) of a `A::B::c` name."""
    owner, _, member = name.rpartition("::")
    return owner, member


def is_data_member(member: str) -> bool:
    return DATA_MEMBER_RE.match(member) is not None


def find_candidates(cstrings: dict[int, str]) -> dict[int, str]:
    """String address -> name, for every candidate string."""
    found: dict[int, str] = {}
    for addr, text in cstrings.items():
        name = candidate_name(text)
        if name:
            found[addr] = name
    return found


def assign_names(
    candidates: dict[int, str], refs: dict[int, set[int]]
) -> tuple[dict[str, dict[str, str]], int]:
    """Apply the naming rules.

    `refs` maps a function start to the candidate string addresses it uses. Returns the names
    (keyed like names_saga.json) and the number of functions skipped for a conflict.
    """
    users: dict[str, set[int]] = {}  # function name -> functions that refer to it
    for func, addrs in refs.items():
        for addr in addrs:
            name = candidates[addr]
            if not is_data_member(split_member(name)[1]):
                users.setdefault(name, set()).add(func)

    names: dict[str, dict[str, str]] = {}
    conflicts = 0
    for func, addrs in sorted(refs.items()):
        func_names: dict[str, int] = {}  # function name -> first string address
        classes: dict[str, int] = {}  # class -> first string address
        for addr in sorted(addrs):
            owner, member = split_member(candidates[addr])
            if is_data_member(member):
                classes.setdefault(owner, addr)
            else:
                func_names.setdefault(candidates[addr], addr)
        key = f"0x{func:X}"
        if func_names:
            (name, addr), *rest = func_names.items()
            if rest or len(users[name]) != 1:
                conflicts += 1
            else:
                names[key] = {
                    "name": name,
                    "confidence": CONFIDENCE_STRING,
                    "evidence": f"0x{addr:X}",
                }
        elif len(classes) == 1:
            ((owner, addr),) = classes.items()
            names[key] = {
                "name": owner,
                "confidence": CONFIDENCE_CLASS,
                "evidence": f"0x{addr:X}",
            }
        elif classes:
            conflicts += 1
    return names, conflicts


def build_names(
    analysis: dict[str, Any], data: bytes, switches: dict[str, Any]
) -> tuple[dict[str, dict[str, str]], int]:
    cstrings = read_cstrings(analysis, data)
    candidates = find_candidates(cstrings)
    mapped = map_to_functions(analysis, data, switches, set(candidates))
    refs = {func: {site.string_addr for site in sites} for func, sites in mapped.items()}
    return assign_names(candidates, refs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("analysis", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("switch_tables", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    names, conflicts = build_names(
        json.loads(args.analysis.read_text()),
        args.binary.read_bytes(),
        json.loads(args.switch_tables.read_text()),
    )
    disasm._write_json_atomic(args.output, names)
    strings = sum(1 for e in names.values() if e["confidence"] == CONFIDENCE_STRING)
    print(f"{strings} string names, {len(names) - strings} class labels, {conflicts} conflicts")


if __name__ == "__main__":
    main()
