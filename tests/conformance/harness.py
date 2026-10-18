"""Differential harness: one x86 snippet run in Unicorn and as lifted C (arm64).

Unicorn is only the oracle. Both sides share one layout: code at CODE_BASE, a
random-filled data page at DATA_BASE, stack top at STACK_TOP. EFLAGS are not
compared because the lifter does not materialise them.
"""

from __future__ import annotations

import hashlib
import json
import random
import struct
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from unicorn import UC_ARCH_X86, UC_MODE_32
from unicorn.unicorn import Uc
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX,
    UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_ESP,
    UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3,
    UC_X86_REG_XMM4, UC_X86_REG_XMM5, UC_X86_REG_XMM6, UC_X86_REG_XMM7,
)

from tools.xrecomp import config
from tools.xrecomp.translator import BatchTranslator

ROOT = Path(__file__).resolve().parents[2]
INCLUDE_DIR = ROOT / "tools" / "xrecomp" / "include"
WORK_DIR = Path(tempfile.gettempdir()) / "hobbit-conformance"

CODE_BASE = 0x00010000
CODE_PAGE = 0x1000
SENTINEL = CODE_BASE + 0xF00
DATA_BASE = 0x00300000
DATA_SIZE = 0x10000
STACK_TOP = 0x00200000 - 0x100
STACK_PAGES = 0x10000
MEM_SIZE = DATA_BASE + DATA_SIZE

GPRS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
COMPARED_GPRS = tuple(r for r in GPRS if r != "esp")
XMM_COUNT = 8
_UC_GPRS = (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
            UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI)
_UC_XMM = (UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3,
           UC_X86_REG_XMM4, UC_X86_REG_XMM5, UC_X86_REG_XMM6, UC_X86_REG_XMM7)

Xmm = tuple[int, int, int, int]


@dataclass(frozen=True)
class State:
    gpr: dict[str, int]
    xmm: tuple[Xmm, ...]
    data: bytes


@dataclass(frozen=True)
class Case:
    name: str
    asm: str
    regs: dict[str, int] = field(default_factory=dict)
    xmm: dict[int, Xmm] = field(default_factory=dict)
    xfail: str | None = None


def _random_xmm(rng: random.Random) -> Xmm:
    return (rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32))


def initial_state(case: Case, seed: int) -> State:
    rng = random.Random(f"{case.name}:{seed}")
    gpr = {r: rng.getrandbits(32) for r in GPRS}
    gpr.update(case.regs)
    gpr["esp"] = STACK_TOP
    xmm = tuple(case.xmm.get(i) or _random_xmm(rng) for i in range(XMM_COUNT))
    return State(gpr, xmm, rng.randbytes(DATA_SIZE))


def _case_dir(name: str) -> Path:
    path = WORK_DIR / name
    path.mkdir(parents=True, exist_ok=True)
    return path


def _run(cmd: list[str], timeout: float | None = None) -> str:
    """Run a command and return stdout; a failure raises with the command's stderr."""
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if proc.returncode:
        raise RuntimeError(f"{cmd[0]} exited {proc.returncode}: {proc.stderr[-1500:]}")
    return proc.stdout


def assemble(case: Case) -> bytes:
    """Assemble with clang and return the __text bytes of the MH_OBJECT."""
    work = _case_dir(case.name)
    src, obj = work / "x.s", work / "x.o"
    src.write_text(f".intel_syntax noprefix\n{case.asm}\n")
    _run(["clang", "-target", "i386-apple-macosx10.9", "-c", str(src), "-o", str(obj)])
    return _text_section(obj.read_bytes())


def _text_section(obj: bytes) -> bytes:
    """__text of the first LC_SEGMENT of a 32-bit MH_OBJECT (no relocation support)."""
    ncmds = struct.unpack_from("<I", obj, 16)[0]
    off = 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<2I", obj, off)
        if cmd == 0x1:
            nsects = struct.unpack_from("<I", obj, off + 48)[0]
            for i in range(nsects):
                sec = off + 56 + 68 * i
                name = obj[sec:sec + 16].rstrip(b"\0")
                sz, offset = struct.unpack_from("<2I", obj, sec + 36)
                if name == b"__text":
                    return obj[offset:offset + sz]
        off += size
    raise ValueError("no __text section")


def _lanes(value: int) -> Xmm:
    return ((value >> 0) & 0xFFFFFFFF, (value >> 32) & 0xFFFFFFFF,
            (value >> 64) & 0xFFFFFFFF, (value >> 96) & 0xFFFFFFFF)


def run_unicorn(code: bytes, state: State) -> dict[str, object]:
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(CODE_BASE, CODE_PAGE)
    uc.mem_write(CODE_BASE, code)
    uc.mem_write(SENTINEL, b"\x90")
    uc.mem_map(STACK_TOP + 0x100 - STACK_PAGES, STACK_PAGES)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    uc.mem_write(DATA_BASE, state.data)
    for reg, name in zip(_UC_GPRS, GPRS):
        uc.reg_write(reg, state.gpr[name])
    for reg, lanes in zip(_UC_XMM, state.xmm):
        uc.reg_write(reg, sum(v << (32 * i) for i, v in enumerate(lanes)))
    esp = state.gpr["esp"] - 4
    uc.mem_write(esp, struct.pack("<I", SENTINEL))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.emu_start(CODE_BASE, SENTINEL)
    gpr = {name: int(uc.reg_read(reg)) for reg, name in zip(_UC_GPRS, GPRS)}
    xmm = [_lanes(int(uc.reg_read(reg))) for reg in _UC_XMM]
    return _result(gpr, xmm, bytes(uc.mem_read(DATA_BASE, DATA_SIZE)), state)


def _result(gpr: dict[str, int], xmm: list[Xmm], data: bytes, start: State) -> dict[str, object]:
    out: dict[str, object] = {r: gpr[r] for r in COMPARED_GPRS}
    out["esp_delta"] = (gpr["esp"] - start.gpr["esp"]) & 0xFFFFFFFF
    for i, lanes in enumerate(xmm):
        out[f"xmm{i}"] = list(lanes)
    out["data"] = data.hex()
    return out


def lift(code: bytes, work: Path) -> str:
    """Lift the snippet as sub_00010000 through the same API the game build uses."""
    image = work / "image.bin"
    image.write_bytes(code)
    analysis = {"segments": [{"name": "__TEXT", "sections": [
        {"name": "__text", "addr": CODE_BASE, "size": len(code), "offset": 0, "flags": 0}]}]}
    return lift_function(image, analysis, CODE_BASE, len(code), work, {})


def lift_function(image: Path, analysis: dict, addr: int, size: int, work: Path,
                  reg_switches: dict[str, list[str]]) -> str:
    """Lift one function of `image` (laid out as `analysis` says) as sub_<addr>."""
    config.configure_from_macho(analysis, image)
    (work / "functions.json").write_text(json.dumps([{
        "start": f"0x{addr:08X}", "end": f"0x{addr + size:08X}", "name": f"sub_{addr:08X}"}]))
    (work / "labels.json").write_text("[]")
    (work / "reg_switches.json").write_text(json.dumps(reg_switches))
    bt = BatchTranslator(
        str(image), str(work / "functions.json"), labels_json_path=str(work / "labels.json"),
        output_dir=str(work), image_format="macho",
        reg_switch_json_path=str(work / "reg_switches.json"), import_stubs={})
    c = bt.translate_single(addr)
    if not c:
        raise RuntimeError(f"lifter produced no code: {bt.failures}")
    return c


_MAIN = r"""
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
%(includes)s
#define XMM_N 8
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompThreadState g_thread_state = {.ts_mxcsr = 0x1F80};

void recomp_divide_error(uint32_t va) {
    fprintf(stderr, "divide error at 0x%%08X\n", va);
    abort();
}

%(lifted)s

static RecompXmm *xmm_reg(int i) {
    RecompXmm *r[XMM_N] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    return r[i];
}

int main(int argc, char **argv) {
    (void)argc;
    uint8_t *host = calloc(1, %(mem_size)s);
%(setup)s
    g_xbox_mem_offset = (ptrdiff_t)host;
    FILE *f = fopen(argv[1], "rb");
    uint32_t gpr[8], xmm[XMM_N * 4];
    if (fread(gpr, 4, 8, f) != 8 || fread(xmm, 4, XMM_N * 4, f) != XMM_N * 4 ||
        fread(host + %(data_base)s, 1, %(data_size)s, f) != %(data_size)s) return 2;
    fclose(f);
    g_eax = gpr[0]; g_ecx = gpr[1]; g_edx = gpr[2]; g_ebx = gpr[3];
    g_ebp = g_seh_ebp = gpr[5]; g_esi = gpr[6]; g_edi = gpr[7];
    for (int i = 0; i < XMM_N; i++) memcpy(xmm_reg(i)->u, xmm + 4 * i, 16);
    g_esp = gpr[4];
    PUSH32(g_esp, %(sentinel)s);
    sub_%(code_base)s();
    printf("{\"eax\":%%u,\"ecx\":%%u,\"edx\":%%u,\"ebx\":%%u,\"ebp\":%%u,\"esi\":%%u,\"edi\":%%u,"
           "\"esp_delta\":%%u", g_eax, g_ecx, g_edx, g_ebx, g_ebp, g_esi, g_edi, g_esp - gpr[4]);
    for (int i = 0; i < XMM_N; i++) {
        const uint32_t *u = xmm_reg(i)->u;
        printf(",\"xmm%%d\":[%%u,%%u,%%u,%%u]", i, u[0], u[1], u[2], u[3]);
    }
    printf(",\"data\":\"");
    for (int i = 0; i < %(data_size)s; i++) printf("%%02x", host[%(data_base)s + i]);
    printf("\"}\n");
    return 0;
}
"""


def native_source(lifted: str, *, mem_size: int = MEM_SIZE, data_base: int = DATA_BASE,
                  data_size: int = DATA_SIZE, sentinel: int = SENTINEL, code_base: int = CODE_BASE,
                  includes: str = "", setup: str = "") -> str:
    """The C program around one lifted function. `includes` goes at file scope and
    `setup` runs in main right after the guest memory block `host` is allocated."""
    return _MAIN % {
        "lifted": lifted, "mem_size": hex(mem_size), "data_base": hex(data_base),
        "data_size": hex(data_size), "sentinel": hex(sentinel), "code_base": f"{code_base:08X}",
        "includes": includes, "setup": setup}


def compile_native(source: str, work: Path, binary: Path) -> None:
    """Compile `source` for arm64 into `binary` (written atomically)."""
    c_file = work / "case.c"
    c_file.write_text(source)
    tmp = binary.with_suffix(".tmp")
    _run(["clang", "-std=gnu11", "-O0", "-arch", "arm64", "-w", "-I", str(INCLUDE_DIR),
          str(c_file), "-o", str(tmp), "-lm"])
    tmp.rename(binary)


def build_native(case: Case, code: bytes) -> Path:
    """Lift and compile the case for arm64; binaries are cached by source hash."""
    work = _case_dir(case.name)
    source = native_source(lift(code, work))
    digest = hashlib.sha256(source.encode()).hexdigest()[:16]
    binary = WORK_DIR / f"{case.name}-{digest}"
    if not binary.exists():
        compile_native(source, work, binary)
    return binary


def run_native(binary: Path, state: State, work: Path,
               timeout: float | None = None) -> dict[str, object]:
    blob = work / "state.bin"
    blob.write_bytes(
        struct.pack("<8I", *(state.gpr[r] for r in GPRS))
        + b"".join(struct.pack("<4I", *lanes) for lanes in state.xmm) + state.data)
    return json.loads(_run([str(binary), str(blob)], timeout))


def compare(expected: dict[str, object], actual: dict[str, object]) -> list[str]:
    """One line per differing field (empty means conformant)."""
    lines: list[str] = []
    for key, want in expected.items():
        got = actual[key]
        if got == want:
            continue
        if isinstance(want, str) and isinstance(got, str):
            first = next(i for i in range(0, len(want), 2) if want[i:i + 2] != got[i:i + 2]) // 2
            lines.append(f"{key} differs at +0x{first:X}: want {want[first * 2:first * 2 + 8]} "
                         f"got {got[first * 2:first * 2 + 8]}")
        else:
            lines.append(f"{key}: want {want} got {got}")
    return lines


def diff(case: Case, seed: int) -> list[str]:
    """Run both sides; return one line per differing field (empty means conformant)."""
    state = initial_state(case, seed)
    code = assemble(case)
    expected = run_unicorn(code, state)
    actual = run_native(build_native(case, code), state, _case_dir(case.name))
    return compare(expected, actual)
