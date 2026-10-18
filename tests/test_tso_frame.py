"""[ebp+...] is a plain access only where the lifter proves ebp is the frame pointer."""

from __future__ import annotations

from pathlib import Path

from tests.conformance.harness import Case, assemble, lift

MARKER = "/* frame: ebp proven */"


def _lift(tmp_path: Path, name: str, asm: str) -> str:
    return lift(assemble(Case(name, asm)), tmp_path)


def test_frame_pointer_function_is_plain(tmp_path: Path) -> None:
    c = _lift(tmp_path, "frame_plain",
              "push ebp\nmov ebp, esp\nmov eax, [ebp+8]\nmov [ebp-4], eax\nmov edx, [ecx]\npop ebp\nret")
    assert MARKER in c
    assert "MEM32(ebp + 8)" in c
    assert "MEM32(ebp + -4) =" in c
    assert "GLD32(ebp" not in c and "GST32(ebp" not in c
    assert "GLD32(ecx)" in c


def test_fpo_function_stays_ordered(tmp_path: Path) -> None:
    c = _lift(tmp_path, "frame_fpo", "mov ebp, ecx\nmov eax, [ebp+8]\nret")
    assert MARKER not in c
    assert "GLD32(ebp + 8)" in c


def test_mid_body_ebp_load_stays_ordered(tmp_path: Path) -> None:
    c = _lift(tmp_path, "frame_reload",
              "push ebp\nmov ebp, esp\nmov ebp, [ecx]\nmov eax, [ebp+8]\npop ebp\nret")
    assert MARKER not in c
    assert "GLD32(ebp + 8)" in c


def test_access_after_pop_ebp_stays_ordered(tmp_path: Path) -> None:
    c = _lift(tmp_path, "frame_pop",
              "push ebp\nmov ebp, esp\npop ebp\nmov eax, [ebp+4]\nret")
    assert MARKER not in c
    assert "GLD32(ebp + 4)" in c
