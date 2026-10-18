from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

import pytest

from tools.hobbit import analyze, disasm


def _default_binary() -> Path:
    common = subprocess.check_output(["git", "rev-parse", "--git-common-dir"], text=True)
    return Path(common.strip()).resolve().parent / "orig.nosync" / "hobbit_i386"


@pytest.fixture(scope="session")
def binary_path() -> Path:
    path = Path(os.environ["HOBBIT_BIN"]) if "HOBBIT_BIN" in os.environ else _default_binary()
    if not path.is_file():
        pytest.fail(f"game binary not found: {path}")
    return path


@pytest.fixture(scope="session")
def binary_data(binary_path: Path) -> bytes:
    return binary_path.read_bytes()


@pytest.fixture(scope="session")
def analysis(binary_data: bytes, tmp_path_factory: pytest.TempPathFactory) -> dict[str, Any]:
    out = tmp_path_factory.mktemp("analysis") / "analysis.json"
    out.write_text(json.dumps(analyze.analyze(binary_data)))
    return json.loads(out.read_text())


@pytest.fixture(scope="session")
def sweep(
    analysis: dict[str, Any], binary_data: bytes
) -> tuple[dict[str, Any], dict[str, Any]]:
    return disasm.sweep(analysis, binary_data)
