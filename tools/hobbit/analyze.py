"""Build recon/analysis.json from a 32-bit Mach-O binary.

Usage: python -m tools.hobbit.analyze <binary> -o <json>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from enum import StrEnum
from pathlib import Path
from typing import Any

from tools.hobbit import macho

class SlotKind(StrEnum):
    STUB = "stub"
    LAZY_PTR = "lazy_ptr"
    NONLAZY_PTR = "nonlazy_ptr"
    GOT = "got"


PTR_KINDS = {
    macho.S_NON_LAZY_SYMBOL_POINTERS: SlotKind.NONLAZY_PTR,
    macho.S_LAZY_SYMBOL_POINTERS: SlotKind.LAZY_PTR,
}


def _section_kind(sec: macho.Section) -> SlotKind | None:
    sec_type = sec.flags & 0xFF
    if sec_type == macho.S_SYMBOL_STUBS:
        return SlotKind.STUB
    if sec_type == macho.S_NON_LAZY_SYMBOL_POINTERS and sec.name == "__got":
        return SlotKind.GOT
    return PTR_KINDS.get(sec_type)


def _indirect_slots(mo: macho.MachO) -> list[dict[str, Any]]:
    slots: list[dict[str, Any]] = []
    for _, sec in mo.sections():
        kind = _section_kind(sec)
        if kind is None:
            continue
        stride = sec.reserved2 if kind == SlotKind.STUB else 4
        for i in range(sec.size // stride):
            index = mo.indirect_symbols[sec.reserved1 + i]
            symbol: str | None
            if index & (macho.INDIRECT_SYMBOL_LOCAL | macho.INDIRECT_SYMBOL_ABS):
                symbol = None
            else:
                symbol = mo.symbols[index].name
            slots.append(
                {"section": sec.name, "addr": sec.addr + stride * i,
                 "kind": kind.value, "symbol": symbol}
            )
    return slots


def _imports(mo: macho.MachO) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for sym in mo.symbols:
        if not sym.is_undefined_external:
            continue
        ordinal = sym.library_ordinal
        dylib = mo.dylibs[ordinal - 1] if 1 <= ordinal <= len(mo.dylibs) else None
        out.append({"name": sym.name, "dylib": dylib})
    return out


def _functions(mo: macho.MachO) -> list[dict[str, int]]:
    text = mo.find_section("__text")
    text_end = text.addr + text.size
    starts = mo.function_starts
    ends = starts[1:] + [text_end]
    return [{"addr": a, "size": e - a} for a, e in zip(starts, ends)]


def _mod_init_funcs(mo: macho.MachO) -> list[int]:
    for _, sec in mo.sections():
        if sec.flags & 0xFF == macho.S_MOD_INIT_FUNC_POINTERS:
            n = sec.size // 4
            return list(struct.unpack_from(f"<{n}I", mo.data, sec.offset))
    return []


def analyze(data: bytes) -> dict[str, Any]:
    mo = macho.parse(data)
    return {
        "binary": {"sha256": hashlib.sha256(data).hexdigest(), "entry": mo.entry},
        "dylibs": mo.dylibs,
        "segments": [
            {
                "name": seg.name, "vmaddr": seg.vmaddr, "vmsize": seg.vmsize,
                "fileoff": seg.fileoff, "filesize": seg.filesize,
                "initprot": seg.initprot, "maxprot": seg.maxprot,
                "sections": [
                    {"name": s.name, "addr": s.addr, "size": s.size,
                     "offset": s.offset, "flags": s.flags,
                     "reserved1": s.reserved1, "reserved2": s.reserved2}
                    for s in seg.sections
                ],
            }
            for seg in mo.segments
        ],
        "functions": _functions(mo),
        "data_in_code": [
            {"addr": d.addr, "length": d.length, "kind": d.kind}
            for d in mo.data_in_code
        ],
        "imports": _imports(mo),
        "indirect_slots": _indirect_slots(mo),
        "mod_init_funcs": _mod_init_funcs(mo),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.binary.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=1) + "\n")


if __name__ == "__main__":
    main()
