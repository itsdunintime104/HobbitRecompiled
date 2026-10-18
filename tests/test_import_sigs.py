"""Check recon/import_sigs.json against recon/analysis.json."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from tools.hobbit.import_sigs import Kind, Status

RECON = Path(__file__).resolve().parent.parent / "recon"
IMPORT_COUNT = 378
VENDOR_DYLIBS = ("Cg", "Vapor", "libsteam_api")
VENDOR_IMPORT_COUNT = 30  # Cg 20 + Vapor 10


@pytest.fixture(scope="module")
def sigs() -> dict[str, dict[str, Any]]:
    entries = json.loads((RECON / "import_sigs.json").read_text())["imports"]
    by_name = {e["name"]: e for e in entries}
    assert len(by_name) == len(entries)
    return by_name


def layout(entry: dict[str, Any]) -> tuple[Any, list[tuple[str, int]]]:
    ret = entry["ret"]["kind"] if entry["ret"] else None
    return ret, [(p["kind"], p["stack_size"]) for p in entry["params"]]


def test_every_import_present_once(sigs: dict[str, dict[str, Any]]) -> None:
    analysis = json.loads((RECON / "analysis.json").read_text())
    names = [i["name"] for i in analysis["imports"]]
    assert len(names) == IMPORT_COUNT
    assert sorted(sigs) == sorted(names)
    for imp in analysis["imports"]:
        assert sigs[imp["name"]]["dylib"] == imp["dylib"]


def test_status_values(sigs: dict[str, dict[str, Any]]) -> None:
    assert {e["status"] for e in sigs.values()} <= set(Status)
    assert {p["kind"] for e in sigs.values() for p in e["params"]} <= set(Kind)


def test_vendor_libraries_are_manual(sigs: dict[str, dict[str, Any]]) -> None:
    vendor = [
        e for e in sigs.values() if e["dylib"].rsplit("/", 1)[-1].startswith(VENDOR_DYLIBS)
    ]
    assert len(vendor) == VENDOR_IMPORT_COUNT
    assert all(e["status"] == Status.MANUAL for e in vendor)


def test_i386_layouts(sigs: dict[str, dict[str, Any]]) -> None:
    assert layout(sigs["_glDrawArrays"]) == (
        Kind.VOID,
        [(Kind.U32, 4), (Kind.I32, 4), (Kind.I32, 4)],
    )
    assert layout(sigs["_fseek"]) == (
        Kind.I32,
        [(Kind.PTR, 4), (Kind.I32, 4), (Kind.I32, 4)],
    )
    assert layout(sigs["_memcpy"]) == (
        Kind.PTR,
        [(Kind.PTR, 4), (Kind.PTR, 4), (Kind.U32, 4)],
    )
    assert layout(sigs["_atanf"]) == (Kind.F32, [(Kind.F32, 4)])
    assert layout(sigs["_ldexp"]) == (Kind.F64, [(Kind.F64, 8), (Kind.I32, 4)])
    assert layout(sigs["_mach_absolute_time"]) == (Kind.U64, [])
    assert layout(sigs["___divdi3"]) == (Kind.I64, [(Kind.I64, 8), (Kind.I64, 8)])
    assert layout(sigs["_CFArrayApplyFunction"])[1][1] == (Kind.STRUCT, 8)


def test_variadic(sigs: dict[str, dict[str, Any]]) -> None:
    for name in ("_printf", "_sprintf", "_snprintf"):
        assert sigs[name]["variadic"] is True
    assert sigs["_memcpy"]["variadic"] is False


def test_abi_source(sigs: dict[str, dict[str, Any]]) -> None:
    assert {e["abi_source"] for e in sigs.values()} == {"i386"}
