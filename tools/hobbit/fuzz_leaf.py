"""Differential fuzz of real leaf functions: Unicorn (oracle) against lifted arm64 C.

Usage: python -m tools.hobbit.fuzz_leaf --binary <hobbit_i386> --work <dir> [--workers 8]

A candidate is a function of 8..400 bytes with no real call, no indirect jump
outside its own switch tables, no jump out of the function and no x87. The
image's segments are mapped at their real addresses on both sides, so PIC loads
and constant tables read real data. Each function runs with 3 seeds; a seed
where Unicorn itself faults is skipped. EFLAGS are not compared.

The stack below the final esp is not compared. Report: <work>/report.json. Mismatches are grouped by the mnemonic of the last
executed instruction that wrote the first differing location (a heuristic, not
a proof). A diff in ebp alone is the known leaf-ebp write-back bug and is only
counted.
"""

from __future__ import annotations

import argparse
import json
import math
import multiprocessing
import random
import re
import struct
import subprocess
import tempfile
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_32
from unicorn.unicorn import Uc
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP

from tests.conformance import harness
from tests.conformance.harness import GPRS, State, Xmm
from tools.hobbit import emit_inputs
from tools.hobbit.disasm import SUBREG_FAMILY, _function_code, _sweep_function, _table_ranges

ROOT = Path(__file__).resolve().parents[2]
MIN_SIZE, MAX_SIZE, MAX_SAMPLE = 8, 400, 3000
SEEDS = (0, 1, 2)
MAX_INSNS = 200_000
NATIVE_TIMEOUT = 20.0
PAGE = 0x1000
DATA_BASE = 0x02100000
DATA_SIZE = 0x10000
POINTER_SPAN = 0x7000  # args point into [DATA_BASE + 0x100, DATA_BASE + POINTER_SPAN)
STACK_TOP = DATA_BASE + 0xC000  # stack grows down inside the random page; args sit above
DEAD_STACK = (0x8000, STACK_TOP - DATA_BASE)  # below the final esp: dead once the function returns
SENTINEL = 0x02200000
MEM_SIZE = SENTINEL + PAGE
ARG_COUNT = 8
MAPPED_SEGMENTS = ("__TEXT", "__DATA")
BAD_MNEMONICS = frozenset({"cpuid", "rdtsc", "int", "int3", "hlt", "ud2", "sysenter", "in", "out"})
STATUS_PASSED, STATUS_MISMATCH, STATUS_SKIPPED = "passed", "mismatch", "skipped"
STATUS_LIFT_FAIL, STATUS_COMPILE_FAIL, STATUS_KNOWN_EBP = "lift_fail", "compile_fail", "known_ebp"
# A switch that does not resolve falls back to an indirect tail call: abort, so the seed
# is reported as a native crash instead of failing to link.
DISPATCH_STUBS = """
void recomp_icall_fail_log(uint32_t va) { (void)va; abort(); }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; abort(); }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; abort(); }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; abort(); }
"""
HEX_TARGET = re.compile(r"^0x[0-9a-f]+$")
EBP_ONLY = ("ebp",)


@dataclass(frozen=True)
class Candidate:
    addr: int
    size: int


def default_binary() -> Path:
    """The original binary: in this checkout, or in the main checkout above a worktree."""
    for base in (ROOT, ROOT.parents[2]):
        path = base / "orig.nosync" / "hobbit_i386"
        if path.exists():
            return path
    raise FileNotFoundError("orig.nosync/hobbit_i386 not found; pass --binary")


def _is_candidate(insns: list[Any], failures: list[int], addr: int, size: int,
                  switch_jumps: set[int]) -> bool:
    if failures:
        return False
    end = addr + size
    for i, insn in enumerate(insns):
        mnem, ops = insn.mnem, insn.ops
        if mnem[0] == "f" or mnem in BAD_MNEMONICS:
            return False
        if mnem == "call":
            nxt = insns[i + 1] if i + 1 < len(insns) else None
            pic = (HEX_TARGET.match(ops) and int(ops, 16) == insn.addr + insn.size
                   and nxt is not None and nxt.mnem == "pop")
            if not pic:
                return False
        elif mnem == "jmp" and not HEX_TARGET.match(ops):
            if insn.addr not in switch_jumps:
                return False
        elif (mnem[0] == "j" or mnem.startswith("loop")) and HEX_TARGET.match(ops):
            if not addr <= int(ops, 16) < end:
                return False
    return True


def select_candidates(analysis: dict[str, Any], data: bytes, switch_tables: dict[str, Any],
                      limit: int = MAX_SAMPLE) -> tuple[list[Candidate], int]:
    """Every k-th candidate by address, at most `limit`; also returns the candidate total."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    tables: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for entry in switch_tables["switch"]:
        tables[entry["func_addr"]].append(entry)
    found: list[Candidate] = []
    for func in sorted(analysis["functions"], key=lambda f: f["addr"]):
        addr, size = func["addr"], func["size"]
        if not MIN_SIZE <= size <= MAX_SIZE:
            continue
        own = tables.get(addr, [])
        insns, failures = _sweep_function(
            md, _function_code(analysis, data, func), addr, _table_ranges(own))
        if _is_candidate(insns, failures, addr, size, {t["jmp_addr"] for t in own}):
            found.append(Candidate(addr, size))
    step = max(1, math.ceil(len(found) / limit))
    return found[::step][:limit], len(found)


def _random_xmm(rng: random.Random) -> Xmm:
    bits = struct.unpack("<4I", struct.pack("<4f", *(rng.uniform(-1e4, 1e4) for _ in range(4))))
    return bits[0], bits[1], bits[2], bits[3]


def make_state(addr: int, seed: int) -> State:
    """Seed 0: pointers everywhere. Seed 1: half the args are pointers. Seed 2: small ints."""
    rng = random.Random(f"{addr}:{seed}")

    def pointer() -> int:
        return DATA_BASE + (rng.randrange(0x100, POINTER_SPAN) & ~(15 if rng.random() < 0.5 else 3))

    gpr = {r: rng.getrandbits(32) for r in GPRS}
    if seed == 0:
        for reg in GPRS:
            gpr[reg] = pointer()
    gpr["esp"] = STACK_TOP
    data = bytearray(rng.randbytes(DATA_SIZE))
    for i in range(ARG_COUNT):
        if seed == 0 or (seed == 1 and rng.random() < 0.5):
            arg = pointer()
        elif seed == 1:
            arg = rng.getrandbits(32)
        else:
            arg = rng.randrange(0, 256)
        struct.pack_into("<I", data, STACK_TOP - DATA_BASE + 4 * i, arg)
    return State(gpr, tuple(_random_xmm(rng) for _ in range(harness.XMM_COUNT)), bytes(data))


class Image:
    """The binary's mapped segments, read once per process."""

    def __init__(self, path: Path, analysis: dict[str, Any]) -> None:
        self.path = path
        raw = path.read_bytes()
        self.segments = [
            (seg["vmaddr"], seg["vmsize"], raw[seg["fileoff"]:seg["fileoff"] + seg["filesize"]],
             seg["fileoff"])
            for seg in analysis["segments"] if seg["name"] in MAPPED_SEGMENTS]

    def native_setup(self) -> str:
        loads = "\n".join(
            f"    fseek(img, {off}, SEEK_SET); fread(host + {vmaddr}, 1, {len(blob)}, img);"
            for vmaddr, _, blob, off in self.segments)
        return f'    FILE *img = fopen("{self.path}", "rb");\n{loads}\n    fclose(img);'


class Skipped(Exception):
    """Unicorn itself faulted or did not finish: the seed says nothing about the lifter."""


def run_unicorn(image: Image, addr: int, state: State, hooks: Any = None) -> dict[str, object]:
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    for vmaddr, vmsize, blob, _ in image.segments:
        uc.mem_map(vmaddr, (vmsize + PAGE - 1) & ~(PAGE - 1))
        uc.mem_write(vmaddr, blob)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    uc.mem_write(DATA_BASE, state.data)
    uc.mem_map(SENTINEL, PAGE)
    uc.mem_write(SENTINEL, b"\x90")
    for reg, name in zip(harness._UC_GPRS, GPRS):
        uc.reg_write(reg, state.gpr[name])
    for reg, lanes in zip(harness._UC_XMM, state.xmm):
        uc.reg_write(reg, sum(v << (32 * i) for i, v in enumerate(lanes)))
    esp = state.gpr["esp"] - 4
    uc.mem_write(esp, struct.pack("<I", SENTINEL))
    uc.reg_write(UC_X86_REG_ESP, esp)
    if hooks:
        hooks(uc)
    try:
        uc.emu_start(addr, SENTINEL, count=MAX_INSNS)
    except Exception as exc:
        raise Skipped(str(exc)) from exc
    if uc.reg_read(UC_X86_REG_EIP) != SENTINEL:
        raise Skipped("instruction limit")
    gpr = {name: int(uc.reg_read(reg)) for reg, name in zip(harness._UC_GPRS, GPRS)}
    xmm = [harness._lanes(int(uc.reg_read(reg))) for reg in harness._UC_XMM]
    return harness._result(gpr, xmm, bytes(uc.mem_read(DATA_BASE, DATA_SIZE)), state)


class Culprit:
    """Which executed instruction last wrote a differing location (Unicorn trace)."""

    def __init__(self, image: Image, addr: int, size: int, state: State) -> None:
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        raw = next(blob for vmaddr, _, blob, _ in image.segments if vmaddr == 0x1000)
        self.code = raw[addr - 0x1000:addr - 0x1000 + size]
        self.addr = addr
        self.pcs: list[int] = []
        self.writes: list[tuple[int, int, int]] = []
        run_unicorn(image, addr, state, self._hooks)

    def _hooks(self, uc: Uc) -> None:
        def on_code(_uc: Uc, pc: int, _size: int, _user: object) -> None:
            self.pcs.append(pc)

        def on_write(_uc: Uc, _access: int, address: int, size: int, _value: int,
                     _user: object) -> None:
            self.writes.append((self.pcs[-1], address, size))

        uc.hook_add(UC_HOOK_CODE, on_code)
        uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

    def _insn(self, pc: int) -> Any:
        off = pc - self.addr
        return next(self.md.disasm(self.code[off:off + 15], pc, 1))

    def _writers(self, family: str) -> int | None:
        for pc in reversed(self.pcs):
            insn = self._insn(pc)
            if insn.mnemonic == "ret":
                continue
            written = {SUBREG_FAMILY.get(insn.reg_name(r), insn.reg_name(r))
                       for r in insn.regs_access()[1]}
            if family in written:
                return pc
        return None

    def find(self, key: str, offset: int | None) -> tuple[int, str] | None:
        if key == "data" and offset is not None:
            target = DATA_BASE + offset
            pc = next((p for p, a, s in reversed(self.writes) if a <= target < a + s), None)
        else:
            pc = self._writers("esp" if key == "esp_delta" else key)
        if pc is None:
            return None
        insn = self._insn(pc)
        return pc, f"{insn.mnemonic} {insn.op_str}"


def _mask_dead_stack(result: dict[str, object]) -> dict[str, object]:
    """Zero the stack below the final esp: the lifter elides stack writes such as the
    return address of `call next; pop`, and nothing may read that memory after the ret."""
    data = str(result["data"])
    lo, hi = DEAD_STACK[0] * 2, DEAD_STACK[1] * 2
    return {**result, "data": data[:lo] + "0" * (hi - lo) + data[hi:]}


def _lane_is_nan(value: int) -> bool:
    return math.isnan(struct.unpack("<f", struct.pack("<I", value & 0xFFFFFFFF))[0])


def _nan_suspect(first: str, want: dict[str, object], got: dict[str, object]) -> bool:
    """A NaN on either side of the first difference: x86 and arm64 disagree on NaN sign."""
    key = first.split(" ", 1)[0].rstrip(":")
    if key.startswith("xmm"):
        lanes = [*want[key], *got[key]]  # type: ignore[misc]
        return any(_lane_is_nan(v) for v in lanes)
    if key == "data":
        off = int(first.split("+0x")[1].split(":")[0], 16) & ~3
        dwords = [struct.unpack_from("<I", bytes.fromhex(str(r["data"])), off)[0] for r in (want, got)]
        return any(_lane_is_nan(v) for v in dwords)
    return False


def disassemble(image: Image, cand: Candidate) -> list[str]:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    raw = next(blob for vmaddr, _, blob, _ in image.segments if vmaddr == 0x1000)
    code = raw[cand.addr - 0x1000:cand.addr - 0x1000 + cand.size]
    return [f"0x{a:x} {m} {o}".rstrip() for a, _, m, o in md.disasm_lite(code, cand.addr)]


class Worker:
    """Per-process state: the image, analysis and switch tables."""

    def __init__(self, binary: Path, analysis_path: Path, switches_path: Path, work: Path) -> None:
        self.analysis = json.loads(analysis_path.read_text())
        self.switches = json.loads(switches_path.read_text())["switch"]
        self.image = Image(binary, self.analysis)
        self.binary = binary
        self.work = work

    def _build(self, cand: Candidate, work: Path) -> Path:
        subset = [e for e in self.switches if e["func_addr"] == cand.addr]
        lifted = harness.lift_function(self.binary, self.analysis, cand.addr, cand.size, work,
                                       emit_inputs.reg_switches({"switch": subset}))
        source = harness.native_source(
            lifted, mem_size=MEM_SIZE, data_base=DATA_BASE, data_size=DATA_SIZE,
            sentinel=SENTINEL, code_base=cand.addr, includes=DISPATCH_STUBS,
            setup=self.image.native_setup())
        binary = work / "native"
        harness.compile_native(source, work, binary)
        return binary

    def run(self, cand: Candidate) -> dict[str, Any]:
        result: dict[str, Any] = {"addr": cand.addr, "size": cand.size, "runs": []}
        expected: dict[int, dict[str, object]] = {}
        for seed in SEEDS:
            try:
                expected[seed] = run_unicorn(self.image, cand.addr, make_state(cand.addr, seed))
            except Skipped:
                result["runs"].append({"seed": seed, "status": STATUS_SKIPPED})
        if not expected:
            result["status"] = STATUS_SKIPPED
            return result
        work = self.work / f"{cand.addr:08x}"
        work.mkdir(parents=True, exist_ok=True)
        try:
            binary = self._build(cand, work)
        except subprocess.SubprocessError as exc:
            return {**result, "status": STATUS_COMPILE_FAIL, "error": str(exc)[-600:]}
        except RuntimeError as exc:
            kind = STATUS_COMPILE_FAIL if "clang" in str(exc) else STATUS_LIFT_FAIL
            return {**result, "status": kind, "error": str(exc)[-600:]}
        except Exception as exc:  # the lifter raising is a lift failure to report, not to crash on
            return {**result, "status": STATUS_LIFT_FAIL, "error": f"{type(exc).__name__}: {exc}"}
        for seed, want in expected.items():
            result["runs"].append(self._compare(cand, seed, want, binary, work))
        statuses = {r["status"] for r in result["runs"]} - {STATUS_SKIPPED}
        if STATUS_MISMATCH in statuses:
            result["status"] = STATUS_MISMATCH
            result["disasm"] = disassemble(self.image, cand)
        elif STATUS_KNOWN_EBP in statuses:
            result["status"] = STATUS_KNOWN_EBP
        else:
            result["status"] = STATUS_PASSED
        return result

    def _compare(self, cand: Candidate, seed: int, want: dict[str, object], binary: Path,
                 work: Path) -> dict[str, Any]:
        state = make_state(cand.addr, seed)
        try:
            got = harness.run_native(binary, state, work, NATIVE_TIMEOUT)
        except subprocess.TimeoutExpired:
            return {"seed": seed, "status": STATUS_MISMATCH, "first": "native: timeout",
                    "culprit": None, "culprit_mnemonic": "native_timeout"}
        except RuntimeError as exc:
            return {"seed": seed, "status": STATUS_MISMATCH, "first": f"native: {str(exc)[:160]}",
                    "culprit": None, "culprit_mnemonic": "native_crash"}
        lines = harness.compare(_mask_dead_stack(want), _mask_dead_stack(got))
        if not lines:
            return {"seed": seed, "status": STATUS_PASSED}
        keys = tuple(line.split(" ", 1)[0].rstrip(":") for line in lines)
        if keys == EBP_ONLY:
            return {"seed": seed, "status": STATUS_KNOWN_EBP}
        lines = [ln for ln in lines if not ln.startswith("ebp:")]
        first = lines[0]
        key = first.split(" ", 1)[0].rstrip(":")
        offset = int(first.split("+0x")[1].split(":")[0], 16) if key == "data" else None
        found = Culprit(self.image, cand.addr, cand.size, state).find(key, offset)
        mnemonic = found[1].split(" ")[0] if found else "unknown"
        nan = _nan_suspect(first, want, got)
        return {"seed": seed, "status": STATUS_MISMATCH, "first": first, "nan_suspect": nan,
                "culprit": None if found is None else {"addr": found[0], "insn": found[1]},
                "culprit_mnemonic": f"nan:{mnemonic}" if nan else mnemonic}


_WORKER: Worker | None = None


def _init(binary: str, analysis: str, switches: str, work: str) -> None:
    global _WORKER
    _WORKER = Worker(Path(binary), Path(analysis), Path(switches), Path(work))


def _run_one(cand: Candidate) -> dict[str, Any]:
    assert _WORKER is not None
    return _WORKER.run(cand)


def summarize(results: list[dict[str, Any]], total_candidates: int, wall: float) -> dict[str, Any]:
    counts: dict[str, int] = defaultdict(int)
    run_counts: dict[str, int] = defaultdict(int)
    groups: dict[str, list[dict[str, Any]]] = defaultdict(list)
    mismatches = []
    for res in results:
        counts[res["status"]] += 1
        for run in res["runs"]:
            run_counts[run["status"]] += 1
        if res["status"] != STATUS_MISMATCH:
            continue
        bad = next(r for r in res["runs"] if r["status"] == STATUS_MISMATCH)
        entry = {"addr": f"0x{res['addr']:08x}", "size": res["size"], "seed": bad["seed"],
                 "first_diff": bad["first"], "culprit": bad["culprit"],
                 "group": bad["culprit_mnemonic"], "disasm": res["disasm"]}
        mismatches.append(entry)
        groups[entry["group"]].append(entry)
    skipped = counts[STATUS_SKIPPED]
    ordered = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    return {
        "candidates": total_candidates, "sampled": len(results), "wall_seconds": round(wall, 1),
        "tested": len(results) - skipped - counts[STATUS_LIFT_FAIL] - counts[STATUS_COMPILE_FAIL],
        "passed": counts[STATUS_PASSED], "mismatched": counts[STATUS_MISMATCH],
        "known_ebp_writeback": counts[STATUS_KNOWN_EBP], "skipped": skipped,
        "lift_fail": counts[STATUS_LIFT_FAIL], "compile_fail": counts[STATUS_COMPILE_FAIL],
        "runs": dict(run_counts),
        "groups": [{"mnemonic": name, "count": len(items),
                    "examples": [{k: e[k] for k in ("addr", "first_diff", "culprit")}
                                 for e in items[:3]]} for name, items in ordered],
        "lift_failures": [{"addr": f"0x{r['addr']:08x}", "status": r["status"], "error": r["error"]}
                          for r in results
                          if r["status"] in (STATUS_LIFT_FAIL, STATUS_COMPILE_FAIL)][:50],
        "mismatches": mismatches,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    parser.add_argument("--binary", type=Path, default=None)
    parser.add_argument("--work", type=Path, default=Path(tempfile.gettempdir()) / "hobbit-fuzz")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--limit", type=int, default=MAX_SAMPLE)
    args = parser.parse_args()
    binary = args.binary or default_binary()
    analysis_path, switches_path = ROOT / "recon" / "analysis.json", ROOT / "recon" / "switch_tables.json"
    start = time.time()
    analysis = json.loads(analysis_path.read_text())
    cands, total = select_candidates(analysis, binary.read_bytes(), json.loads(switches_path.read_text()),
                                     args.limit)
    print(f"{total} candidates, sampling {len(cands)}", flush=True)
    args.work.mkdir(parents=True, exist_ok=True)
    ctx = multiprocessing.get_context("spawn")
    results: list[dict[str, Any]] = []
    with ctx.Pool(args.workers, _init,
                  (str(binary), str(analysis_path), str(switches_path), str(args.work))) as pool:
        for i, res in enumerate(pool.imap_unordered(_run_one, cands, chunksize=4), 1):
            results.append(res)
            if i % 200 == 0:
                print(f"{i}/{len(cands)} {time.time() - start:.0f}s", flush=True)
    report = summarize(results, total, time.time() - start)
    (args.work / "report.json").write_text(json.dumps(report, indent=1))
    print({k: v for k, v in report.items() if k not in ("groups", "mismatches", "lift_failures")})


if __name__ == "__main__":
    main()
