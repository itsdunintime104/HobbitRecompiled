"""Checks on the engine file and name labels (names_saga.json)."""

from __future__ import annotations

import json
import struct
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.hobbit import disasm, name_from_saga as nfs

NAMES = Path(__file__).resolve().parent.parent / "recon" / "names_saga.json"
BASE = 0x2000


def _insns(code: bytes) -> list[disasm.Insn]:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    return [disasm.Insn(a, s, m, o) for a, s, m, o in md.disasm_lite(code, BASE)]


def test_finds_pic_lea_and_push_references() -> None:
    pop_addr = BASE + 5
    target = 0x116E8D8
    code = (
        b"\xe8\x00\x00\x00\x00"  # call +0
        b"\x5b"  # pop ebx
        + b"\x8d\x83" + struct.pack("<i", target - pop_addr)  # lea eax, [ebx + disp]
        + b"\x68" + struct.pack("<I", target + 8)  # push imm32
        + b"\x8d\x83" + struct.pack("<i", 0x10)  # lea eax, [ebx + 0x10]: not a target
        + b"\xc3"
    )
    sites = nfs.find_references(_insns(code), {target, target + 8})
    assert [s.string_addr for s in sites] == [target, target + 8]


def test_file_string_regex_and_file_choice() -> None:
    text = "/private/tmp/x/AccuRev/legohobbit-1/nu2api/nucore/NuMemoryManager.cpp:1562"
    match = nfs.FILE_STRING_RE.match(text)
    assert match is not None
    assert (match["path"], int(match["line"])) == ("nu2api/nucore/NuMemoryManager.cpp", 1562)
    cpp = nfs.FileString(1, "nu2api/a.cpp", 10)
    header = nfs.FileString(2, "nu2api/a.h", 20)
    sites = [nfs.Site(0x10, 2), nfs.Site(0x20, 1)]
    assert nfs.pick_site(sites, {1: cpp, 2: header}) == cpp


SOURCE = """
int Other::Run(int a) { return Log("first message here"); }
void Thing::Start(int a, char *b) {
    Assert(a, "thing start failed\\n");  // "commented out text"
}
void Thing::Start(int a) { Log("one arg " "joined text"); }
"""


def test_saga_source_parsing_and_unique_match() -> None:
    assert nfs.split_demangled("Thing::Start(int, char*)") == ("Thing::Start", 2)
    assert nfs.split_demangled("Thing::Stop()") == ("Thing::Stop", 0)
    two = nfs.function_body(SOURCE, "Thing::Start", 2)
    one = nfs.function_body(SOURCE, "Thing::Start", 1)
    assert two is not None and one is not None
    assert nfs.source_literals(two) == {"thing start failed\n"}
    assert nfs.source_literals(one) == {"one arg joined text"}
    shared = {"one arg joined text"}
    candidates = [("Other::Run(int)", {"first message here"}), ("Thing::Start(int)", shared)]
    assert nfs.propose_name(candidates, shared) == "Thing::Start(int)"
    assert nfs.propose_name(candidates, {"unseen text"}) is None
    assert nfs.propose_name(candidates + [("Third::Run()", shared)], shared) is None


def test_committed_labels() -> None:
    names = json.loads(NAMES.read_text())
    entry = names["0x1A8560"]
    assert entry["file"] == "nu2api/nucore/NuMemoryManager.cpp"
    assert entry["line"] == 1562
    assert entry["confidence"] == "file"
    for item in names.values():
        assert item["confidence"] in ("file", "name")
        assert (item["name"] is not None) == (item["confidence"] == "name")
    assert any(item["confidence"] == "name" for item in names.values())
