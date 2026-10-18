"""Compile and run the audio thunk unit test (plays 0.3 s of a quiet 440 Hz tone)."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/thunk_audio.c",
    "runtime/src/guest_call.c",
    "runtime/src/handles.c",
    "runtime/src/dlmalloc.c",
    "runtime/src/guest_heap.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/tests/test_thunk_audio.c",
]


def test_thunk_audio(tmp_path: Path) -> None:
    exe = tmp_path / "test_thunk_audio"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-w", "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES,
            "-framework", "AudioToolbox", "-framework", "CoreAudio",
            "-framework", "CoreFoundation", "-o", str(exe),
        ],
        cwd=ROOT,
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
