"""Compile the Cg helper (x86_64) and the Cg thunk unit test (arm64), then run it."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_APP = (
    Path.home() / "Library/Application Support/Steam/steamapps/common/LEGO - The Hobbit/LEGO The Hobbit.app"
)
CG_SUBPATH = "Contents/Frameworks/Cg.framework/Versions/A/Cg"
SOURCES = [
    "runtime/src/thunk_cg.c",
    "runtime/src/dlmalloc.c",
    "runtime/src/guest_heap.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/tests/test_thunk_cg.c",
]


def test_thunk_cg(tmp_path: Path) -> None:
    app = Path(os.environ.get("HOBBIT_APP") or DEFAULT_APP)
    if not (app / CG_SUBPATH).is_file():
        pytest.skip(f"game bundle Cg.framework not found under {app}")
    helper = tmp_path / "cghelper"
    subprocess.run(
        ["clang", "-std=gnu11", "-O1", "-arch", "x86_64", "-o", str(helper), "tools/cghelper/cghelper.c"],
        cwd=ROOT,
        check=True,
    )
    exe = tmp_path / "test_thunk_cg"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-w", "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-o", str(exe),
        ],
        cwd=ROOT,
        check=True,
    )
    env = {**os.environ, "HOBBIT_APP": str(app), "HOBBIT_CGHELPER": str(helper)}
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, env=env)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
