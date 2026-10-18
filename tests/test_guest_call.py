"""Compile and run the guest_call unit test."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/guest_call.c",
    "runtime/src/thunk_files.c",
    "runtime/src/handles.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/src/host_funcs.c",
    "runtime/tests/test_guest_call.c",
]


def test_guest_call(tmp_path: Path) -> None:
    exe = tmp_path / "test_guest_call"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-w", "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-o", str(exe),
        ],
        cwd=ROOT,
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
