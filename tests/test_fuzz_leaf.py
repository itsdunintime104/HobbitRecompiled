"""Smoke test of the leaf-function differential fuzzer on 3 real functions."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from tools.hobbit import fuzz_leaf
from tools.hobbit.fuzz_leaf import Candidate, Worker

SMOKE_ADDRS = (0x0013FD80, 0x00157950, 0x0015B630)
CONFORMANT = {fuzz_leaf.STATUS_PASSED, fuzz_leaf.STATUS_KNOWN_EBP}
ANALYSIS = fuzz_leaf.ROOT / "recon" / "analysis.json"
SWITCHES = fuzz_leaf.ROOT / "recon" / "switch_tables.json"


def _binary() -> Path:
    try:
        return fuzz_leaf.default_binary()
    except FileNotFoundError:
        pytest.skip("original binary not available")


def test_three_real_functions_match_unicorn(tmp_path: Path) -> None:
    worker = Worker(_binary(), ANALYSIS, SWITCHES, tmp_path)
    sizes = {f["addr"]: f["size"] for f in worker.analysis["functions"]}
    results = [worker.run(Candidate(addr, sizes[addr])) for addr in SMOKE_ADDRS]
    assert [r["status"] in CONFORMANT for r in results] == [True] * 3, results
    assert all(any(run["status"] != fuzz_leaf.STATUS_SKIPPED for run in r["runs"]) for r in results)


def test_selection_is_deterministic() -> None:
    analysis = json.loads(ANALYSIS.read_text())
    switches = json.loads(SWITCHES.read_text())
    data = _binary().read_bytes()
    first, total = fuzz_leaf.select_candidates(analysis, data, switches, limit=20)
    again, _ = fuzz_leaf.select_candidates(analysis, data, switches, limit=20)
    assert first == again
    assert 0 < len(first) <= 20 <= total
