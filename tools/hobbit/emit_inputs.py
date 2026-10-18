"""Turn our analyzer output into the JSON inputs the xrecomp translator reads.

analysis.json + switch_tables.json ->
  functions.json     [{start, end, name}]                 (hex strings)
  labels.json        [{address, name, type: "import_stub"}]
  reg_switches.json  {jmp_addr: [target, ...]}            (hex strings)
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

STUB_KIND = "stub"
STUB_LABEL_TYPE = "import_stub"
_NON_IDENT = re.compile(r"[^A-Za-z0-9_]")


def import_name(symbol: str) -> str:
    """C name of the host import function for a __symbol_stub slot."""
    return "imp_" + _NON_IDENT.sub("_", symbol)


def functions(analysis: dict) -> list[dict[str, str]]:
    return [
        {"start": f"0x{f['addr']:08X}", "end": f"0x{f['addr'] + f['size']:08X}",
         "name": f"sub_{f['addr']:08X}"}
        for f in analysis["functions"]
    ]


def import_stubs(analysis: dict) -> dict[int, str]:
    """{stub VA: imp_<symbol>} for every 6-byte __symbol_stub slot."""
    return {s["addr"]: import_name(s["symbol"])
            for s in analysis["indirect_slots"] if s["kind"] == STUB_KIND}


def labels(analysis: dict) -> list[dict[str, str]]:
    return [{"address": f"0x{addr:08X}", "name": name, "type": STUB_LABEL_TYPE}
            for addr, name in sorted(import_stubs(analysis).items())]


def reg_switches(switch_tables: dict) -> dict[str, list[str]]:
    return {f"0x{e['jmp_addr']:08X}": [f"0x{t:08X}" for t in e["targets"]]
            for e in switch_tables["switch"]}


def emit(analysis: dict, switch_tables: dict, out_dir: Path) -> dict[str, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = {
        "functions": out_dir / "functions.json",
        "labels": out_dir / "labels.json",
        "reg_switches": out_dir / "reg_switches.json",
    }
    paths["functions"].write_text(json.dumps(functions(analysis)))
    paths["labels"].write_text(json.dumps(labels(analysis)))
    paths["reg_switches"].write_text(json.dumps(reg_switches(switch_tables)))
    return paths


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--analysis", type=Path, required=True)
    ap.add_argument("--switch", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    emit(json.loads(args.analysis.read_text()),
         json.loads(args.switch.read_text()), args.out)


if __name__ == "__main__":
    main()
