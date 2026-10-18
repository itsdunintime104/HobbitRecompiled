"""Compile and run the C checks for the auto-generated thunks."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    ROOT / "runtime" / "src" / "thunk_auto.c",
    ROOT / "runtime" / "src" / "guest_mem.c",
    ROOT / "runtime" / "src" / "macho_load.c",
    ROOT / "runtime" / "src" / "recomp_glue.c",
    ROOT / "runtime" / "tests" / "test_thunk_auto.c",
]


def test_thunk_auto(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_auto"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-DRECOMP_GLUE_NO_MAIN",
            "-I", str(ROOT / "tools" / "xrecomp" / "include"),
            "-I", str(ROOT / "runtime" / "include"),
            *map(str, SOURCES),
            "-framework", "OpenGL",
            "-o", str(exe),
        ],
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("PASS") == 5, result.stdout
