"""Compile and run the C checks for runtime/src/thunk_gl.c (hidden CGL context)."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    ROOT / "runtime" / "src" / "thunk_gl.c",
    ROOT / "runtime" / "src" / "thunk_gl_ext.c",
    ROOT / "runtime" / "src" / "thunk_rt.c",
    ROOT / "runtime" / "src" / "steam_fake.c",
    ROOT / "runtime" / "src" / "data_imports.c",
    ROOT / "runtime" / "src" / "handles.c",
    ROOT / "runtime" / "src" / "host_funcs.c",
    ROOT / "runtime" / "src" / "guest_heap.c",
    ROOT / "runtime" / "src" / "dlmalloc.c",
    ROOT / "runtime" / "src" / "guest_mem.c",
    ROOT / "runtime" / "src" / "macho_load.c",
    ROOT / "runtime" / "src" / "recomp_glue.c",
    ROOT / "runtime" / "tests" / "test_thunk_gl.c",
]


def test_thunk_gl(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_gl"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-w", "-arch", "arm64", "-DRECOMP_GLUE_NO_MAIN",
            "-I", str(ROOT / "tools" / "xrecomp" / "include"),
            "-I", str(ROOT / "runtime" / "include"),
            *map(str, SOURCES),
            "-framework", "OpenGL",
            "-o", str(exe),
        ],
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert "thunk_gl ok" in result.stdout
