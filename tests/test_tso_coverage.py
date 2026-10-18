"""Every non-stack guest access in the lifted code goes through the ordered accessors.

[esp+...] is always stack; [ebp+...] is stack only in a function marked
`frame: ebp proven`.

Needs generated.nosync from a lift made with the current lifter; skipped when absent.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

GENERATED = Path(__file__).resolve().parents[1] / "generated.nosync"
RAW_ACCESS = re.compile(r"\b(S?MEM(8|16|32|64)|MEMF|MEMD)\(")
FUNCTION_START = re.compile(r"^\w+ sub_")
PROVEN_MARKER = "/* frame: ebp proven */"
MAX_SHOWN = 10


def _sources() -> list[Path]:
    files = sorted(GENERATED.glob("*.c"))
    if not files:
        pytest.skip("generated.nosync has no lifted sources")
    return files


def test_only_stack_accesses_stay_plain() -> None:
    hits: list[str] = []
    for path in _sources():
        proven = False
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if FUNCTION_START.match(line):
                proven = False
            elif PROVEN_MARKER in line:
                proven = True
            for match in RAW_ACCESS.finditer(line):
                is_stack = line.startswith("esp", match.end()) or (
                    proven and line.startswith("ebp", match.end()))
                if not is_stack:
                    hits.append(f"{path.name}:{number}: {line.strip()[:160]}")
    assert not hits, f"{len(hits)} raw non-stack accesses; first {MAX_SHOWN}:\n" + "\n".join(hits[:MAX_SHOWN])


def test_ordered_accessors_are_used() -> None:
    text = "\n".join(path.read_text(errors="replace") for path in _sources())
    assert "GLD32(" in text
    assert "GST32(" in text
