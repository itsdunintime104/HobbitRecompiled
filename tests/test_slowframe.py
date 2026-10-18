"""Compile and run the C check for runtime/src/slowframe.c (slow-frame stack sampler)."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_slowframe(tmp_path: Path) -> None:
    exe = tmp_path / "test_slowframe"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-fno-omit-frame-pointer",
            "-I", str(ROOT / "runtime" / "include"),
            str(ROOT / "runtime" / "src" / "slowframe.c"),
            str(ROOT / "runtime" / "tests" / "test_slowframe.c"),
            "-o", str(exe),
        ],
        check=True,
    )
    env = {**os.environ, "RECOMP_SLOWFRAME_MS": "20"}
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
    assert result.returncode == 0, result.stderr
    lasted = [float(m) for m in re.findall(r"SLOWFRAME: .* lasted ([0-9.]+) ms", result.stderr)]
    assert lasted, result.stderr
    assert all(ms >= 20 for ms in lasted), result.stderr  # the 5 ms frame printed nothing
    assert "busy_wait_marker" in result.stderr
