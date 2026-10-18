"""Compile and run the T3 thunk test (stdio, directories, errno, stat, time)."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime"
SOURCES = [
    RUNTIME / "src" / "thunk_files.c",
    RUNTIME / "src" / "handles.c",
    RUNTIME / "src" / "guest_mem.c",
    RUNTIME / "src" / "macho_load.c",
    RUNTIME / "src" / "recomp_glue.c",
    RUNTIME / "tests" / "test_thunk_files.c",
]


def test_thunk_files(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_files"
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    subprocess.run(
        [
            "clang",
            "-std=gnu11",
            "-O1",
            "-arch",
            "arm64",
            "-Wall",
            "-DRECOMP_GLUE_NO_MAIN",
            "-I",
            str(ROOT / "tools" / "xrecomp" / "include"),
            "-I",
            str(RUNTIME / "include"),
            *map(str, SOURCES),
            "-o",
            str(exe),
        ],
        check=True,
    )
    result = subprocess.run([str(exe), str(scratch)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
