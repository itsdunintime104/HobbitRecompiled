"""Compile and run the printf-family thunk unit test."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/thunk_printf.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/tests/test_thunk_printf.c",
]


def test_thunk_printf(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_printf"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-Wall",
            "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-o", str(exe),
        ],
        cwd=ROOT,
        check=True,
    )
    run = subprocess.run([str(exe)], capture_output=True, check=False)
    assert run.returncode == 0, run.stderr.decode(errors="replace")
    assert run.stdout.strip() == b"ok"
    assert run.stderr.decode().count("UNSUPPORTED imp__fcntl_UNIX2003: fcntl command 99") == 1
