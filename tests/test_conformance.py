"""Differential conformance: lifted C (arm64) must match Unicorn on the same snippet."""

from __future__ import annotations

import pytest

from tests.conformance.cases import CASES, SEEDS
from tests.conformance.harness import Case, diff


def _param(case: Case, seed: int) -> object:
    marks = [pytest.mark.xfail(strict=True, reason=case.xfail)] if case.xfail else []
    return pytest.param(case, seed, id=f"{case.name}-{seed}", marks=marks)


@pytest.mark.parametrize("case,seed", [_param(c, s) for c in CASES for s in SEEDS])
def test_lifted_matches_unicorn(case: Case, seed: int) -> None:
    assert diff(case, seed) == []
