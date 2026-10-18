"""Generate runtime/src/thunk_auto.c: i386-to-host thunks for the simple imports.

Usage: emit_thunks_auto.py [import_sigs.json [thunk_auto.c]]
Defaults are recon/import_sigs.json and runtime/src/thunk_auto.c under the repo root.

An import is auto-generated when its signature uses only scalars and plain
guest pointers. Imports that need guest callbacks, handles or host pointers
are listed in a "deferred" comment block instead.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]

PARAM_KINDS = {"i32", "u32", "i64", "u64", "f32", "f64", "ptr"}
RET_KINDS = {"void", "i32", "u32", "i64", "u64", "f32", "f64"}
# Pointer-returning functions whose result points into one of their arguments.
RET_INTO_ARG = {
    "_memcpy", "_memmove", "_memset", "_memchr", "_strcpy", "_strncpy",
    "_strcat", "_strchr", "_strrchr", "_strstr", "_strtok", "_strtok_r",
}
DYLIBS = ("/libSystem.B.dylib", "/OpenGL")
# Other slices own these names (pthread, heap, stdio, printf, time, ...).
EXCLUDED_SUBSTRINGS = (
    "pthread", "OSAtomic", "alloc", "free", "strdup", "printf", "scanf",
    "fcntl", "fopen", "fclose", "fread", "fwrite", "fseek", "ftell", "dir",
    "stat", "mkdir", "chdir", "error", "time", "pwuid", "hostbyname",
    "dlopen", "dlsym", "getenv", "std",
)
SOCKET_CALLS = {
    "socket", "connect", "send", "recv", "sendto", "recvfrom", "setsockopt",
    "getsockopt", "shutdown", "bind", "listen", "accept", "select", "poll",
    "getaddrinfo", "getpeername", "getsockname",
}
# Written by hand at the end of thunk_auto.c; the generator does not emit them.
HAND_WRITTEN = {
    "_strtok_r": "char ** slot is a 4-byte guest slot",
    "_task_policy_set": "no-op returning KERN_SUCCESS",
}
# Headers beyond the default set, only for names the default set does not declare.
EXTRA_HEADERS = {
    "_inet_addr": "arpa/inet.h",
    "_remove": "stdio.h",
    "_rename": "stdio.h",
    "_sched_get_priority_max": "sched.h",
    "_sched_yield": "sched.h",
    "_setpriority": "sys/resource.h",
}
DEFAULT_HEADERS: tuple[str, ...] = (
    "string.h", "stdlib.h", "math.h", "unistd.h", "mach/mach_time.h",
    "ctype.h", "wchar.h", "OpenGL/gl.h", "OpenGL/glext.h",
)

Sig = dict[str, Any]

THUNK_DEF = re.compile(r"^void\s+(imp_\w+)\s*\(\s*void\s*\)\s*\{", re.M)


def defined_elsewhere(src_dir: Path) -> set[str]:
    """imp_ functions that other runtime/src/thunk_*.c files define."""
    names: set[str] = set()
    for path in src_dir.glob("thunk_*.c"):
        if path.name != "thunk_auto.c":
            names.update(THUNK_DEF.findall(path.read_text(encoding="utf-8")))
    return names


def base_name(name: str) -> str:
    """Import name without the Mach-O prefix underscore and $UNIX2003 suffix."""
    out = name[1:] if name.startswith("_") else name
    return out.split("$", 1)[0]


def passes_base_rules(sig: Sig) -> bool:
    name = sig["name"]
    if sig["status"] != "resolved" or sig["variadic"]:
        return False
    if any(p["kind"] not in PARAM_KINDS for p in sig["params"]):
        return False
    if sig["ret"]["kind"] not in RET_KINDS and name not in RET_INTO_ARG:
        return False
    if not sig["dylib"].endswith(DYLIBS):
        return False
    return not any(s in name for s in EXCLUDED_SUBSTRINGS)


def deferral_reason(sig: Sig) -> str | None:
    """Why an in-scope import is not auto-generated, or None if it is."""
    name = sig["name"]
    if name.startswith("_CGL"):
        return "GL context work (M4) owns CGL"
    if base_name(name) in SOCKET_CALLS:
        return "socket call; the game runs offline"
    for p in sig["params"]:
        ctype = p["type"]
        if "(" in ctype:
            return "takes a guest function pointer (guest callbacks come later)"
        if "FILE" in ctype or "DIR" in ctype:
            return "FILE/DIR parameter needs T3 handles"
        if p["kind"] == "ptr" and ctype.count("*") >= 2:
            return "pointer-to-pointer parameter"
        if p["kind"] == "ptr" and "*" not in ctype:
            return f"opaque handle type {ctype}"
    return None


def c_ident(name: str) -> str:
    return "imp_" + name.replace("$", "_")


def arg_decl(i: int, param: dict[str, Any], off: int) -> str:
    kind, size = param["kind"], param["size"]
    if kind == "ptr":
        return f"void *a{i} = gp(arg_u32({off}));"
    if kind == "f32":
        return f"float a{i} = arg_f32({off});"
    if kind == "f64":
        return f"double a{i} = arg_f64({off});"
    if kind in ("i64", "u64"):
        ctype = "int64_t" if kind == "i64" else "uint64_t"
        return f"{ctype} a{i} = ({ctype})arg_u64({off});"
    signed = kind == "i32"
    narrow = {1: "int8_t" if signed else "uint8_t", 2: "int16_t" if signed else "uint16_t"}
    if size in narrow:
        return f"{'int32_t' if signed else 'uint32_t'} a{i} = ({narrow[size]})arg_u32({off});"
    return f"{'int32_t' if signed else 'uint32_t'} a{i} = ({'int32_t' if signed else 'uint32_t'})arg_u32({off});"


def emit_thunk(sig: Sig) -> str:
    name = sig["name"]
    host = base_name(name)
    lines = [f"void {c_ident(name)}(void)", "{"]
    off = 4
    for i, p in enumerate(sig["params"]):
        lines.append("    " + arg_decl(i, p, off))
        off += p["stack_size"]
    call = f"{host}({', '.join(f'a{i}' for i in range(len(sig['params'])))})"
    rk = sig["ret"]["kind"]
    if name in RET_INTO_ARG:
        lines.append(f"    g_eax = host_to_guest((void *){call});")
    elif rk == "void":
        lines.append(f"    {call};")
    elif rk in ("f32", "f64"):
        lines.append(f"    fp_push({call});")
    elif rk in ("i64", "u64"):
        lines.append(f"    uint64_t r = (uint64_t){call};")
        lines.append("    g_eax = (uint32_t)r;")
        lines.append("    g_edx = (uint32_t)(r >> 32);")
    else:
        lines.append(f"    g_eax = (uint32_t){call};")
    lines.append("    g_esp += 4;")
    lines.append("}")
    return "\n".join(lines)


PREAMBLE = r'''
#define GL_SILENCE_DEPRECATION
%(includes)s
#include "recomp_types.h"
#include "guest_mem.h"

/* The generated chunks define fp_push as a macro in each function; this is the
 * same operation: i386 returns floating-point results in st(0). */
static inline void fp_push(double v)
{
    g_fp_top = (g_fp_top + 7u) & 7u;
    g_fp_stack[g_fp_top] = v;
}

static inline uint32_t arg_u32(uint32_t off) { return MEM32(g_esp + off); }

static inline uint64_t arg_u64(uint32_t off)
{
    return (uint64_t)arg_u32(off) | ((uint64_t)arg_u32(off + 4) << 32);
}

static inline float arg_f32(uint32_t off)
{
    uint32_t u = arg_u32(off);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static inline double arg_f64(uint32_t off)
{
    uint64_t u = arg_u64(off);
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

/* Guest address to host pointer; guest 0 is NULL. */
static inline void *gp(uint32_t a) { return a ? guest_ptr(a) : NULL; }

/* Host pointer into the guest window to guest address; NULL is 0. */
static inline uint32_t host_to_guest(void *p)
{
    return p ? (uint32_t)((uint8_t *)p - g_guest_base) : 0;
}
'''

HAND_WRITTEN_C = r'''
/* ---- hand-written ---- */

/* char *strtok_r(char *s, const char *delim, char **save): the guest passes
 * the address of a 4-byte slot that holds a guest pointer. */
void imp__strtok_r(void)
{
    char *s = gp(arg_u32(4));
    const char *delim = gp(arg_u32(8));
    uint32_t slot = arg_u32(12);
    char *save = gp(MEM32(slot));
    char *r = strtok_r(s, delim, &save);
    MEM32(slot) = host_to_guest(save);
    g_eax = host_to_guest(r);
    g_esp += 4;
}

/* kern_return_t task_policy_set(...): scheduling hint, nothing to do. */
void imp__task_policy_set(void)
{
    g_eax = 0;
    g_esp += 4;
}
'''


def wrap_names(names: list[str], indent: str = " * ") -> list[str]:
    lines: list[str] = []
    cur = ""
    for n in names:
        if cur and len(cur) + len(n) + 1 > 76:
            lines.append(indent + cur)
            cur = n
        else:
            cur = f"{cur} {n}".strip()
    if cur:
        lines.append(indent + cur)
    return lines


def generate(sigs: list[Sig], others: set[str]) -> str:
    selected: list[Sig] = []
    deferred: list[tuple[str, str]] = []
    skipped: list[str] = []
    for sig in sorted(sigs, key=lambda s: s["name"]):
        if not passes_base_rules(sig):
            continue
        if sig["name"] in HAND_WRITTEN:
            continue
        if c_ident(sig["name"]) in others:
            skipped.append(sig["name"])
            continue
        reason = deferral_reason(sig)
        if reason:
            deferred.append((sig["name"], reason))
        else:
            selected.append(sig)

    headers: list[str] = list(DEFAULT_HEADERS)
    for sig in selected:
        extra = EXTRA_HEADERS.get(sig["name"])
        if extra and extra not in headers:
            headers.append(extra)
    includes = "\n".join(f"#include <{h}>" for h in headers)

    out = ["/* Generated by tools/hobbit/emit_thunks_auto.py. Do not edit.", " *",
           f" * Auto-generated thunks ({len(selected)}):"]
    out += wrap_names([s["name"] for s in selected])
    out += [" *", f" * Hand-written below ({len(HAND_WRITTEN)}):"]
    out += [f" *   {n}: {why}" for n, why in HAND_WRITTEN.items()]
    out += [" *", f" * Already defined in another thunk_*.c, skipped ({len(skipped)}):"]
    out += wrap_names(skipped)
    out += [" *", f" * Deferred, not generated ({len(deferred)}):"]
    out += [f" *   {n}: {why}" for n, why in deferred]
    out.append(" */")
    out.append(PREAMBLE.replace("%(includes)s", includes).rstrip("\n"))
    out.append("")
    out.append("/* ---- generated ---- */")
    out.append("")
    out.append("\n\n".join(emit_thunk(s) for s in selected))
    out.append(HAND_WRITTEN_C.rstrip("\n"))
    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    sigs_path = Path(argv[1]) if len(argv) > 1 else ROOT / "recon" / "import_sigs.json"
    out_path = Path(argv[2]) if len(argv) > 2 else ROOT / "runtime" / "src" / "thunk_auto.c"
    sigs = json.loads(sigs_path.read_text(encoding="utf-8"))["imports"]
    text = generate(sigs, defined_elsewhere(out_path.parent))
    out_path.write_text(text, encoding="utf-8")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
