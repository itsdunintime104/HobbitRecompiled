"""Message-passing litmus test: the TSO accessors keep x86 store and load order."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _run_litmus(tmp_path: Path, tso: int, mode: str = "ordered") -> int:
    exe = tmp_path / f"test_tso_litmus_{tso}"
    build = subprocess.run(
        ["clang", "-std=gnu11", "-O1", "-arch", "arm64", "-w", f"-DRECOMP_TSO={tso}",
         "-I", "tools/xrecomp/include", "-I", "runtime/include",
         "runtime/tests/test_tso_litmus.c", "-o", str(exe)],
        cwd=ROOT, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=120)
    print(run.stdout.strip())
    match = re.search(r"violations=(\d+)", run.stdout)
    assert match, run.stdout + run.stderr
    assert run.returncode in (0, 1), run.stderr
    return int(match.group(1))


@pytest.mark.parametrize("mode", ["ordered", "publish", "consume"])
def test_tso_on_has_no_violations(tmp_path: Path, mode: str) -> None:
    assert _run_litmus(tmp_path, 1, mode) == 0


def test_tso_off_reports_count(tmp_path: Path) -> None:
    # No assertion on the count: plain accesses may or may not reorder on this core.
    _run_litmus(tmp_path, 0)
