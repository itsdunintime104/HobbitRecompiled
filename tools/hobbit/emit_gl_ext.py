"""Generate runtime/src/thunk_gl_ext.c: thunks for every GL name the game looks up
with dlsym that the host OpenGL framework exports.

Usage: emit_gl_ext.py [game_binary [thunk_gl_ext.c]]
Defaults are orig.nosync/hobbit_i386 in the main repo and runtime/src/thunk_gl_ext.c
under the repo root.

Steps:
 1. Candidate names are the gl* strings in the game binary (`strings -a`).
 2. A candidate stays when the host OpenGL framework exports it. `nm -gU` is
    tried first; the framework lives in the dyld shared cache on current macOS
    and has no file for nm to read, so a small C probe calls dlsym instead.
 3. Names that runtime/src already defines (imp__gl* thunks, the curated table
    in thunk_gl.c) are skipped.
 4. Prototypes come from `clang -Xclang -ast-dump=json` over <OpenGL/gl.h> and
    <OpenGL/glext.h>. Pointer-to-pointer, pointer-returning and opaque-handle
    (GLhandleARB, GLsync) prototypes are skipped and listed in a comment.
 5. Each kept name gets a thunk with the T1 argument conventions of
    emit_thunks_auto.py, and a sorted name
    table behind gl_ext_auto_lookup().
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
GAME_BIN = ROOT / ".." / ".." / ".." / "orig.nosync" / "hobbit_i386"
GL_FRAMEWORK = Path("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL")
GL_FRAMEWORK_DLOPEN = "/System/Library/Frameworks/OpenGL.framework/OpenGL"
OUT_NAME = "thunk_gl_ext.c"

GL_NAME = re.compile(r"gl[A-Z][A-Za-z0-9]*")
IMP_DEF = re.compile(r"^void\s+imp__(gl\w+)\s*\(\s*void\s*\)\s*\{?", re.M)
CURATED = re.compile(r"\b(E|E_ARB|E_EXT)\((\w+)\)")
QUALIFIERS = re.compile(r"\b(const|volatile|restrict|__restrict|struct|enum|union)\b")

PROBE_C = r"""
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    void *h = dlopen(argv[1], RTLD_LAZY);
    char name[256];
    if (h == NULL)
        return 2;
    while (fgets(name, sizeof name, stdin) != NULL) {
        name[strcspn(name, "\n")] = 0;
        if (dlsym(h, name) != NULL)
            puts(name);
    }
    return 0;
}
"""

PROTO_SOURCE = """\
#define GL_GLEXT_PROTOTYPES
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
"""

# Scalar C types (after typedef resolution) -> (kind, guest stack bytes).
# long and unsigned long are 8 bytes on the arm64 host but 4 in the guest
# (GLintptr, GLsizeiptr), so they are 4-byte guest arguments.
SCALARS: dict[str, tuple[str, int]] = {
    "float": ("f32", 4),
    "double": ("f64", 8),
    "long long": ("i64", 8),
    "unsigned long long": ("u64", 8),
    "long": ("iptr", 4),
    "unsigned long": ("uptr", 4),
    "int": ("i32", 4),
    "unsigned int": ("u32", 4),
    "short": ("i16", 4),
    "unsigned short": ("u16", 4),
    "char": ("i8", 4),
    "signed char": ("i8", 4),
    "unsigned char": ("u8", 4),
    "_Bool": ("u8", 4),
}
C_TYPES = {
    "i32": "int32_t", "u32": "uint32_t", "i16": "int16_t", "u16": "uint16_t",
    "i8": "int8_t", "u8": "uint8_t", "iptr": "intptr_t", "uptr": "uintptr_t",
}

Param = dict[str, Any]

# Pointer arguments that are byte offsets while a buffer is bound to the slot's
# target (see gl_buf_ptr in thunk_gl.c): {GL name: {argument index: slot}}.
OFFSET_ARGS: dict[str, dict[int, str]] = {
    "glDrawElementsBaseVertex": {3: "GL_BIND_ELEMENT"},
    "glDrawRangeElementsBaseVertex": {5: "GL_BIND_ELEMENT"},
    "glDrawElementsInstancedARB": {3: "GL_BIND_ELEMENT"},
    "glDrawElementsInstancedBaseVertex": {3: "GL_BIND_ELEMENT"},
    "glVertexAttribIPointerEXT": {4: "GL_BIND_ARRAY"},
    "glSecondaryColorPointer": {3: "GL_BIND_ARRAY"},
    "glSecondaryColorPointerEXT": {3: "GL_BIND_ARRAY"},
    "glFogCoordPointer": {2: "GL_BIND_ARRAY"},
    "glFogCoordPointerEXT": {2: "GL_BIND_ARRAY"},
    "glWeightPointerARB": {3: "GL_BIND_ARRAY"},
}


class Skip(Exception):
    """The prototype cannot be an auto thunk; the message is the reason."""


# ---- inputs ---------------------------------------------------------------


def game_names(binary: Path) -> list[str]:
    out = subprocess.run(["strings", "-a", str(binary)], check=True, capture_output=True).stdout.decode("latin-1")
    return sorted({s for s in out.splitlines() if GL_NAME.fullmatch(s)})


def host_exports(names: list[str]) -> set[str]:
    """Names the host OpenGL framework exports."""
    if GL_FRAMEWORK.is_file():
        res = subprocess.run(["nm", "-gU", str(GL_FRAMEWORK)], capture_output=True, text=True)
        if res.returncode == 0:
            syms = {line.split()[-1] for line in res.stdout.splitlines() if line.strip()}
            return {n for n in names if "_" + n in syms}
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "probe.c", Path(tmp) / "probe"
        src.write_text(PROBE_C, encoding="utf-8")
        subprocess.run(["clang", "-o", str(exe), str(src)], check=True)
        res = subprocess.run([str(exe), GL_FRAMEWORK_DLOPEN], input="\n".join(names) + "\n",
                             check=True, capture_output=True, text=True)
    return set(res.stdout.split())


def defined_elsewhere(src_dir: Path, own: str) -> set[str]:
    """GL names that other runtime/src files define or serve through the curated table."""
    names: set[str] = set()
    for path in sorted(src_dir.glob("*.c")):
        if path.name == own:
            continue
        text = path.read_text(encoding="utf-8")
        names.update(IMP_DEF.findall(text))
        if path.name == "thunk_gl.c":
            for kind, ident in CURATED.findall(text):
                names.add("gl" + ident)
                if kind == "E_ARB":
                    names.add("gl" + ident + "ARB")
                elif kind == "E_EXT":
                    names.add("gl" + ident + "EXT")
    return names


def header_ast() -> dict[str, Any]:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "gl_headers.c"
        src.write_text(PROTO_SOURCE, encoding="utf-8")
        res = subprocess.run(
            ["clang", "-Xclang", "-ast-dump=json", "-fsyntax-only", "-arch", "arm64", str(src)],
            check=True, capture_output=True, text=True,
        )
    return json.loads(res.stdout)


def prototypes(ast: dict[str, Any]) -> tuple[dict[str, tuple[str, list[str]]], dict[str, str]]:
    """({function name: (return type, [param types])}, {typedef name: underlying type})."""
    protos: dict[str, tuple[str, list[str]]] = {}
    typedefs: dict[str, str] = {}
    for node in ast["inner"]:
        kind, name = node.get("kind"), node.get("name", "")
        if kind == "TypedefDecl":
            typedefs[name] = node["type"]["qualType"]
        elif kind == "FunctionDecl" and name.startswith("gl") and name not in protos:
            ftype = node["type"]["qualType"]
            ret = ftype.split("(", 1)[0].strip()
            params = [p["type"]["qualType"] for p in node.get("inner", []) if p.get("kind") == "ParmVarDecl"]
            if ftype.count("(") != 1 or ftype.endswith("...)"):
                params = ["<unsupported>"]
            protos[name] = (ret, params)
    return protos, typedefs


# ---- type classification --------------------------------------------------


def classify(ctype: str, typedefs: dict[str, str], is_return: bool) -> Param:
    """Kind and guest stack size of one prototype type, or Skip."""
    own = ctype.count("*")
    base = " ".join(QUALIFIERS.sub(" ", ctype.replace("*", " ")).split())
    added = 0
    seen: set[str] = set()
    while base in typedefs and base not in seen:
        seen.add(base)
        under = typedefs[base]
        added += under.count("*")
        base = " ".join(QUALIFIERS.sub(" ", under.replace("*", " ")).split())
    if "(" in ctype or base == "<unsupported>":
        raise Skip("function-pointer or variadic prototype")
    if own + added == 0:
        if base == "void" and is_return:
            return {"kind": "void", "size": 0, "ctype": ctype}
        if base not in SCALARS:
            raise Skip("unsupported scalar type")
        kind, size = SCALARS[base]
        return {"kind": kind, "size": size, "ctype": ctype}
    if is_return:
        raise Skip("returns a pointer")
    if own == 0:
        raise Skip("opaque handle type (GLhandleARB, GLsync)")
    if own + added > 1:
        raise Skip("pointer-to-pointer parameter")
    return {"kind": "ptr", "size": 4, "ctype": ctype}


def build_sig(name: str, proto: tuple[str, list[str]], typedefs: dict[str, str]) -> dict[str, Any]:
    ret = classify(proto[0], typedefs, True)
    params = [classify(t, typedefs, False) for t in proto[1]]
    return {"name": name, "ret": ret, "params": params}


# ---- emission -------------------------------------------------------------


def arg_decl(i: int, p: Param, off: int, slot: str | None = None) -> str:
    kind = p["kind"]
    if kind == "ptr" and slot is not None:
        value = f"gl_buf_ptr({slot}, arg_u32({off}))"
        if "const" in p["ctype"]:
            return f"const void *a{i} = {value};"
        return f"void *a{i} = (void *){value};"
    if kind == "ptr":
        return f"void *a{i} = gp(arg_u32({off}));"
    if kind == "f32":
        return f"float a{i} = arg_f32({off});"
    if kind == "f64":
        return f"double a{i} = arg_f64({off});"
    if kind in ("i64", "u64"):
        ctype = "int64_t" if kind == "i64" else "uint64_t"
        return f"{ctype} a{i} = ({ctype})arg_u64({off});"
    if kind == "iptr":
        return f"intptr_t a{i} = (intptr_t)(int32_t)arg_u32({off});"
    if kind == "uptr":
        return f"uintptr_t a{i} = (uintptr_t)arg_u32({off});"
    return f"{C_TYPES[kind]} a{i} = ({C_TYPES[kind]})arg_u32({off});"


def emit_thunk(sig: dict[str, Any]) -> str:
    name = sig["name"]
    lines = [f"static void x_{name}(void)", "{"]
    off = 4
    slots = OFFSET_ARGS.get(name, {})
    for i, p in enumerate(sig["params"]):
        if i in slots and p["kind"] != "ptr":
            raise ValueError(f"{name} argument {i} is not a pointer")
        lines.append("    " + arg_decl(i, p, off, slots.get(i)))
        off += p["size"]
    call = f"{name}({', '.join(f'a{i}' for i in range(len(sig['params'])))})"
    if name.startswith(("glDraw", "glMultiDraw")) and not name.startswith("glDrawBuffer"):
        lines.append("    hud_count_draw();")
    rk = sig["ret"]["kind"]
    if rk == "void":
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
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "gl_bind.h"
#include "guest_mem.h"
#include "hud_counters.h"
#include "thunk_gl.h"

/* i386 returns floating-point results in st(0). */
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
'''

LOOKUP_C = r'''
static int cmp_entry(const void *key, const void *elem)
{
    return strcmp((const char *)key, ((const struct gl_auto_entry *)elem)->name);
}

recomp_func_t gl_ext_auto_lookup(const char *name)
{
    const struct gl_auto_entry *e =
        bsearch(name, g_auto_table, sizeof g_auto_table / sizeof g_auto_table[0],
                sizeof g_auto_table[0], cmp_entry);
    return e != NULL ? e->fn : NULL;
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


def generate(candidates: list[str], exported: set[str], others: set[str],
             protos: dict[str, tuple[str, list[str]]], typedefs: dict[str, str]) -> tuple[str, dict[str, Any]]:
    kept: list[dict[str, Any]] = []
    defined: list[str] = []
    no_proto: list[str] = []
    skipped: dict[str, list[str]] = {}
    for name in sorted(candidates):
        if name not in exported:
            continue
        if name in others:
            defined.append(name)
        elif name not in protos:
            no_proto.append(name)
        else:
            try:
                kept.append(build_sig(name, protos[name], typedefs))
            except Skip as why:
                skipped.setdefault(str(why), []).append(name)

    stats = {
        "candidates": len(candidates), "exported": len(exported), "kept": len(kept),
        "defined": len(defined), "no_proto": len(no_proto),
        "skipped": {k: len(v) for k, v in sorted(skipped.items())},
    }
    out = ["/* Generated by tools/hobbit/emit_gl_ext.py. Do not edit.", " *",
           " * Thunks for the GL names the game looks up with dlsym that the host OpenGL",
           " * framework exports. gl_ext_lookup() in thunk_gl.c falls back to",
           " * gl_ext_auto_lookup() when its curated table has no entry.",
           " *",
           f" * Game candidates: {stats['candidates']}; exported by the host: {stats['exported']};"
           f" thunks emitted: {stats['kept']}.",
           " *", f" * Emitted ({len(kept)}):"]
    out += wrap_names([s["name"] for s in kept])
    out += [" *", f" * Already defined in another runtime/src file ({len(defined)}):"]
    out += wrap_names(defined)
    out += [" *", f" * No prototype in the SDK headers ({len(no_proto)}):"]
    out += wrap_names(no_proto)
    for reason, names in sorted(skipped.items()):
        out += [" *", f" * Skipped, {reason} ({len(names)}); needs a hand-written shadow or handle map:"]
        out += wrap_names(names)
    out.append(" */")
    out.append(PREAMBLE.rstrip("\n"))
    out += ["", "/* ---- generated ---- */", ""]
    out.append("\n\n".join(emit_thunk(s) for s in kept))
    out += ["", "struct gl_auto_entry {", "    const char *name;", "    recomp_func_t fn;", "};", "",
            "/* Sorted by name for bsearch. */", "static const struct gl_auto_entry g_auto_table[] = {"]
    out += [f'    {{"{s["name"]}", x_{s["name"]}}},' for s in sorted(kept, key=lambda s: s["name"])]
    out.append("};")
    out.append(LOOKUP_C.rstrip("\n"))
    return "\n".join(out) + "\n", stats


def main(argv: list[str]) -> int:
    binary = Path(argv[1]) if len(argv) > 1 else GAME_BIN
    out_path = Path(argv[2]) if len(argv) > 2 else ROOT / "runtime" / "src" / OUT_NAME
    candidates = game_names(binary)
    exported = host_exports(candidates)
    protos, typedefs = prototypes(header_ast())
    others = defined_elsewhere(out_path.parent, out_path.name)
    text, stats = generate(candidates, exported, others, protos, typedefs)
    out_path.write_text(text, encoding="utf-8")
    print(f"wrote {out_path}")
    print(json.dumps(stats, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
