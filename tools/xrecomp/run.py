"""Lift the game's i386 Mach-O image to C with the ported xboxrecomp lifter.

    python -I -m tools.xrecomp.run --analysis recon/analysis.json \\
        --switch recon/switch_tables.json --bin <image> --out generated.nosync \\
        [--sample 200] [--only 0x1c260,0x21e00] [--jobs N]

Writes <out>/hobbit_NNNN.c (500 functions each), recomp_funcs.h, imports.h,
the dispatch table and lift_report.json. --sample N also writes sample.c
(every len/N-th function) and compiles it with clang for arm64.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Any

from tools.hobbit import emit_inputs
from tools.xrecomp import config
from tools.xrecomp.translator import BatchTranslator, write_if_changed

CHUNK_SIZE = 500
PREFIX = "hobbit"
HEADER = "recomp_funcs.h"
TOP_MNEMONICS = 30
INCLUDE_DIR = Path(__file__).parent / "include"
NATIVE_OVERRIDES = Path(__file__).resolve().parents[2] / "recon" / "native_overrides.txt"


def write_imports_h(stubs: dict[int, str], out: Path) -> None:
    lines = ["/* Host import functions, one per __symbol_stub slot. */",
             "#ifndef HOBBIT_IMPORTS_H", "#define HOBBIT_IMPORTS_H", ""]
    lines += [f"extern void {name}(void);  /* stub 0x{addr:08X} */"
              for addr, name in sorted(stubs.items())]
    lines += ["", "#endif", ""]
    write_if_changed(out / "imports.h", "\n".join(lines))


def build(analysis_path: Path, switch_path: Path, bin_path: Path, out: Path,
          only: set[int] | None = None) -> BatchTranslator:
    """Configure from Mach-O, emit translator inputs, build the translator."""
    analysis = json.loads(analysis_path.read_text())
    switch_tables = json.loads(switch_path.read_text())
    if only is not None:
        analysis = dict(analysis)
        analysis["functions"] = [f for f in analysis["functions"]
                                 if f["addr"] in only]
    config.configure_from_macho(analysis, bin_path)
    out.mkdir(parents=True, exist_ok=True)
    inputs = emit_inputs.emit(analysis, switch_tables, out / "inputs")
    stubs = emit_inputs.import_stubs(analysis)
    write_imports_h(stubs, out)
    bt = BatchTranslator(
        str(bin_path), str(inputs["functions"]),
        labels_json_path=str(inputs["labels"]), output_dir=str(out),
        image_format="macho",
        reg_switch_json_path=str(inputs["reg_switches"]),
        import_stubs=stubs)
    bt.native_overrides = read_native_overrides(NATIVE_OVERRIDES)
    return bt


def read_native_overrides(path: Path) -> set[int]:
    """Guest VAs of lifted functions that runtime/src replaces natively.

    One hex VA per line; '#' starts a comment. A missing file means none."""
    if not path.exists():
        return set()
    vas: set[int] = set()
    for line in path.read_text().splitlines():
        text = line.split("#", 1)[0].strip()
        if text:
            vas.add(int(text, 16))
    return vas


# Set in the parent before the pool forks; the workers inherit it, so the
# translator (minutes to build) is built once and never pickled.
_shared_bt: BatchTranslator | None = None
_shared_funcs: list[tuple[int, Any]] = []


def _translate_range(bounds: tuple[int, int]) -> Any:
    assert _shared_bt is not None
    return _shared_bt.translate_shard(_shared_funcs[bounds[0]:bounds[1]])


def split_ranges(count: int, parts: int) -> list[tuple[int, int]]:
    """Split range(count) into at most `parts` contiguous, near-equal runs."""
    size, extra = divmod(count, max(1, parts))
    ranges, start = [], 0
    for i in range(parts):
        end = start + size + (1 if i < extra else 0)
        if end > start:
            ranges.append((start, end))
        start = end
    return ranges


def translate_parallel(bt: BatchTranslator, func_list: list[tuple[int, Any]],
                       jobs: int) -> list[Any]:
    """Translate contiguous shards of func_list in `jobs` forked processes."""
    global _shared_bt, _shared_funcs
    _shared_bt = bt
    _shared_funcs = [item for item in func_list
                     if item[0] not in bt.translator.owned_function_starts]
    ranges = split_ranges(len(_shared_funcs), jobs)
    with multiprocessing.get_context("fork").Pool(len(ranges)) as pool:
        return pool.map(_translate_range, ranges, chunksize=1)


def sample_addrs(addrs: list[int], n: int) -> list[int]:
    step = max(1, len(addrs) // n)
    return addrs[::step][:n]


def write_sample(bt: BatchTranslator, addrs: list[int], out: Path) -> Path:
    parts = ["#define RECOMP_GENERATED_CODE", f'#include "{HEADER}"',
             "#include <math.h>", ""]
    for addr in addrs:
        parts.append(bt.translate_single(addr)
                     or f"/* sample: 0x{addr:08X} did not translate */")
    path = out / "sample.c"
    path.write_text("\n".join(parts))
    return path


def compile_sample(path: Path, out: Path) -> dict:
    cmd = ["clang", "-std=gnu11", "-arch", "arm64", "-c",
           "-I", str(INCLUDE_DIR), "-I", str(out), str(path), "-o", "/dev/null"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    errors = [ln for ln in proc.stderr.splitlines() if " error: " in ln]
    distinct = list(dict.fromkeys(e.split(" error: ", 1)[1] for e in errors))
    return {"command": " ".join(cmd), "returncode": proc.returncode,
            "errors": len(errors), "first_distinct_errors": distinct[:5]}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--analysis", type=Path, required=True)
    ap.add_argument("--switch", type=Path, required=True)
    ap.add_argument("--bin", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--only", default=None,
                    help="comma-separated hex addresses to lift (debug); "
                         "@FILE reads the list from FILE")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                    help="worker processes (default: all cores); 1 lifts "
                         "in this process")
    args = ap.parse_args()

    only_text = args.only
    if only_text and only_text.startswith("@"):
        only_text = Path(only_text[1:]).read_text().strip()
    only = ({int(a, 16) for a in only_text.split(",")} if only_text else None)
    bt = build(args.analysis, args.switch, args.bin, args.out, only)
    func_list = sorted(bt.func_db.items())
    shards = (translate_parallel(bt, func_list, args.jobs)
              if args.jobs > 1 else None)
    stats = bt.translate_batch_split(
        func_list, str(args.out), chunk_size=CHUNK_SIZE, header_name=HEADER,
        prefix=PREFIX, verbose=True, shards=shards)

    absorbed = sorted(set(bt.func_db) & bt.translator.owned_function_starts)
    unimpl = Counter({m: len(a) for m, a in stats["unimplemented"].items()})
    failures = bt.failures
    report = {
        "functions_in_analysis": len(bt.func_db),
        "functions_lifted": stats["translated"],
        "functions_failed": stats["failed"],
        "failures": {f"0x{a:08X}": r for a, r in sorted(failures.items())},
        "functions_absorbed": len(absorbed),
        "absorbed": [f"0x{a:08X}" for a in absorbed],
        "unresolved_call_stubs": stats["unresolved_stubs"],
        "unhandled_total": sum(unimpl.values()),
        "unhandled_top": dict(unimpl.most_common(TOP_MNEMONICS)),
        "num_chunks": stats["num_chunks"],
    }
    if args.sample:
        lifted = [a for a, _ in func_list
                  if a not in failures and a not in set(absorbed)]
        path = write_sample(bt, sample_addrs(lifted, args.sample), args.out)
        report["sample"] = {"functions": args.sample, "file": str(path),
                            **compile_sample(path, args.out)}
    (args.out / "lift_report.json").write_text(json.dumps(report, indent=1))
    summary = {k: v for k, v in report.items()
               if k not in ("failures", "absorbed")}
    print(json.dumps(summary, indent=1), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
