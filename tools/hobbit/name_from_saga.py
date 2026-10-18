"""Label stripped functions with their engine source file, and a name where one is shown.

Usage: python -m tools.hobbit.name_from_saga <analysis.json> <binary> <switch_tables.json>
           <saga_root> -o <names_saga.json>

The binary holds `__FILE__:__LINE__` strings (`/.../nu2api/nucore/NuMemory.cpp:471`).
Every instruction that refers to one of them (`push imm32`, `mov reg, imm32`, or
`lea reg, [pic_base + disp]` after `call L; pop base`) puts that file and line on the
function that holds the instruction.

A name comes from the saga decompilation (matching.json lists its functions per source
file, with demangled names). The function is named only if exactly one saga function in
the same source file has a string literal that our function also refers to.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any, NamedTuple

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.hobbit import disasm

FILE_STRING_RE = re.compile(
    r"^/.*?/AccuRev/[^/]+/(?P<path>.+\.(?:cpp|cc|c|mm|m|hpp|h)):(?P<line>\d+)$"
)
MEM_OPERAND_RE = re.compile(r"\[(\w+) ([+-]) (0x[0-9a-f]+)\]")
IMMEDIATE_RE = re.compile(r"(?:^|, )(0x[0-9a-f]+)$")
HEADER_SUFFIXES = (".h", ".hpp")
MIN_LITERAL = 6  # shorter string literals are too common to identify a function
NOT_A_DEFINITION = frozenset({"return", "else", "case", "new", "delete", "sizeof", "throw"})
NOT_AFTER = frozenset(".(,=!|+-/?:<")
CONFIDENCE_FILE = "file"
CONFIDENCE_NAME = "name"


class FileString(NamedTuple):
    addr: int
    path: str
    line: int


class Site(NamedTuple):
    insn_addr: int
    string_addr: int


def read_cstrings(analysis: dict[str, Any], data: bytes) -> dict[int, str]:
    """Address -> text for every string in __cstring."""
    amap = disasm.AddressMap(analysis)
    section = next(
        sec
        for seg in analysis["segments"]
        for sec in seg["sections"]
        if sec["name"] == "__cstring"
    )
    start = amap.file_offset(section["addr"])
    assert start is not None
    blob = data[start : start + section["size"]]
    strings: dict[int, str] = {}
    pos = 0
    for raw in blob.split(b"\0"):
        strings[section["addr"] + pos] = raw.decode("latin-1")
        pos += len(raw) + 1
    return strings


def find_file_strings(cstrings: dict[int, str]) -> dict[int, FileString]:
    """Address -> (path, line) for every `__FILE__:line` string."""
    found: dict[int, FileString] = {}
    for addr, text in cstrings.items():
        match = FILE_STRING_RE.match(text)
        if match:
            found[addr] = FileString(addr, match["path"], int(match["line"]))
    return found


def find_references(insns: list[disasm.Insn], targets: set[int]) -> list[Site]:
    """Instructions that put the address of one of `targets` into a register or onto the stack."""
    states = disasm._pic_bases(insns)
    sites: list[Site] = []
    for insn, known in zip(insns, states):
        if insn.mnem in ("call", "jmp") or insn.mnem.startswith("j"):
            continue
        hit = IMMEDIATE_RE.search(insn.ops)
        if hit and int(hit[1], 16) in targets:
            sites.append(Site(insn.addr, int(hit[1], 16)))
        for reg, sign, disp in MEM_OPERAND_RE.findall(insn.ops):
            if reg not in known:
                continue
            addr = (known[reg] + (int(disp, 16) if sign == "+" else -int(disp, 16))) & disasm.MASK32
            if addr in targets:
                sites.append(Site(insn.addr, addr))
    return sites


def map_to_functions(
    analysis: dict[str, Any], data: bytes, switches: dict[str, Any], targets: set[int]
) -> dict[int, list[Site]]:
    """Function start -> sites in that function that refer to a target address."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    skip: dict[int, list[tuple[int, int]]] = {}
    for table in switches["switch"]:
        skip.setdefault(table["func_addr"], []).extend(disasm._table_ranges([table]))
    result: dict[int, list[Site]] = {}
    for func in analysis["functions"]:
        code = disasm._function_code(analysis, data, func)
        insns, _ = disasm._sweep_function(md, code, func["addr"], skip.get(func["addr"], []))
        sites = find_references(insns, targets)
        if sites:
            result[func["addr"]] = sites
    return result


def pick_site(sites: list[Site], strings: dict[int, FileString]) -> FileString:
    """The source file for a function: a .cpp over a header, then the most sites, then the first."""
    counts: dict[str, int] = {}
    for site in sites:
        path = strings[site.string_addr].path
        counts[path] = counts.get(path, 0) + 1

    def rank(site: Site) -> tuple[bool, int]:
        path = strings[site.string_addr].path
        return (path.endswith(HEADER_SUFFIXES), -counts[path])

    return strings[min(sites, key=lambda s: (rank(s), s.insn_addr)).string_addr]


def referenced_literals(sites: list[Site], cstrings: dict[int, str]) -> set[str]:
    """Text of the strings (other than file strings) that a function refers to."""
    out: set[str] = set()
    for site in sites:
        text = cstrings[site.string_addr]
        if len(text) >= MIN_LITERAL and not FILE_STRING_RE.match(text):
            out.add(text)
    return out


# ---- saga source parsing -------------------------------------------------------------------


def unescape_c(text: str) -> str:
    """Decode C escape sequences in a string literal body."""
    return text.encode("latin-1", "backslashreplace").decode("unicode_escape")


def source_literals(body: str) -> set[str]:
    """String literals in C++ code. Adjacent literals join; comments and chars are skipped."""
    literals: set[str] = set()
    current: list[str] | None = None
    i, n = 0, len(body)
    while i < n:
        ch = body[i]
        if body.startswith("//", i):
            i = body.find("\n", i)
            i = n if i < 0 else i
        elif body.startswith("/*", i):
            i = body.find("*/", i)
            i = n if i < 0 else i + 2
        elif ch == '"':
            j = i + 1
            while j < n and body[j] != '"':
                j += 2 if body[j] == "\\" else 1
            if current is None:
                current = []
            current.append(unescape_c(body[i + 1 : j]))
            i = j + 1
        elif ch == "'":
            j = i + 1
            while j < n and body[j] != "'":
                j += 2 if body[j] == "\\" else 1
            i = j + 1
        else:
            if not ch.isspace() and current is not None:
                literals.add("".join(current))
                current = None
            i += 1
    if current is not None:
        literals.add("".join(current))
    return {s for s in literals if len(s) >= MIN_LITERAL}


def _matching_paren(text: str, open_at: int) -> int:
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _matching_brace(text: str, open_at: int) -> int:
    depth = 0
    i, n = open_at, len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or ch == "'":
            j = i + 1
            while j < n and text[j] != ch:
                j += 2 if text[j] == "\\" else 1
            i = j
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def count_params(params: str) -> int:
    """Number of top-level comma-separated parameters; `()` and `(void)` have none."""
    inner = params.strip()
    if inner in ("", "void"):
        return 0
    depth = 0
    commas = 0
    for ch in inner:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        elif ch == "," and depth == 0:
            commas += 1
    return commas + 1


def split_demangled(demangled: str) -> tuple[str, int] | None:
    """(qualified name, parameter count) of a demangled function, or None if not a plain function."""
    if demangled.startswith(("_GLOBAL__", "non-virtual thunk", "virtual thunk", "guard variable")):
        return None
    head = demangled.split(" [clone", 1)[0]
    depth = 0
    for i in range(len(head) - 1, -1, -1):
        if head[i] == ")":
            depth += 1
        elif head[i] == "(":
            depth -= 1
            if depth == 0:
                name = head[:i].split(" ")[-1] if "operator" not in head[:i] else head[:i]
                params = head[i + 1 : head.rfind(")")]
                if "operator" in name or "<" in name:
                    return None
                return name, count_params(params)
    return None


def function_body(source: str, qualified: str, nparams: int) -> str | None:
    """Body of the one definition of `qualified` with `nparams` parameters, else None."""
    bodies: list[str] = []
    for match in re.finditer(r"(?<![\w:~])" + re.escape(qualified) + r"\s*\(", source):
        before = source[: match.start()].rstrip()
        if before.endswith("->") or (before and before[-1] in NOT_AFTER):
            continue
        word = re.search(r"(\w+)$", before)
        if word and word[1] in NOT_A_DEFINITION:
            continue
        open_at = match.end() - 1
        close = _matching_paren(source, open_at)
        if close < 0 or count_params(source[open_at + 1 : close]) != nparams:
            continue
        depth = 0
        brace = -1
        for i in range(close + 1, len(source)):
            ch = source[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth < 0:
                    break
            elif ch == ";" and depth == 0:
                break
            elif ch == "{" and depth == 0:
                brace = i
                break
        if brace < 0:
            continue
        end = _matching_brace(source, brace)
        if end > 0:
            bodies.append(source[brace : end + 1])
    return bodies[0] if len(bodies) == 1 else None


class SagaFile(NamedTuple):
    source: str
    functions: list[tuple[str, set[str]]]  # (demangled name, literals), in source order


def load_saga_units(saga_root: Path) -> dict[str, list[dict[str, Any]]]:
    """Lower-case source basename -> matching.json units with that basename."""
    matching = json.loads((saga_root / "matching.json").read_text())
    by_base: dict[str, list[dict[str, Any]]] = {}
    for unit in matching["units"]:
        by_base.setdefault(Path(unit["source"]).name.lower(), []).append(unit)
    return by_base


def choose_unit(units: list[dict[str, Any]], path: str) -> dict[str, Any] | None:
    """The unit whose source path shares the longest directory tail with `path`; None if tied."""
    parts = path.lower().split("/")

    def tail(unit: dict[str, Any]) -> int:
        other = unit["source"].lower().split("/")
        n = 0
        while n < min(len(parts), len(other)) and parts[-1 - n] == other[-1 - n]:
            n += 1
        return n

    ranked = sorted(units, key=tail, reverse=True)
    if len(ranked) > 1 and tail(ranked[0]) == tail(ranked[1]):
        return None
    return ranked[0] if ranked else None


def saga_functions(saga_root: Path, unit: dict[str, Any]) -> list[tuple[str, set[str]]]:
    """Functions of one unit in source order, each with the literals of its body."""
    source = (saga_root / unit["source"]).read_text(errors="replace")
    out: list[tuple[str, set[str]]] = []
    for func in sorted(unit["functions"], key=lambda f: f["address"]):
        split = split_demangled(func["demangled_name"])
        body = function_body(source, *split) if split else None
        out.append((func["demangled_name"], source_literals(body) if body else set()))
    return out


def propose_name(
    candidates: list[tuple[str, set[str]]], literals: set[str]
) -> str | None:
    """The one saga function whose literals overlap ours, if exactly one does."""
    hits = [name for name, theirs in candidates if theirs & literals]
    return hits[0] if len(hits) == 1 else None


def build_names(
    analysis: dict[str, Any], data: bytes, switches: dict[str, Any], saga_root: Path
) -> dict[str, dict[str, Any]]:
    cstrings = read_cstrings(analysis, data)
    strings = find_file_strings(cstrings)
    mapped = map_to_functions(analysis, data, switches, set(cstrings))
    units = load_saga_units(saga_root)
    cache: dict[str, list[tuple[str, set[str]]] | None] = {}
    names: dict[str, dict[str, Any]] = {}
    for func_addr, sites in sorted(mapped.items()):
        file_sites = [site for site in sites if site.string_addr in strings]
        if not file_sites:
            continue
        label = pick_site(file_sites, strings)
        entry: dict[str, Any] = {
            "file": label.path,
            "line": label.line,
            "name": None,
            "confidence": CONFIDENCE_FILE,
        }
        if label.path.endswith(HEADER_SUFFIXES):
            entry["inlined_from_header"] = True  # the function itself belongs to some other file
        unit = choose_unit(units.get(Path(label.path).name.lower(), []), label.path)
        if unit is not None:
            if unit["source"] not in cache:
                cache[unit["source"]] = saga_functions(saga_root, unit)
            candidates = cache[unit["source"]] or []
            name = propose_name(candidates, referenced_literals(sites, cstrings))
            if name:
                entry["name"] = name
                entry["confidence"] = CONFIDENCE_NAME
        names[f"0x{func_addr:X}"] = entry
    return names


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("analysis", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("switch_tables", type=Path)
    parser.add_argument("saga_root", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    names = build_names(
        json.loads(args.analysis.read_text()),
        args.binary.read_bytes(),
        json.loads(args.switch_tables.read_text()),
        args.saga_root,
    )
    disasm._write_json_atomic(args.output, names)
    named = sum(1 for e in names.values() if e["name"])
    print(f"{len(names)} functions with a file, {named} with a name")


if __name__ == "__main__":
    main()
