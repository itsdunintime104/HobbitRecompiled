"""Compare analysis.json against Apple's otool and nm, run at test time."""

from __future__ import annotations

import bisect
import re
import subprocess
from pathlib import Path
from typing import Any

FUNCTION_COUNT = 61627
FIRST_FUNCTION = 0x2720
LAST_FUNCTION = 0x11480D0
TEXT_SIZE = 18111635
MOD_INIT_COUNT = 3021
IMPORT_COUNT = 378


def run_tool(*args: str) -> str:
    return subprocess.check_output(args, text=True)


def _otool_indirect(binary: Path) -> dict[int, str | None]:
    result: dict[int, str | None] = {}
    for line in run_tool("otool", "-Iv", str(binary)).splitlines():
        parts = line.split()
        if not parts or not parts[0].startswith("0x"):
            continue
        if parts[1] in ("LOCAL", "ABSOLUTE"):
            result[int(parts[0], 16)] = None
        else:
            assert parts[1].isdigit(), line
            result[int(parts[0], 16)] = parts[2]
    return result


def _otool_sections(binary: Path) -> dict[str, dict[str, int]]:
    sections: dict[str, dict[str, int]] = {}
    current: dict[str, int] | None = None
    for line in run_tool("otool", "-l", str(binary)).splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "sectname":
            current = sections.setdefault(parts[1], {})
        elif (
            current is not None
            and len(parts) >= 2
            and parts[0] in ("addr", "size", "offset", "reserved1", "reserved2")
        ):
            current[parts[0]] = int(parts[1], 0)
    return sections


def test_functions(analysis: dict[str, Any]) -> None:
    funcs = analysis["functions"]
    assert len(funcs) == FUNCTION_COUNT
    assert funcs[0]["addr"] == FIRST_FUNCTION
    assert funcs[-1]["addr"] == LAST_FUNCTION
    assert all(f["size"] > 0 for f in funcs)
    addrs = [f["addr"] for f in funcs]
    assert all(a < b for a, b in zip(addrs, addrs[1:]))
    assert sum(f["size"] for f in funcs) == TEXT_SIZE


def test_imports_match_nm(analysis: dict[str, Any], binary_path: Path) -> None:
    nm_names = set(run_tool("nm", "-u", str(binary_path)).split())
    names = [i["name"] for i in analysis["imports"]]
    assert set(names) == nm_names
    assert len(names) == len(nm_names) == IMPORT_COUNT
    dylibs = set(analysis["dylibs"])
    assert all(i["dylib"] is None or i["dylib"] in dylibs for i in analysis["imports"])


def test_indirect_slots_match_otool(analysis: dict[str, Any], binary_path: Path) -> None:
    expected = _otool_indirect(binary_path)
    actual = {s["addr"]: s["symbol"] for s in analysis["indirect_slots"]}
    assert len(actual) == len(analysis["indirect_slots"])
    assert actual == expected
    kinds = {s["section"]: s["kind"] for s in analysis["indirect_slots"]}
    assert kinds == {
        "__symbol_stub": "stub",
        "__nl_symbol_ptr": "nonlazy_ptr",
        "__got": "got",
        "__la_symbol_ptr": "lazy_ptr",
    }


def test_dylibs_match_otool(analysis: dict[str, Any], binary_path: Path) -> None:
    lines = run_tool("otool", "-L", str(binary_path)).splitlines()[1:]
    expected = [line.strip().split(" (compatibility version")[0] for line in lines]
    assert analysis["dylibs"] == expected


def test_entry_matches_otool(analysis: dict[str, Any], binary_path: Path) -> None:
    out = run_tool("otool", "-l", str(binary_path))
    match = re.search(r"\beip 0x([0-9a-f]+)", out)
    assert match is not None
    assert analysis["binary"]["entry"] == int(match.group(1), 16)


def test_data_in_code_matches_otool(analysis: dict[str, Any], binary_path: Path) -> None:
    out = run_tool("otool", "-G", str(binary_path))
    match = re.search(r"Data in code table \((\d+) entries\)", out)
    assert match is not None
    assert len(analysis["data_in_code"]) == int(match.group(1))


def test_sections_match_otool(analysis: dict[str, Any], binary_path: Path) -> None:
    expected = _otool_sections(binary_path)
    actual = {sec["name"]: sec for seg in analysis["segments"] for sec in seg["sections"]}
    assert set(actual) == set(expected)
    for name, fields in expected.items():
        for key, value in fields.items():
            assert actual[name][key] == value, (name, key)


def test_mod_init_funcs(analysis: dict[str, Any], binary_path: Path) -> None:
    size = _otool_sections(binary_path)["__mod_init_func"]["size"]
    inits = analysis["mod_init_funcs"]
    assert len(inits) == size // 4 == MOD_INIT_COUNT
    funcs = analysis["functions"]
    addrs = [f["addr"] for f in funcs]
    ends = {f["addr"]: f["addr"] + f["size"] for f in funcs}
    for target in inits:
        i = bisect.bisect_right(addrs, target) - 1
        assert i >= 0 and target < ends[addrs[i]], hex(target)
