"""Compile and run the pthread and OSAtomic thunk test program."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/thunk_threads.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/src/host_funcs.c",
    "runtime/tests/test_thunk_threads.c",
]


def test_thunk_threads(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_threads"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-o", str(exe),
        ],
        cwd=ROOT,
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert "mutex ok: counter=400000" in result.stdout
    assert "create ok: handle=1 ret=0xC05A4B dtor_arg=0x55" in result.stdout
    assert result.stdout.strip().endswith("all ok")
