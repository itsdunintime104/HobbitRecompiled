"""String-level checks that the SSE2 integer and overflow-flag lifts emit code.

For each mnemonic group, one small function from the real image that uses the
mnemonic is lifted, and the line carrying the instruction's tag must be a
statement, not a bare comment or an unimplemented marker. Semantics are
checked elsewhere (tests/conformance).
"""
from __future__ import annotations

import json
from pathlib import Path

import pytest
from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.xrecomp import run
from tools.xrecomp.translator import BatchTranslator

ROOT = Path(__file__).resolve().parent.parent
ANALYSIS = ROOT / "recon" / "analysis.json"
SWITCH = ROOT / "recon" / "switch_tables.json"
TEXT_VA_TO_FILE = 4096  # __TEXT vmaddr; fileoff is 0

GROUPS: dict[str, tuple[str, ...]] = {
    "move": ("movdqa", "movdqu"),
    "shuffle": ("pshufd",),
    "word_lane": ("pinsrw", "pextrw"),
    "add_sub": ("paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq"),
    "unpack": ("punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
               "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq"),
    "multiply": ("pmuludq",),
    "compare": ("pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd"),
    "shift": ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"),
    "byte_shift": ("pslldq", "psrldq"),
    "logic": ("pand", "pandn", "por", "pxor"),
    "pack": ("packuswb", "packsswb", "packssdw"),
    "convert": ("cvtdq2ps", "cvtps2dq", "cvttps2dq"),
    "movq": ("movq",),
    "overflow": ("seto", "setno", "cmovo", "cmovno"),
}
ALL = {m for ms in GROUPS.values() for m in ms}


def _pick_functions(binary: bytes) -> dict[str, int]:
    """The smallest function in the image that uses each mnemonic."""
    analysis = json.loads(ANALYSIS.read_text())
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    best: dict[str, tuple[int, int]] = {}
    for func in analysis["functions"]:
        start = func["addr"] - TEXT_VA_TO_FILE
        for insn in md.disasm(binary[start:start + func["size"]], func["addr"]):
            if insn.mnemonic in ALL and func["size"] < best.get(
                    insn.mnemonic, (1 << 30, 0))[0]:
                best[insn.mnemonic] = (func["size"], func["addr"])
    return {m: addr for m, (_, addr) in best.items()}


@pytest.fixture(scope="module")
def lifted(binary_path: Path, tmp_path_factory: pytest.TempPathFactory) -> dict[str, str]:
    if not binary_path.exists():
        pytest.skip(f"game binary not present: {binary_path}")
    picks = _pick_functions(binary_path.read_bytes())
    out = tmp_path_factory.mktemp("lift_sse2")
    bt: BatchTranslator = run.build(ANALYSIS, SWITCH, binary_path, out,
                                    only=set(picks.values()))
    return {m: bt.translate_single(addr) or "" for m, addr in picks.items()}


def _statement_lines(code: str, mnemonic: str) -> list[str]:
    # movdqa/movdqu lift as the same 16-byte move the movaps lift emits.
    tag = "movaps" if mnemonic in ("movdqa", "movdqu") else mnemonic
    return [ln.strip() for ln in code.splitlines() if f"/* {tag} */" in ln]


@pytest.mark.parametrize("group", sorted(GROUPS))
def test_group_lifts_to_statements(lifted: dict[str, str], group: str) -> None:
    for mnemonic in GROUPS[group]:
        code = lifted.get(mnemonic)
        if code is None:
            continue  # the image never uses this mnemonic
        lines = _statement_lines(code, mnemonic)
        assert lines, f"{mnemonic}: no tagged line in the lift"
        assert all(not ln.startswith("/*") and "RECOMP_UNIMPL" not in ln
                   for ln in lines), f"{mnemonic}: {lines}"
