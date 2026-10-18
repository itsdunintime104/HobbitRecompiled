"""Capstone linear sweep over the functions listed in analysis.json.

Usage: python -m tools.hobbit.disasm <analysis.json> <binary> -o <dir>
Writes <dir>/disasm_stats.json and <dir>/switch_tables.json.

Every indirect `jmp reg` is classified as `switch` (jump table, resolved: kind
`pic` for base-relative entries, `abs` for absolute entries), `tailcall`,
`explained` (a jump through a code-pointer table that is not a switch, with the
reason) or `other` (unclassified). Resolved table byte ranges are treated as
data and are skipped when the function is swept again.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import os
from collections import Counter
from pathlib import Path
from typing import Any, NamedTuple

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

MASK32 = 0xFFFFFFFF
LOOKBACK = 12  # instructions before `jmp reg` that the patterns may span
CMP_LOOKBACK = 48  # instructions before the table load that may hold `cmp`
FLAG_GAP = 24  # flag-neutral instructions allowed between `cmp` and `ja`
CONTEXT = 8  # instructions saved for an `other` jump

DIRECT_TARGET_RE = re.compile(r"^0x[0-9a-f]+$")
REGS32 = frozenset({"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"})
# mov rT, dword ptr [rP + rI*4 (+|-) disp]
TABLE_LOAD_RE = re.compile(
    r"^(\w+), dword ptr \[(\w+) \+ (\w+)\*4(?: ([+-]) (0x[0-9a-f]+))?\]$"
)
# 32-bit register that each sub-register belongs to
SUBREG_FAMILY = {
    sub: full
    for full, subs in {
        "eax": ("ax", "al", "ah"),
        "ebx": ("bx", "bl", "bh"),
        "ecx": ("cx", "cl", "ch"),
        "edx": ("dx", "dl", "dh"),
        "esi": ("si",),
        "edi": ("di",),
        "ebp": ("bp",),
    }.items()
    for sub in subs
}
FLAG_SETTING_SSE = frozenset({"comiss", "ucomiss", "comisd", "ucomisd", "ptest"})
COMPARE_MNEMONICS = ("cmp", "sub")  # `sub rX, N` sets the flags that `cmp rX, N` would
FLAG_NEUTRAL_MNEMONICS = frozenset({"lea", "xorps", "pxor", "push", "pop", "nop"})
COPY_LOOKBACK = 8  # instructions before a `cmp` that may copy the compared register
# mov rT, dword ptr [(rB +) rI*scale (+|-) disp]: an indexed load with no base add
INDEXED_LOAD_RE = re.compile(
    r"^(\w+), dword ptr \[(?:(\w+) \+ )?(\w+)\*(\d)(?: ([+-]) (0x[0-9a-f]+))?\]$"
)
# sections that may hold a table of absolute code addresses
ABS_TABLE_SECTIONS = frozenset({"__DATA,__const", "__DATA,__data", "__TEXT,__const"})
# lea rX, [rP + rQ + disp]
LEA_BASE_RE = re.compile(r"^(\w+), \[(\w+) \+ (\w+)(?: ([+-]) (0x[0-9a-f]+))?\]$")
EXPLAINED_ENTRIES = 8  # table entries read to back an `explained` reason
NO_WRITE_MNEMONICS = frozenset({"cmp", "test", "push", "jmp", "call", "nop"})


class Insn(NamedTuple):
    addr: int
    size: int
    mnem: str
    ops: str


class AddressMap:
    """Virtual address -> file offset, from the segments in analysis.json."""

    def __init__(self, analysis: dict[str, Any]) -> None:
        self._ranges: list[tuple[int, int, int]] = [
            (seg["vmaddr"], seg["fileoff"], seg["filesize"])
            for seg in analysis["segments"]
            if seg["filesize"] > 0
        ]
        self._sections: list[tuple[int, int, str]] = [
            (sec["addr"], sec["size"], f'{seg["name"]},{sec["name"]}')
            for seg in analysis["segments"]
            for sec in seg["sections"]
        ]

    def file_offset(self, addr: int) -> int | None:
        for vmaddr, fileoff, filesize in self._ranges:
            if vmaddr <= addr < vmaddr + filesize:
                return fileoff + (addr - vmaddr)
        return None

    def section_of(self, addr: int) -> str | None:
        for start, size, name in self._sections:
            if start <= addr < start + size:
                return name
        return None

    def read_i32(self, data: bytes, addr: int) -> int | None:
        off = self.file_offset(addr)
        if off is None or off + 4 > len(data):
            return None
        (value,) = struct.unpack_from("<i", data, off)
        return value


def _text_section(analysis: dict[str, Any]) -> dict[str, Any]:
    for seg in analysis["segments"]:
        for sec in seg["sections"]:
            if sec["name"] == "__text":
                return sec
    raise KeyError("__text")


def _sweep_function(
    md: Cs,
    code: bytes,
    func_addr: int,
    skip: list[tuple[int, int]],
) -> tuple[list[Insn], list[int]]:
    """Linear sweep of one function. `skip` holds [start, end) data ranges."""
    func_end = func_addr + len(code)
    bounds: list[tuple[int, int]] = []
    cursor = func_addr
    for start, end in sorted(skip):
        start, end = max(start, func_addr), min(end, func_end)
        if start >= end or start < cursor:
            continue
        bounds.append((cursor, start))
        cursor = end
    bounds.append((cursor, func_end))

    insns: list[Insn] = []
    failures: list[int] = []
    for seg_start, seg_end in bounds:
        pos = seg_start
        while pos < seg_end:
            chunk = code[pos - func_addr : seg_end - func_addr]
            consumed = pos
            for addr, size, mnem, ops in md.disasm_lite(chunk, pos):
                insns.append(Insn(addr, size, mnem, ops))
                consumed = addr + size
            if consumed >= seg_end:
                break
            failures.append(consumed)  # decode failure: record, skip 1 byte
            pos = consumed + 1
    return insns, failures


def _is_indirect(insn: Insn) -> bool:
    return insn.mnem in ("call", "jmp") and not DIRECT_TARGET_RE.match(insn.ops)


def _pic_bases(insns: list[Insn]) -> list[dict[str, int]]:
    """For each instruction index, the PIC base values known before it.

    Keys are register names, or `[mem]` operand text for a stack slot that
    holds a spilled copy of the base. Other writes do not clear a value: code
    after `ret` or `jmp` is reached by branches, so linear order says nothing
    about which writes happened. A wrong value is caught because every table
    target must land inside the function.
    """
    states: list[dict[str, int]] = []
    current: dict[str, int] = {}
    for i, insn in enumerate(insns):
        states.append(dict(current))
        prev = insns[i - 1] if i else None
        if (
            insn.mnem == "pop"
            and insn.ops in REGS32
            and prev is not None
            and prev.mnem == "call"
            and DIRECT_TARGET_RE.match(prev.ops)
            and int(prev.ops, 16) == insn.addr
        ):
            current[insn.ops] = insn.addr
        elif insn.mnem == "mov":
            dst, _, src = insn.ops.partition(", ")
            src = src.removeprefix("dword ptr ")
            if src in current:
                current[dst.removeprefix("dword ptr ")] = current[src]  # copy, spill, reload
    return states


def _find_table_load(
    insns: list[Insn], j: int, states: list[dict[str, int]]
) -> tuple[int, re.Match[str], str] | None:
    """Locate the table load before `jmp rT`: (index, match, pic register).

    Two shapes: `mov rT, [rP + rI*4 + d]` then `add rT, rP`, or the folded
    `add rT, [rT + rI*4 + d]` where rT itself holds the PIC base.
    """
    r_t = insns[j].ops
    lo = max(0, j - LOOKBACK)
    for a in range(j - 1, lo - 1, -1):
        insn = insns[a]
        if insn.mnem != "add" or not insn.ops.startswith(f"{r_t}, "):
            continue
        folded = TABLE_LOAD_RE.match(insn.ops)
        if folded is not None and folded.group(2) == r_t:
            return a, folded, r_t
        r_p = insn.ops.split(", ")[1]
        for m in range(a - 1, lo - 1, -1):
            load = TABLE_LOAD_RE.match(insns[m].ops) if insns[m].mnem == "mov" else None
            if load is None or load.group(1) != r_t:
                continue
            # the add operand may be a copy of the load's base register
            base = states[m].get(load.group(2))
            if base is not None and states[a].get(r_p) == base:
                return m, load, load.group(2)
        return None
    return None


def _match_switch(
    insns: list[Insn],
    j: int,
    states: list[dict[str, int]],
    jumps_to: dict[int, list[int]],
    func_range: tuple[int, int],
    amap: AddressMap,
    data: bytes,
) -> dict[str, Any] | None:
    func_addr, func_end = func_range
    found = _find_table_load(insns, j, states)
    if found is None:
        return None
    m, load, r_p = found
    base = states[m].get(r_p)
    if base is None:
        return None
    count = _entry_count(insns, m, load.group(3), jumps_to)
    if count is None:
        return None
    disp = int(load.group(5) or "0", 16)
    if load.group(4) == "-":
        disp = -disp
    table_addr = (base + disp) & MASK32
    targets: list[int] = []
    for k in range(count):
        entry = amap.read_i32(data, table_addr + 4 * k)
        if entry is None:
            return None
        target = (base + entry) & MASK32
        if not func_addr <= target < func_end:
            return None
        targets.append(target)
    return {
        "jmp_addr": insns[j].addr,
        "pic_base": base,
        "table_addr": table_addr,
        "count": count,
        "targets": targets,
        "kind": "pic",
    }


def _family(operand: str) -> str:
    """32-bit register that holds operand (`al` -> `eax`); other operands are unchanged."""
    return SUBREG_FAMILY.get(operand, operand)


def _flag_neutral(insn: Insn) -> bool:
    """True if insn leaves EFLAGS alone. SSE arithmetic does, except the compares."""
    if insn.mnem.startswith("mov") or insn.mnem in FLAG_NEUTRAL_MNEMONICS:
        return True
    return "xmm" in insn.ops and insn.mnem not in FLAG_SETTING_SSE


def _cmp_bound(insns: list[Insn], q: int, key: str) -> int | None:
    """Value N of the closest `cmp key, N` before index q, skipping flag-neutral moves."""
    for c in range(q - 1, max(0, q - 1 - FLAG_GAP) - 1, -1):
        mnem = insns[c].mnem
        if mnem in COMPARE_MNEMONICS:
            left, _, right = insns[c].ops.partition(", ")
            if not _same_value(insns, c, left, key):
                return None
            try:
                return int(right, 0)
            except ValueError:
                return None
        if not _flag_neutral(insns[c]):
            return None
    return None


def _same_value(insns: list[Insn], c: int, left: str, key: str) -> bool:
    """True if operand `left` of the `cmp` at c holds the value tracked as `key`.

    Same register family (`dx` for `edx`), or a register copied from it a few
    instructions earlier (`mov cl, al` before `cmp cl, 5`), or the register
    stored into the tracked stack slot (`mov [ebp - 8], esi` before `cmp esi, 5`).
    """
    if _family(left) == _family(key):
        return True
    for e in range(c - 1, max(0, c - COPY_LOOKBACK) - 1, -1):
        dst, _, src = insns[e].ops.partition(", ")
        if insns[e].mnem not in ("mov", "movzx"):
            continue
        if dst == left and _family(src) == _family(key):
            return True
        if dst == key and _family(src) == _family(left):  # key was stored from left
            return True
    return False


def _entry_count(
    insns: list[Insn], m: int, r_i: str, jumps_to: dict[int, list[int]]
) -> int | None:
    """Table size from the range check on rI.

    Inline form: the closest earlier `cmp rI, N` is followed by ja (N+1 entries)
    or jae (N entries). Out-of-line form: the table load starts a block that a
    `jbe` (N+1) or `jb` (N) reaches from elsewhere.
    """
    for c in range(m - 1, max(0, m - CMP_LOOKBACK) - 1, -1):
        if insns[c].mnem in ("jmp", "ret"):
            break  # copies and range checks before a block boundary do not reach the load
        if insns[c].mnem in ("mov", "movzx", "movsx"):
            dst, _, src = insns[c].ops.partition(", ")
            if _family(dst) == _family(r_i):
                r_i = src  # index was copied, widened or reloaded from this operand
                continue
        if insns[c].mnem not in COMPARE_MNEMONICS or not _same_value(
            insns, c, insns[c].ops.partition(", ")[0], r_i
        ):
            continue
        for f in range(c + 1, min(c + 1 + FLAG_GAP, len(insns))):
            follow = insns[f].mnem
            if _flag_neutral(insns[f]):
                continue
            n = _cmp_bound(insns, f, r_i)
            if n is not None and follow == "ja":
                return n + 1
            if n is not None and follow == "jae":
                return n
            break
        break
    for k in range(m, max(0, m - CMP_LOOKBACK) - 1, -1):
        for q in jumps_to.get(insns[k].addr, []):
            n = _cmp_bound(insns, q, r_i)
            if n is not None and insns[q].mnem == "jbe":
                return n + 1
            if n is not None and insns[q].mnem == "jb":
                return n
        if k > 0 and insns[k - 1].mnem in ("jmp", "ret"):
            break
    return None


def _nearest_writer(insns: list[Insn], j: int) -> int | None:
    """Index of the closest instruction before j that writes the register `jmp` uses."""
    r_t = insns[j].ops
    for k in range(j - 1, max(0, j - LOOKBACK) - 1, -1):
        insn = insns[k]
        if insn.mnem in NO_WRITE_MNEMONICS or insn.mnem.startswith("j"):
            continue
        if insn.ops.split(",")[0] == r_t:
            return k
    return None


def _is_tailcall(insns: list[Insn], j: int) -> bool:
    k = _nearest_writer(insns, j)
    if k is None:
        return False
    insn = insns[k]
    return insn.mnem == "mov" and "dword ptr [" in insn.ops and "*" not in insn.ops


def _indexed_load(insns: list[Insn], j: int) -> tuple[int, re.Match[str]] | None:
    """The `mov rT, [(rB +) rI*s (+ disp)]` that feeds `jmp rT` with no base add."""
    k = _nearest_writer(insns, j)
    if k is None or insns[k].mnem != "mov":
        return None
    load = INDEXED_LOAD_RE.match(insns[k].ops)
    return None if load is None else (k, load)


def _table_start(
    insns: list[Insn], k: int, load: re.Match[str], states: list[dict[str, int]]
) -> int | None:
    """Address of the table that the indexed load at k reads, if it is a constant.

    Either `[rI*4 + disp]` / `[rB + rI*4 + disp]` with rB a PIC base, or
    `[rX + rI*4]` where rX was built by `lea rX, [rP + rQ + disp]` with rP a PIC base.
    """
    disp = int(load.group(6) or "0", 16)
    if load.group(5) == "-":
        disp = -disp
    r_b = load.group(2)
    if r_b is None:
        return disp & MASK32
    if r_b in states[k]:
        return (states[k][r_b] + disp) & MASK32
    for a in range(k - 1, max(0, k - LOOKBACK) - 1, -1):
        lea = LEA_BASE_RE.match(insns[a].ops) if insns[a].mnem == "lea" else None
        if lea is not None and lea.group(1) == r_b:
            base = states[a].get(lea.group(2))
            if base is None:
                return None
            lea_disp = int(lea.group(5) or "0", 16)
            if lea.group(4) == "-":
                lea_disp = -lea_disp
            return (base + lea_disp + disp) & MASK32
    return None


def _match_abs_switch(
    insns: list[Insn],
    j: int,
    states: list[dict[str, int]],
    jumps_to: dict[int, list[int]],
    func_range: tuple[int, int],
    amap: AddressMap,
    data: bytes,
) -> dict[str, Any] | None:
    """`mov rT, [rI*4 + disp]` then `jmp rT`: a table of absolute in-function targets."""
    found = _indexed_load(insns, j)
    if found is None:
        return None
    k, load = found
    if load.group(4) != "4":
        return None
    table_addr = _table_start(insns, k, load, states)
    if table_addr is None or amap.section_of(table_addr) not in ABS_TABLE_SECTIONS:
        return None
    count = _entry_count(insns, k, load.group(3), jumps_to)
    if count is None:
        return None
    func_addr, func_end = func_range
    targets: list[int] = []
    for n in range(count):
        target = amap.read_i32(data, table_addr + 4 * n)
        if target is None or not func_addr <= target & MASK32 < func_end:
            return None
        targets.append(target & MASK32)
    return {
        "jmp_addr": insns[j].addr,
        "pic_base": 0,
        "table_addr": table_addr,
        "count": count,
        "targets": targets,
        "kind": "abs",
    }


def _explain_table_jump(
    insns: list[Insn],
    j: int,
    states: list[dict[str, int]],
    func_range: tuple[int, int],
    amap: AddressMap,
    data: bytes,
) -> str | None:
    """Reason for a `jmp rT` that follows an indexed load but is not a switch, or None."""
    found = _indexed_load(insns, j)
    if found is None:
        return None
    k, load = found
    where = f"index {load.group(3)}*{load.group(4)}"
    table_addr = _table_start(insns, k, load, states)
    if table_addr is None:
        return f"jump through a code-pointer table ({where}); table address is not constant"
    section = amap.section_of(table_addr)
    entries = [amap.read_i32(data, table_addr + 4 * n) for n in range(EXPLAINED_ENTRIES)]
    func_addr, func_end = func_range
    text = [
        e
        for e in entries
        if e is not None
        and amap.section_of(e & MASK32) == "__TEXT,__text"
        and not func_addr <= e & MASK32 < func_end
    ]
    if entries[0] is None:
        evidence = "the table is in zero-filled memory, so the pointers are stored at run time"
    else:
        evidence = (
            f"{len(text)} of the first {EXPLAINED_ENTRIES} entries are __text addresses "
            f"outside the function"
        )
    return (
        f"jump through a function-pointer table at {table_addr:#x} ({section}), {where}: "
        f"no base add and no in-function targets, so it is a computed tail call, not a switch; "
        f"{evidence}"
    )


class Jumps(NamedTuple):
    switches: list[dict[str, Any]]
    tailcalls: list[int]
    explained: list[dict[str, Any]]
    others: list[dict[str, Any]]


def _classify_jumps(
    insns: list[Insn], func_range: tuple[int, int], amap: AddressMap, data: bytes
) -> Jumps:
    switches: list[dict[str, Any]] = []
    tailcalls: list[int] = []
    explained: list[dict[str, Any]] = []
    others: list[dict[str, Any]] = []
    states: list[dict[str, int]] | None = None
    jumps_to: dict[int, list[int]] = {}
    for q, insn in enumerate(insns):
        if insn.mnem.startswith("j") and DIRECT_TARGET_RE.match(insn.ops):
            jumps_to.setdefault(int(insn.ops, 16), []).append(q)
    for j, insn in enumerate(insns):
        if insn.mnem != "jmp" or insn.ops not in REGS32:
            continue
        if states is None:
            states = _pic_bases(insns)
        switch = _match_switch(insns, j, states, jumps_to, func_range, amap, data)
        if switch is None:
            switch = _match_abs_switch(insns, j, states, jumps_to, func_range, amap, data)
        if switch is not None:
            switches.append(switch)
        elif _is_tailcall(insns, j):
            tailcalls.append(insn.addr)
        else:
            context = [
                f"{i.addr:#x} {i.mnem} {i.ops}" for i in insns[max(0, j - CONTEXT) : j + 1]
            ]
            reason = _explain_table_jump(insns, j, states, func_range, amap, data)
            if reason is not None:
                explained.append({"addr": insn.addr, "reason": reason, "context": context})
            else:
                others.append({"addr": insn.addr, "context": context})
    return Jumps(switches, tailcalls, explained, others)


def _table_ranges(switches: list[dict[str, Any]]) -> list[tuple[int, int]]:
    return [(s["table_addr"], s["table_addr"] + 4 * s["count"]) for s in switches]


def _function_code(analysis: dict[str, Any], data: bytes, func: dict[str, Any]) -> bytes:
    text = _text_section(analysis)
    base = text["offset"] + (func["addr"] - text["addr"])
    return data[base : base + func["size"]]


def _resolve_function(
    md: Cs, amap: AddressMap, code: bytes, func_addr: int, data: bytes
) -> tuple[list[Insn], list[int], list[int], Jumps]:
    """Sweep, find tables, and sweep again with table bytes skipped, until stable.

    Returns the final instructions, the first (plain) sweep's decode failures,
    the final sweep's decode failures, and the classified indirect jumps.
    """
    skip: list[tuple[int, int]] = []
    first_failures: list[int] | None = None
    while True:
        insns, failures = _sweep_function(md, code, func_addr, skip)
        if first_failures is None:
            first_failures = failures
        jumps = _classify_jumps(insns, (func_addr, func_addr + len(code)), amap, data)
        ranges = _table_ranges(jumps.switches)
        if all(r in skip for r in ranges):
            return insns, first_failures, failures, jumps
        skip = sorted(set(skip) | set(ranges))


def sweep(analysis: dict[str, Any], data: bytes) -> tuple[dict[str, Any], dict[str, Any]]:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    amap = AddressMap(analysis)

    total = 0
    raw_failures: list[int] = []
    failures_outside: list[int] = []
    histogram: Counter[str] = Counter()
    x87 = sse = indirect_calls = indirect_jmps = seg_prefixed = 0
    switch_out: list[dict[str, Any]] = []
    tailcall_out: list[int] = []
    explained_out: list[dict[str, Any]] = []
    other_out: list[dict[str, Any]] = []

    for func in analysis["functions"]:
        func_addr: int = func["addr"]
        code = _function_code(analysis, data, func)
        insns, first, final, jumps = _resolve_function(
            md, amap, code, func_addr, data
        )
        raw_failures.extend(first)
        failures_outside.extend(final)
        for insn in insns:
            total += 1
            histogram[insn.mnem] += 1
            if insn.mnem[0] == "f":
                x87 += 1
            if "xmm" in insn.ops:
                sse += 1
            if "gs:" in insn.ops or "fs:" in insn.ops:
                seg_prefixed += 1
            if insn.mnem == "call" and _is_indirect(insn):
                indirect_calls += 1
            elif insn.mnem == "jmp" and _is_indirect(insn):
                indirect_jmps += 1
        switch_out.extend({**s, "func_addr": func_addr} for s in jumps.switches)
        tailcall_out.extend(jumps.tailcalls)
        explained_out.extend(jumps.explained)
        other_out.extend(jumps.others)

    stats = {
        "total_instructions": total,
        "decode_failure_count": len(raw_failures),
        "decode_failures": raw_failures,
        "decode_failures_outside_tables": len(failures_outside),
        "decode_failures_outside_tables_addrs": failures_outside,
        "x87": x87,
        "sse": sse,
        "indirect_call": indirect_calls,
        "indirect_jmp": indirect_jmps,
        "segment_prefixed": seg_prefixed,
        "mnemonics": dict(histogram.most_common()),
    }
    switch = {
        "switch": switch_out,
        "tailcall": tailcall_out,
        "other": other_out,
        "explained": explained_out,
    }
    return stats, switch


def instruction_starts(
    analysis: dict[str, Any], data: bytes, func_addr: int, tables: list[dict[str, Any]]
) -> set[int]:
    """Instruction start addresses of one function, with the given tables as data."""
    func = next(f for f in analysis["functions"] if f["addr"] == func_addr)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    insns, _ = _sweep_function(
        md, _function_code(analysis, data, func), func_addr, _table_ranges(tables)
    )
    return {i.addr for i in insns}


def _write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    """Readers (the xrecomp driver) never see a half-written file."""
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(payload, indent=1) + "\n")
    os.replace(tmp, path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("analysis", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("-o", "--output-dir", type=Path, required=True)
    args = parser.parse_args()
    analysis = json.loads(args.analysis.read_text())
    stats, switch = sweep(analysis, args.binary.read_bytes())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    _write_json_atomic(args.output_dir / "disasm_stats.json", stats)
    _write_json_atomic(args.output_dir / "switch_tables.json", switch)


if __name__ == "__main__":
    main()
