"""Build and run the guest heap and allocation thunk test program."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/dlmalloc.c",
    "runtime/src/guest_heap.c",
    "runtime/src/thunk_heap.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/tests/test_thunk_heap.c",
]


def test_thunk_heap(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_heap"
    cmd = [
        "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-DRECOMP_GLUE_NO_MAIN", "-w",
        "-I", "tools/xrecomp/include", "-I", "runtime/include",
        *SOURCES, "-o", str(exe),
    ]  # fmt: skip
    subprocess.run(cmd, cwd=ROOT, check=True)
    result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "ok"
