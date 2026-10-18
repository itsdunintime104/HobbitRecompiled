"""Compile and run the IOKit, keyboard-layout and socket thunk unit test."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/thunk_sys.c",
    "runtime/src/thunk_files.c",
    "runtime/src/handles.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    # thunk_sys.c returns host CF objects through thunk_cf.c's handle table.
    "runtime/src/thunk_cf.c",
    "runtime/src/feral_loader.c",
    "runtime/src/thunk_printf.c",
    "runtime/src/guest_call.c",
    "runtime/src/host_funcs.c",
    "runtime/src/data_imports.c",
    "runtime/src/guest_heap.c",
    "runtime/src/dlmalloc.c",
    "runtime/tests/test_thunk_sys.c",
]


def test_thunk_sys(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_sys"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-w", "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-framework", "Carbon", "-framework", "IOKit",
            "-framework", "CoreFoundation", "-o", str(exe),
        ],
        cwd=ROOT,
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
