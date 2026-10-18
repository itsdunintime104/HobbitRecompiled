"""Compile and run the C check for the RECOMP_XMM_LOCAL text form (runtime/tests/test_xmm_local.c)."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_xmm_local(tmp_path: Path) -> None:
    exe = tmp_path / "test_xmm_local"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O2", "-arch", "arm64", "-w", "-fno-strict-aliasing",
            "-DRECOMP_GENERATED_CODE",
            "-I", str(ROOT / "tools" / "xrecomp" / "include"),
            "-I", str(ROOT / "runtime" / "include"),
            str(ROOT / "runtime" / "tests" / "test_xmm_local.c"),
            "-o", str(exe), "-lm",
        ],
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "ok"
