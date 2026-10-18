"""Build recon/import_sigs.json: C prototype and i386 argument layout per import.

Usage: python -m tools.hobbit.import_sigs <analysis.json> -o <json>

Prototypes come from clang's JSON AST of the macOS SDK headers, compiled for
i386. Imports that headers cannot describe (vendor libraries, C++ and compiler
runtime, Objective-C runtime) are filled in by hand in MANUAL.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import tempfile
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import Any

HEADERS = [
    "stdio.h", "stdlib.h", "string.h", "unistd.h", "fcntl.h", "pthread.h",
    "math.h", "time.h", "ctype.h", "errno.h", "sys/stat.h", "sys/mman.h",
    "dlfcn.h", "mach/mach_time.h", "mach/mach.h", "mach/task_policy.h",
    "libkern/OSAtomic.h", "setjmp.h", "sys/socket.h", "netdb.h",
    "arpa/inet.h", "dirent.h", "pwd.h", "sys/time.h", "sys/resource.h",
    "sched.h", "bootstrap.h", "CoreFoundation/CoreFoundation.h",
    "OpenGL/gl.h", "OpenGL/OpenGL.h", "OpenGL/glext.h",
    "ApplicationServices/ApplicationServices.h", "IOKit/IOKitLib.h",
    "IOKit/hid/IOHIDLib.h", "IOKit/pwr_mgt/IOPMLib.h", "IOKit/IOCFPlugIn.h",
    "AudioToolbox/AudioToolbox.h", "AudioUnit/AudioUnit.h",
    "CoreAudio/CoreAudio.h", "CoreServices/CoreServices.h",
    "Carbon/Carbon.h", "objc/runtime.h", "objc/message.h",
]  # fmt: skip
MANUAL_DYLIBS = ("Cg", "Vapor", "libsteam_api")
ABI_SOURCE = "i386"
DATA_PREFIX = "data symbol of type "
PROBE = "zzprobe_"
CLANG_ARGS = ["-target", "i386-apple-macosx10.9", "-x", "c", "-fsyntax-only"]


class Status(StrEnum):
    RESOLVED = "resolved"
    MANUAL = "manual"
    UNRESOLVED = "unresolved"


class Kind(StrEnum):
    I32 = "i32"
    U32 = "u32"
    I64 = "i64"
    U64 = "u64"
    F32 = "f32"
    F64 = "f64"
    PTR = "ptr"
    STRUCT = "struct"
    VOID = "void"


@dataclass(frozen=True)
class CType:
    type: str
    kind: Kind
    size: int

    def param_json(self) -> dict[str, Any]:
        return {
            "type": self.type,
            "kind": self.kind,
            "size": self.size,
            "stack_size": (self.size + 3) & ~3,
        }

    def ret_json(self) -> dict[str, Any]:
        return {"type": self.type, "kind": self.kind, "size": self.size}


def _t(text: str, kind: Kind, size: int) -> CType:
    return CType(text, kind, size)


PTR = _t("void *", Kind.PTR, 4)
I32 = _t("int", Kind.I32, 4)
U32 = _t("unsigned int", Kind.U32, 4)
I64 = _t("long long", Kind.I64, 8)
U64 = _t("unsigned long long", Kind.U64, 8)
F32 = _t("float", Kind.F32, 4)
F64 = _t("double", Kind.F64, 8)
VOID = _t("void", Kind.VOID, 0)

BUILTINS: dict[str, CType] = {
    "void": VOID,
    "char": _t("char", Kind.I32, 1),
    "signed char": _t("signed char", Kind.I32, 1),
    "unsigned char": _t("unsigned char", Kind.U32, 1),
    "short": _t("short", Kind.I32, 2),
    "unsigned short": _t("unsigned short", Kind.U32, 2),
    "int": I32,
    "unsigned int": U32,
    "long": _t("long", Kind.I32, 4),
    "unsigned long": _t("unsigned long", Kind.U32, 4),
    "long long": I64,
    "unsigned long long": U64,
    "float": F32,
    "double": F64,
    "_Bool": _t("_Bool", Kind.U32, 1),
}


@dataclass(frozen=True)
class Manual:
    ret: CType | None
    params: tuple[CType, ...]
    variadic: bool
    note: str


def _m(
    ret: CType | None, *params: CType, variadic: bool = False, note: str = ""
) -> Manual:
    return Manual(ret, params, variadic, note)


VENDOR = "vendor library; signature not extracted, fill in by hand"
OBJC_CLASS = "Objective-C class reference symbol (data)"
UNWIND = "called by the unwinder, not by guest code"

MANUAL: dict[str, Manual] = {
    "__Znwm": _m(PTR, U32, note="operator new(unsigned long); size_t is 4 bytes"),
    "__Znam": _m(PTR, U32, note="operator new[](unsigned long)"),
    "__ZdlPv": _m(VOID, PTR, note="operator delete(void*)"),
    "__ZdaPv": _m(VOID, PTR, note="operator delete[](void*)"),
    "__ZNSt8ios_base4InitC1Ev": _m(VOID, PTR, note="std::ios_base::Init::Init(); this"),
    "__ZNSt8ios_base4InitD1Ev": _m(VOID, PTR, note="std::ios_base::Init::~Init(); this"),
    "___cxa_guard_acquire": _m(I32, PTR, note="C++ static-init guard"),
    "___cxa_guard_release": _m(VOID, PTR, note="C++ static-init guard"),
    "___cxa_guard_abort": _m(VOID, PTR, note="C++ static-init guard"),
    "___cxa_pure_virtual": _m(VOID, note="C++ runtime; aborts"),
    "___cxa_atexit": _m(I32, PTR, PTR, PTR, note="C++ ABI; func, arg, dso_handle"),
    "___gxx_personality_v0": _m(None, note=f"C++ personality routine; {UNWIND}"),
    "__Unwind_Resume": _m(VOID, PTR, note="exception unwinder entry"),
    "___divdi3": _m(I64, I64, I64, note="libgcc 64-bit signed divide"),
    "___moddi3": _m(I64, I64, I64, note="libgcc 64-bit signed remainder"),
    "___udivdi3": _m(U64, U64, U64, note="libgcc 64-bit unsigned divide"),
    "___umoddi3": _m(U64, U64, U64, note="libgcc 64-bit unsigned remainder"),
    "___fixunsdfdi": _m(U64, F64, note="libgcc double to u64"),
    "___fixunssfdi": _m(U64, F32, note="libgcc float to u64"),
    "_objc_msgSend": _m(
        PTR, PTR, PTR, variadic=True, note="id objc_msgSend(id, SEL, ...); dispatch by hand"
    ),
    "_objc_msgSendSuper": _m(
        PTR, PTR, PTR, variadic=True, note="id objc_msgSendSuper(struct objc_super*, SEL, ...)"
    ),
    "_objc_msgSend_fpret": _m(
        F64, PTR, PTR, variadic=True, note="returns floating point in st(0)"
    ),
    "_objc_msgSend_stret": _m(
        VOID, PTR, PTR, PTR, variadic=True, note="first arg is the struct return slot"
    ),
    "___stack_chk_fail": _m(VOID, note="stack protector failure; aborts"),
    "___stack_chk_guard": _m(U32, note="data symbol: stack protector canary"),
    "___CFConstantStringClassReference": _m(
        None, note="data symbol: class object for constant CFStrings; not in SDK headers"
    ),
    "_NSLog": _m(VOID, PTR, variadic=True, note="NSLog(NSString *format, ...)"),
    "_NSHomeDirectory": _m(PTR, note="NSString *NSHomeDirectory(void)"),
    "_NSRunAlertPanel": _m(
        I32, PTR, PTR, PTR, PTR, PTR, variadic=True,
        note="title, msg, defaultButton, alternateButton, otherButton, ...",
    ),  # fmt: skip
    "_NSApp": _m(PTR, note="data symbol: NSApplication *"),
    "_NSDefaultRunLoopMode": _m(PTR, note="data symbol: NSString *"),
    "_NSViewGlobalFrameDidChangeNotification": _m(PTR, note="data symbol: NSString *"),
    "_NSWindowWillCloseNotification": _m(PTR, note="data symbol: NSString *"),
}

_ATTRIBUTE = re.compile(r"__attribute__\(\(.*?\)\)\s*")
_QUALIFIER = re.compile(r"\b(const|volatile|restrict|__restrict|_Nonnull|_Nullable|_Null_unspecified|__nullable|__nonnull)\b")


def clean(text: str) -> str:
    text = _ATTRIBUTE.sub("", text)
    text = _QUALIFIER.sub("", text)
    return re.sub(r"\s+", " ", text).strip()


def top_level(text: str, sep: str) -> list[str]:
    """Split text on sep outside any parentheses or brackets."""
    parts: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts]


def split_function(text: str) -> tuple[str, str] | None:
    """Return (return type, argument text) when text is a function type."""
    groups: list[tuple[int, int]] = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch == "(":
            if depth == 0:
                start = i
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                groups.append((start, i))
    if len(groups) == 1 and groups[0][1] == len(text) - 1:
        return text[: groups[0][0]].strip(), text[groups[0][0] + 1 : -1]
    return None


def is_pointer_like(text: str) -> bool:
    return any(ch in text for ch in "*[^")


def run_clang(sdk: str, source: str) -> list[dict[str, Any]]:
    """Compile source and return the probe declarations from the JSON AST."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "probe.c"
        path.write_text(source)
        proc = subprocess.run(
            ["clang", *CLANG_ARGS, "-isysroot", sdk, "-Xclang", "-ast-dump=json",
             "-Xclang", f"-ast-dump-filter={PROBE}", str(path)],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
    decoder = json.JSONDecoder()
    out = proc.stdout
    decls: list[dict[str, Any]] = []
    pos = 0
    while True:
        while pos < len(out) and out[pos].isspace():
            pos += 1
        if pos >= len(out):
            return decls
        obj, pos = decoder.raw_decode(out, pos)
        decls.append(obj)


def probe_types(sdk: str, prologue: str, lines: dict[str, str]) -> dict[str, str | None]:
    """Compile one declaration per key; return its type text, or None if invalid.

    A typedef reports its fully desugared type in desugaredQualType.
    """
    source = prologue + "\n".join(f"{decl}" for decl in lines.values()) + "\n"
    found: dict[str, str | None] = {}
    for decl in run_clang(sdk, source):
        name = str(decl.get("name", ""))
        if not name.startswith(PROBE) or "type" not in decl:
            continue
        found[name] = (
            None
            if decl.get("isInvalid")
            else clean(decl["type"].get("desugaredQualType") or decl["type"]["qualType"])
        )
    return {key: found.get(PROBE + key) for key in lines}


def c_name(symbol: str) -> str:
    """Mach-O symbol to C identifier: drop one underscore and any $ suffix."""
    return symbol[1:].split("$", 1)[0]


@dataclass
class Signature:
    ret: CType | None
    params: list[CType]
    variadic: bool
    note: str


def build(imports: list[dict[str, str]]) -> dict[str, Signature | str]:
    """Map each non-manual import name to a Signature, or to a reason it failed."""
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    prologue = "".join(f"#include <{h}>\n" for h in HEADERS)
    names = [i["name"] for i in imports]
    fn_decls = {
        f"fn_{n}": f"typedef __typeof__({c_name(n)}) {PROBE}fn_{n};" for n in names
    }
    fn_types = probe_types(sdk, prologue, fn_decls)

    parsed: dict[str, tuple[str, list[str], bool] | str] = {}
    for n in names:
        text = fn_types[f"fn_{n}"]
        if text is None:
            parsed[n] = "not declared in the SDK headers"
            continue
        parts = split_function(text)
        if parts is None:
            parsed[n] = f"{DATA_PREFIX}{text}"
            continue
        ret, arg_text = parts
        args = [a for a in top_level(arg_text, ",") if a]
        variadic = bool(args) and args[-1] == "..."
        if variadic:
            args.pop()
        if args == ["void"]:
            args = []
        parsed[n] = (ret, [clean(a) for a in args], variadic)

    wanted: set[str] = set()
    for entry in parsed.values():
        if isinstance(entry, str):
            continue
        for text in [entry[0], *entry[1]]:
            if not is_pointer_like(text) and text not in BUILTINS:
                wanted.add(text)
    ordered = sorted(wanted)
    canon_raw = probe_types(
        sdk, prologue, {f"ty_{i}": f"typedef {t} {PROBE}ty_{i};" for i, t in enumerate(ordered)}
    )
    canon = {t: canon_raw[f"ty_{i}"] for i, t in enumerate(ordered)}

    sized = sorted(
        {c for c in canon.values() if c and c not in BUILTINS and not is_pointer_like(c)}
    )
    size_lines: dict[str, str] = {}
    for i, c in enumerate(sized):
        size_lines[f"sz_{i}"] = f"typedef char {PROBE}sz_{i}[sizeof({c})];"
        size_lines[f"sg_{i}"] = f"typedef char {PROBE}sg_{i}[({c})-1 < 0 ? 1 : 2];"
    size_raw = probe_types(sdk, prologue, size_lines) if size_lines else {}
    layouts: dict[str, CType] = {}
    for i, c in enumerate(sized):
        size_text, sign_text = size_raw.get(f"sz_{i}"), size_raw.get(f"sg_{i}")
        if size_text is None:
            continue
        match = re.search(r"\[(\d+)\]", size_text)
        if not match:
            continue
        size = int(match.group(1))
        if sign_text is None:
            layouts[c] = _t(c, Kind.STRUCT, size)
        else:
            signed = "[1]" in sign_text
            kind = (Kind.I64 if signed else Kind.U64) if size == 8 else (Kind.I32 if signed else Kind.U32)
            layouts[c] = _t(c, kind, size)

    def classify(text: str) -> CType | str:
        if is_pointer_like(text):
            return _t(text, Kind.PTR, 4)
        if text in BUILTINS:
            return BUILTINS[text]
        target = canon.get(text)
        if target is None:
            return f"cannot resolve type {text}"
        if is_pointer_like(target):
            return _t(text, Kind.PTR, 4)
        if target in BUILTINS:
            base = BUILTINS[target]
            return _t(text, base.kind, base.size)
        if target in layouts:
            return _t(text, layouts[target].kind, layouts[target].size)
        return f"cannot size type {target}"

    result: dict[str, Signature | str] = {}
    for n, entry in parsed.items():
        if isinstance(entry, str):
            result[n] = entry
            continue
        ret_text, arg_texts, variadic = entry
        types = [classify(t) for t in [ret_text, *arg_texts]]
        bad = [t for t in types if isinstance(t, str)]
        if bad:
            result[n] = bad[0]
            continue
        ctypes = [t for t in types if isinstance(t, CType)]
        note = "x87 long double not modeled" if any(t.size > 8 for t in ctypes) else ""
        result[n] = Signature(ctypes[0], ctypes[1:], variadic, note)
    return result


def entry(
    name: str, dylib: str, status: Status, sig: Signature | None, note: str
) -> dict[str, Any]:
    return {
        "name": name,
        "dylib": dylib,
        "status": status,
        "ret": sig.ret.ret_json() if sig and sig.ret else None,
        "params": [p.param_json() for p in sig.params] if sig else [],
        "variadic": sig.variadic if sig else False,
        "abi_source": ABI_SOURCE,
        "note": note,
    }


def is_manual_dylib(dylib: str) -> bool:
    base = dylib.rsplit("/", 1)[-1]
    return any(base.startswith(prefix) for prefix in MANUAL_DYLIBS)


def generate(analysis: dict[str, Any]) -> dict[str, Any]:
    imports: list[dict[str, str]] = analysis["imports"]
    hand = {
        i["name"]
        for i in imports
        if i["name"] in MANUAL
        or is_manual_dylib(i["dylib"])
        or i["name"].startswith(".objc_class_name_")
    }
    probed = build([i for i in imports if i["name"] not in hand])
    out: list[dict[str, Any]] = []
    for imp in imports:
        name, dylib = imp["name"], imp["dylib"]
        if name in MANUAL:
            m = MANUAL[name]
            sig = Signature(m.ret, list(m.params), m.variadic, m.note)
            out.append(entry(name, dylib, Status.MANUAL, sig, m.note))
        elif name.startswith(".objc_class_name_"):
            out.append(entry(name, dylib, Status.MANUAL, None, OBJC_CLASS))
        elif name in hand:
            out.append(entry(name, dylib, Status.MANUAL, None, VENDOR))
        else:
            res = probed[name]
            if isinstance(res, Signature):
                out.append(entry(name, dylib, Status.RESOLVED, res, res.note))
            elif res.startswith(DATA_PREFIX):
                out.append(entry(name, dylib, Status.MANUAL, None, res))
            else:
                out.append(entry(name, dylib, Status.UNRESOLVED, None, res))
    out.sort(key=lambda e: e["name"])
    return {"imports": out}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("analysis", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    result = generate(json.loads(args.analysis.read_text()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=1) + "\n")


if __name__ == "__main__":
    main()
