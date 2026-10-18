"""Compile and run the host function and data import test program."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/data_imports.c",
    "runtime/src/host_funcs.c",
    "runtime/src/guest_heap.c",
    "runtime/src/dlmalloc.c",
    "runtime/src/thunk_files.c",
    "runtime/src/handles.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/tests/test_data_imports.c",
]


def build(tmp_path: Path) -> Path:
    exe = tmp_path / "test_data_imports"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-DRECOMP_GLUE_NO_MAIN", "-w",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-framework", "OpenGL", "-o", str(exe),
        ],
        cwd=ROOT,
        check=True,
    )
    return exe


def test_data_imports(tmp_path: Path) -> None:
    exe = build(tmp_path)
    result = subprocess.run(
        [str(exe), str(tmp_path / "fake_image")], capture_output=True, text=True, timeout=120
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"


def test_real_image_external_relocations(tmp_path: Path, binary_path: Path) -> None:
    exe = build(tmp_path)
    result = subprocess.run(
        [str(exe), "--real", str(binary_path)], capture_output=True, text=True, timeout=120
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
    match = re.search(r"data_imports: (\d+) extrel bound", result.stderr)
    assert match is not None, result.stderr
    assert int(match.group(1)) >= 433
