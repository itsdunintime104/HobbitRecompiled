from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from tools.xrecomp import run
from tools.xrecomp.translator import BatchTranslator

ROOT = Path(__file__).resolve().parent.parent
ANALYSIS = ROOT / "recon" / "analysis.json"
SWITCH = ROOT / "recon" / "switch_tables.json"
TAIL_FN, PIC_FN = 0x1C260, 0x21E00
PIC_POP, SWITCH_JMP = 0x21E0B, 0x21E20


@pytest.fixture(scope="module")
def lifter(binary_path: Path, tmp_path_factory: pytest.TempPathFactory) -> BatchTranslator:
    if not binary_path.exists():
        pytest.skip(f"game binary not present: {binary_path}")
    out = tmp_path_factory.mktemp("lift")
    return run.build(ANALYSIS, SWITCH, binary_path, out, only={TAIL_FN, PIC_FN})


def _lift(bt: BatchTranslator, addr: int) -> str:
    code = bt.translate_single(addr)
    assert code
    return code


def test_indirect_tail_call_uses_dispatch_macro(lifter: BatchTranslator) -> None:
    code = _lift(lifter, TAIL_FN)
    assert re.search(r"RECOMP_ITAIL\(\w+\); return; /\* indirect tail jmp \*/", code)


def test_pic_pair_is_register_assignment(lifter: BatchTranslator) -> None:
    code = _lift(lifter, PIC_FN)
    assert f" ecx = 0x{PIC_POP:08X}u;" in code
    assert f"0x{PIC_POP:08X}u, sub_{PIC_POP:08X}" not in code
    assert f"PUSH32(esp, 0x{PIC_POP:08X}u)" not in code
    assert "POP32" not in code.split(f" ecx = 0x{PIC_POP:08X}u;")[1].split("\n")[1]


def test_switch_emits_one_compare_per_target(lifter: BatchTranslator) -> None:
    entry = next(e for e in json.loads(SWITCH.read_text())["switch"]
                 if e["jmp_addr"] == SWITCH_JMP)
    targets = list(dict.fromkeys(entry["targets"]))
    code = _lift(lifter, PIC_FN)
    compares = re.findall(r"if \(e\w\w == 0x([0-9A-F]{8})u\) goto loc_\1;", code)
    assert [int(t, 16) for t in compares] == targets
    for t in targets:
        assert f"loc_{t:08X}:" in code
