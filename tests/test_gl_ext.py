"""Compile and run the C checks for runtime/src/thunk_gl_ext.c (hidden CGL context)."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    ROOT / "runtime" / "src" / "thunk_gl_ext.c",
    ROOT / "runtime" / "src" / "thunk_gl.c",
    ROOT / "runtime" / "src" / "guest_heap.c",
    ROOT / "runtime" / "src" / "dlmalloc.c",
    ROOT / "runtime" / "src" / "guest_mem.c",
    ROOT / "runtime" / "src" / "macho_load.c",
    ROOT / "runtime" / "src" / "recomp_glue.c",
    ROOT / "runtime" / "tests" / "test_gl_ext.c",
]


def test_gl_ext(tmp_path: Path) -> None:
    exe = tmp_path / "test_gl_ext"
    subprocess.run(
        [
            "clang", "-std=gnu11", "-O1", "-w", "-arch", "arm64", "-DRECOMP_GLUE_NO_MAIN",
            "-I", str(ROOT / "tools" / "xrecomp" / "include"),
            "-I", str(ROOT / "runtime" / "include"),
            *map(str, SOURCES),
            "-framework", "OpenGL",
            "-o", str(exe),
        ],
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert "gl_ext ok" in result.stdout


# GL name -> {argument index: bind slot}: pointer arguments that are byte offsets
# while a buffer is bound.
OFFSET_ARGS = {
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


def test_offset_args_use_gl_buf_ptr() -> None:
    text = (ROOT / "runtime" / "src" / "thunk_gl_ext.c").read_text(encoding="utf-8")
    for name, args in OFFSET_ARGS.items():
        match = re.search(rf"static void x_{name}\(void\)\n\{{\n(.*?)\n\}}\n", text, re.S)
        assert match is not None, f"no thunk for {name}"
        body = match.group(1)
        for index, slot in args.items():
            line = next(ln for ln in body.splitlines() if re.search(rf"\ba{index} =", ln))
            assert f"gl_buf_ptr({slot}, " in line, f"{name} a{index}: {line.strip()}"
            assert "gp(" not in line, f"{name} a{index} still uses gp: {line.strip()}"
