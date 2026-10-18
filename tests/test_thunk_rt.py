"""Build and run the unit test for runtime/src/thunk_rt.c."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_thunk_rt(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_rt"
    cmd = [
        "clang",
        "-std=gnu11",
        "-O1",
        "-Wall",
        "-arch",
        "arm64",
        "-DRECOMP_GLUE_NO_MAIN",
        "-I",
        str(ROOT / "tools/xrecomp/include"),
        "-I",
        str(ROOT / "runtime/include"),
        str(ROOT / "runtime/src/thunk_rt.c"),
        str(ROOT / "runtime/src/steam_fake.c"),
        str(ROOT / "runtime/src/data_imports.c"),
        str(ROOT / "runtime/src/handles.c"),
        str(ROOT / "runtime/src/thunk_gl.c"),
        str(ROOT / "runtime/src/thunk_gl_ext.c"),
        str(ROOT / "runtime/src/guest_heap.c"),
        str(ROOT / "runtime/src/dlmalloc.c"),
        str(ROOT / "runtime/src/guest_mem.c"),
        str(ROOT / "runtime/src/macho_load.c"),
        str(ROOT / "runtime/src/recomp_glue.c"),
        str(ROOT / "runtime/src/host_funcs.c"),
        str(ROOT / "runtime/tests/test_thunk_rt.c"),
        "-framework",
        "OpenGL",
        "-o",
        str(exe),
    ]
    build = subprocess.run(cmd, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert "thunk_rt ok" in run.stdout
