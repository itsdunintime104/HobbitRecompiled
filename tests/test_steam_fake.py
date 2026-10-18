"""Compile and run the fake Steam interface test."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/steam_fake.c",
    "runtime/src/data_imports.c",
    "runtime/src/guest_call.c",
    "runtime/src/guest_heap.c",
    "runtime/src/dlmalloc.c",
    "runtime/src/handles.c",
    "runtime/src/host_funcs.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/tests/test_steam_fake.c",
]


# The same bytes as SEED in runtime/tests/test_steam_fake.c.
SEED = b"seed\x00\x01\xfe\xff-end"
STEAM_SAVE = "Library/Application Support/Steam/userdata/1/285160/remote/savedgames/slot1/game1.savegamedata"
ROOT_SAVE = "Library/Application Support/HobbitRecomp/remote/savedgames/slot1/game1.savegamedata"


def _files(top: Path) -> dict[str, bytes]:
    return {str(p.relative_to(top)): p.read_bytes() for p in sorted(top.rglob("*")) if p.is_file()}


def test_steam_fake(tmp_path: Path) -> None:
    exe = tmp_path / "test_steam_fake"
    home = tmp_path / "home"
    steam_file = home / STEAM_SAVE
    steam_file.parent.mkdir(parents=True)
    steam_file.write_bytes(SEED)
    build = subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-w", "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-framework", "OpenGL", "-o", str(exe),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert build.returncode == 0, build.stderr
    env = {k: v for k, v in os.environ.items() if k != "HOBBIT_SAVE_DIR"}
    env["HOME"] = str(home)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
    root = home / "Library/Application Support/HobbitRecomp/remote"
    assert f"steam: saves in {root} (seeded from {steam_file.parents[2]})" in result.stderr
    # The Steam file is untouched, the seeded copy is the only file left in
    # HOME besides it, and nothing was written outside the root.
    assert steam_file.read_bytes() == SEED
    assert _files(home) == {STEAM_SAVE: SEED, ROOT_SAVE: SEED}
    assert _files(home / "Library/Application Support/Steam") == {
        STEAM_SAVE.removeprefix("Library/Application Support/Steam/"): SEED
    }
