"""Compile and run the run-time diagnostics test (trace opens, lockwatch, caller log)."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    "runtime/src/diag.c",
    "runtime/src/thunk_files.c",
    "runtime/src/thunk_threads.c",
    "runtime/src/handles.c",
    "runtime/src/guest_mem.c",
    "runtime/src/macho_load.c",
    "runtime/src/recomp_glue.c",
    "runtime/src/host_funcs.c",
    "runtime/tests/test_diag.c",
]


@pytest.fixture(scope="module")
def exe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("diag") / "test_diag"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-arch", "arm64", "-Wall", "-DRECOMP_GLUE_NO_MAIN",
            "-I", "tools/xrecomp/include", "-I", "runtime/include",
            *SOURCES, "-o", str(out),
        ],
        cwd=ROOT,
        check=True,
    )
    return out


def run(exe: Path, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
    clean = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_")}
    return subprocess.run(
        [str(exe), *args], capture_output=True, text=True, timeout=60, env={**clean, **env}
    )


def test_trace_opens(exe: Path, tmp_path: Path) -> None:
    target = tmp_path / "file.txt"
    target.write_text("x")
    result = run(exe, "opens", str(target), RECOMP_TRACE_OPENS="1")
    assert result.returncode == 0, result.stderr
    assert f"OPEN {target} r -> ok" in result.stderr
    assert "OPEN /nonexistent-diag-test-path r -> errno=2" in result.stderr


def test_trace_opens_off_by_default(exe: Path, tmp_path: Path) -> None:
    target = tmp_path / "file.txt"
    target.write_text("x")
    result = run(exe, "opens", str(target))
    assert result.returncode == 0, result.stderr
    assert "OPEN " not in result.stderr


def test_lockwatch_reports_stuck_lock(exe: Path) -> None:
    result = run(exe, "lock", RECOMP_LOCKWATCH="1")
    assert result.returncode == 0, result.stderr
    assert result.stderr.count("STUCK-LOCK guest_mutex=0x00F00000 owner_thread=") == 1


def test_no_lockwatch_warning_by_default(exe: Path) -> None:
    result = run(exe, "lock")
    assert result.returncode == 0, result.stderr
    assert "STUCK-LOCK" not in result.stderr


def test_failure_log_has_return_address(exe: Path) -> None:
    result = run(exe, "fail")
    assert result.returncode == 3
    assert "indirect call to unresolved VA 0x00001234" in result.stderr
    line = next(ln for ln in result.stderr.splitlines() if ln.startswith("RECOMP:"))
    assert "caller=0xCAFEBABE" in line
    assert "icalls=[0x00402000 0x00401000" in line


def test_unsupported_prints_once(exe: Path) -> None:
    result = run(exe, "unsupported")
    assert result.returncode == 0, result.stderr
    lines = [ln for ln in result.stderr.splitlines() if "UNSUPPORTED" in ln]
    assert lines == [
        "UNSUPPORTED f: value 7 (guest return 0x00001234)",
        "UNSUPPORTED f: value 8 (guest return 0x00005678)",
    ]


def test_no_cache_switch(exe: Path) -> None:
    assert "caches_off=0" in run(exe, "caches").stdout
    assert "caches_off=1" in run(exe, "caches", RECOMP_NO_CACHE="1").stdout
