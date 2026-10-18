"""Fail on a hand-typed four-char code in the runtime sources.

A literal that spells a code, such as 0x73726E64 ('srnd'), must be an SDK constant name.
A literal with a quoted code in its line comment must spell one of the quoted
codes.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {"thunk_gl_ext.c", "thunk_auto.c", "dlmalloc.c"}
ALLOWED_DEFINES = (
    "GUARD_LIVE", "GUARD_FREE", "GT_MUTEX_SIG", "GT_COND_SIG", "GT_MUTEXATTR_SIG", "GT_ATTR_SIG",
)
LITERAL = re.compile(r"\b0x([0-9A-Fa-f]{8})[uU]?\b")
COMMENT = re.compile(r"/\*|//")
QUOTED = re.compile(r"'([ -~]{4})'")
CODE_BYTES = set(b" ?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
LETTERS = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
ALLOWED_RE = re.compile(r"#define (?:" + "|".join(ALLOWED_DEFINES) + r") ")


def sources() -> list[Path]:
    files = [*(ROOT / "runtime/src").glob("*.c"), *(ROOT / "runtime/include").glob("*.h")]
    return sorted(p for p in files if p.name not in EXCLUDED)


def check_line(line: str) -> list[str]:
    bad: list[str] = []
    cm = COMMENT.search(line)
    quoted = QUOTED.findall(line[cm.end():]) if cm else []
    for m in LITERAL.finditer(line):
        raw = bytes.fromhex(m.group(1))
        if quoted:
            if raw.decode("latin-1") not in quoted:
                bad.append(f"{m.group(0)} does not spell any of {quoted}")
        elif set(raw) <= CODE_BYTES and set(raw) & LETTERS and not ALLOWED_RE.match(line):
            bad.append(f"{m.group(0)} spells {raw.decode('ascii')!r}")
    return bad


def test_no_hand_typed_four_char_codes() -> None:
    failures: list[str] = []
    for path in sources():
        for no, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            for why in check_line(line):
                failures.append(
                    f"{path.relative_to(ROOT)}:{no}: hand-typed four-char code: "
                    f"use the SDK constant name ({why})"
                )
    assert not failures, "\n" + "\n".join(failures)
