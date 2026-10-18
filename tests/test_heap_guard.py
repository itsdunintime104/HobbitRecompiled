"""Build and run the guest heap guard test program, one case per process."""

from __future__ import annotations

import os
import signal
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/dlmalloc.c",
    "runtime/src/guest_heap.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/tests/test_heap_guard.c",
]
ABORT = -signal.SIGABRT
SITES = ["alloc site 0x00001000", "free site 0x00002000"]


@pytest.fixture(scope="module")
def exe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("heap_guard") / "test_heap_guard"
    cmd = [
        "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-DRECOMP_GLUE_NO_MAIN", "-w",
        "-I", "tools/xrecomp/include", "-I", "runtime/include",
        *SOURCES, "-lpthread", "-o", str(out),
    ]  # fmt: skip
    subprocess.run(cmd, cwd=ROOT, check=True)
    return out


def run_case(exe: Path, case: str, mode: str | None) -> subprocess.CompletedProcess[str]:
    env = {k: v for k, v in os.environ.items() if k != "RECOMP_HEAP_GUARD"}
    if mode is not None:
        env["RECOMP_HEAP_GUARD"] = mode
    return subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=60)


def test_ok(exe: Path) -> None:
    result = run_case(exe, "ok", mode="1")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"


@pytest.mark.parametrize(
    ("case", "kind"),
    [("overrun", "overrun"), ("uaf", "write after free"), ("double", "double free")],
)
def test_bad_heap_use_aborts(exe: Path, case: str, kind: str) -> None:
    result = run_case(exe, case, mode="1")
    assert result.returncode == ABORT, result.stderr
    assert f"guest_heap_guard: {kind} block" in result.stderr
    assert "last icalls" in result.stderr
    for site in SITES:
        assert site in result.stderr


def test_overrun_offset(exe: Path) -> None:
    result = run_case(exe, "overrun", mode="1")
    assert "offset 0" in result.stderr


def test_uaf_offset(exe: Path) -> None:
    result = run_case(exe, "uaf", mode="1")
    assert "offset 8" in result.stderr


def test_guard_off(exe: Path) -> None:
    result = run_case(exe, "off", mode=None)
    assert result.returncode == 0, result.stderr
    assert "guest_heap_guard" not in result.stderr


@pytest.mark.parametrize(
    ("case", "kind"),
    [("overrun", "overrun"), ("uaf_evict", "write after free"), ("double", "double free")],
)
def test_continue_mode_logs_once(exe: Path, case: str, kind: str) -> None:
    result = run_case(exe, case, mode="2")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "ok"
    assert result.stderr.count(f"guest_heap_guard: {kind} block") == 1, result.stderr
