"""Check the guest memory window and Mach-O loader against the file bytes."""

from __future__ import annotations

import json
import random
import subprocess
from pathlib import Path
from typing import Any

import pytest

from tools.hobbit import emit_runtime

RUNTIME = Path(__file__).resolve().parents[1] / "runtime"
LOADER = RUNTIME / "build.nosync" / "test_loader"
LOW_RESERVED = 0x4000
SAMPLE_LEN = 16
SAMPLES = 1000
ZERO_SECTIONS = ("__bss", "__common")


def _sections(analysis: dict[str, Any]) -> list[tuple[str, str, int, int, int]]:
    """Return (segment, section, addr, size, file offset) for every section."""
    result: list[tuple[str, str, int, int, int]] = []
    for seg in analysis["segments"]:
        for sec in seg["sections"]:
            result.append((seg["name"], sec["name"], sec["addr"], sec["size"], sec["offset"]))
    return result


def _run_loader(binary: Path, *args: str) -> list[str]:
    out = subprocess.run(
        [str(LOADER), str(binary), *args], check=True, capture_output=True, text=True
    ).stdout
    lines = out.splitlines()
    assert lines[0].startswith("base=0x")
    return lines[1:]


@pytest.fixture(scope="module")
def loader_built(binary_path: Path) -> Path:
    subprocess.run(
        ["make", "-C", str(RUNTIME), "test", f"HOBBIT_BIN={binary_path}"], check=True
    )
    return binary_path


@pytest.fixture(scope="module")
def recon_analysis() -> dict[str, Any]:
    return json.loads(emit_runtime.ANALYSIS.read_text())


def test_gen_segments_is_current(recon_analysis: dict[str, Any]) -> None:
    assert emit_runtime.OUTPUT.read_text() == emit_runtime.render(recon_analysis)


def test_file_bytes_match(
    loader_built: Path, binary_data: bytes, recon_analysis: dict[str, Any]
) -> None:
    groups: dict[str, list[tuple[int, int, int]]] = {
        "text": [],
        "const_text": [],
        "cstring": [],
        "data": [],
        "const_data": [],
        "objc": [],
    }
    for seg, name, addr, size, offset in _sections(recon_analysis):
        key: str | None = None
        if seg == "__TEXT" and name == "__text":
            key = "text"
        elif seg == "__TEXT" and name == "__const":
            key = "const_text"
        elif seg == "__TEXT" and name == "__cstring":
            key = "cstring"
        elif seg == "__DATA" and name == "__data":
            key = "data"
        elif seg == "__DATA" and name == "__const":
            key = "const_data"
        elif seg == "__OBJC":
            key = "objc"
        if key is not None:
            groups[key].append((addr, size, offset))

    rng = random.Random(1234)
    keys = list(groups)
    picks: list[tuple[int, int, int]] = []
    for i in range(SAMPLES):
        addr, size, offset = rng.choice(groups[keys[i % len(keys)]])
        length = min(SAMPLE_LEN, size)
        guest = rng.randint(max(addr, LOW_RESERVED), addr + size - length)
        picks.append((guest, offset + (guest - addr), length))

    args: list[str] = []
    for guest, _, length in picks:
        args += ["--dump", f"{guest:x}", f"{length:x}"]
    lines = _run_loader(loader_built, *args)
    assert len(lines) == SAMPLES
    for (guest, off, length), line in zip(picks, lines):
        assert line == binary_data[off : off + length].hex(), hex(guest)


def test_bss_and_common_read_zero(loader_built: Path, recon_analysis: dict[str, Any]) -> None:
    rng = random.Random(1234)
    args: list[str] = []
    count = 0
    for _, name, addr, size, _ in _sections(recon_analysis):
        if name in ZERO_SECTIONS:
            for guest in (addr, addr + size - SAMPLE_LEN, rng.randint(addr, addr + size - SAMPLE_LEN)):
                args += ["--dump", f"{guest:x}", f"{SAMPLE_LEN:x}"]
                count += 1
    lines = _run_loader(loader_built, *args)
    assert len(lines) == count
    assert all(line == "00" * SAMPLE_LEN for line in lines)


def test_low_pages_fault(loader_built: Path) -> None:
    lines = _run_loader(
        loader_built, "--probe", "0", "--probe", "fff", "--probe", "1000", "--probe", "2720"
    )
    assert lines == ["fault"] * 4
    assert _run_loader(loader_built, "--probe", "4000") != ["fault"]


def test_header_side_buffer(loader_built: Path, binary_data: bytes) -> None:
    (line,) = _run_loader(loader_built, "--header", "0", "20")
    assert line == binary_data[:32].hex()
    assert line.startswith("cefaedfe")
