from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
ANALYSIS = ROOT / "recon" / "analysis.json"
SWITCH = ROOT / "recon" / "switch_tables.json"
SUBSET_STEP = 41
FUNC_BODY = re.compile(
    r"^/\*\*\n \* (sub_[0-9A-F]{8})\n.*?(?=^/\*\*\n \* sub_|\Z)", re.M | re.S)


def _lift(binary: Path, only_file: Path, out: Path, jobs: int) -> None:
    subprocess.run(
        [sys.executable, "-m", "tools.xrecomp.run",
         "--analysis", str(ANALYSIS), "--switch", str(SWITCH),
         "--bin", str(binary), "--out", str(out),
         "--only", f"@{only_file}", "--jobs", str(jobs)],
        cwd=ROOT, check=True, capture_output=True, text=True)


def _bodies(out: Path) -> dict[str, str]:
    return {m.group(1): m.group(0)
            for chunk in sorted(out.glob("hobbit_[0-9]*.c"))
            for m in FUNC_BODY.finditer(chunk.read_text())}


def test_parallel_lift_matches_serial(binary_path: Path, tmp_path: Path) -> None:
    funcs = json.loads(ANALYSIS.read_text())["functions"]
    only = ",".join(f"0x{f['addr']:x}" for f in funcs[::SUBSET_STEP])
    only_file = tmp_path / "only.txt"
    only_file.write_text(only)
    serial, parallel = tmp_path / "serial", tmp_path / "parallel"
    _lift(binary_path, only_file, serial, 1)
    _lift(binary_path, only_file, parallel, 10)

    serial_bodies, parallel_bodies = _bodies(serial), _bodies(parallel)
    # The lifter adds jump-table targets to the requested set.
    assert {f"sub_{int(a, 16):08X}" for a in only.split(",")} <= serial_bodies.keys()
    assert sorted(serial_bodies.items()) == sorted(parallel_bodies.items())
    assert ((serial / "hobbit_dispatch.c").read_text()
            == (parallel / "hobbit_dispatch.c").read_text())
    assert ((serial / "recomp_funcs.h").read_text()
            == (parallel / "recomp_funcs.h").read_text())
    assert (json.loads((serial / "lift_report.json").read_text())
            == json.loads((parallel / "lift_report.json").read_text()))
