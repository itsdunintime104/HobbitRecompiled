"""Checks on the log symbolizer."""

from __future__ import annotations

from tools.hobbit import symbolize

SYMBOLS = symbolize.Symbols(
    starts=[0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7000],
    code_end=0x7100,
    strings={
        0x1000: {"name": "NuA::Run", "confidence": "string", "evidence": "0x9"},
        0x2000: {"name": "NuB", "confidence": "class", "evidence": "0x9"},
        0x4000: {"name": "NuC::FromStrings", "confidence": "string", "evidence": "0x9"},
    },
    saga={
        0x1000: {"file": "a.cpp", "line": 1, "name": "SagaA", "confidence": "name"},
        0x3000: {"file": "c.cpp", "line": 7, "name": None, "confidence": "file"},
        0x4000: {"file": "d.cpp", "line": 8, "name": "SagaD", "confidence": "name"},
        0x5000: {"file": "e.cpp", "line": 9, "name": "SagaE", "confidence": "name"},
        0x6000: {"file": "f.cpp", "line": 3, "name": None, "confidence": "file"},
    },
    ghidra={0x5000: "GhidraE", 0x6000: "GhidraF", 0x2000: "GhidraB"},
)


def test_string_name_beats_saga_name() -> None:
    assert symbolize.best_name(SYMBOLS, 0x1000) == "NuA::Run"
    assert symbolize.best_name(SYMBOLS, 0x4000) == "NuC::FromStrings"


def test_saga_name_beats_ghidra_name() -> None:
    assert symbolize.best_name(SYMBOLS, 0x5000) == "SagaE"


def test_ghidra_name_beats_class_label_and_file_label() -> None:
    assert symbolize.best_name(SYMBOLS, 0x2000) == "GhidraB"
    assert symbolize.best_name(SYMBOLS, 0x6000) == "GhidraF"


def test_class_label_beats_file_label() -> None:
    symbols = SYMBOLS._replace(
        saga={0x2000: {"file": "b.cpp", "line": 2, "name": None, "confidence": "file"}},
        ghidra={},
    )
    assert symbolize.best_name(symbols, 0x2000) == "NuB::*"


def test_file_label_and_lifted_symbol_fallbacks() -> None:
    assert symbolize.best_name(SYMBOLS, 0x3000) == "c.cpp:7"
    assert symbolize.best_name(SYMBOLS, 0x7000) == "sub_00007000"


def test_offset_inside_function() -> None:
    assert symbolize.resolve(SYMBOLS, 0x1000) == "NuA::Run"
    assert symbolize.resolve(SYMBOLS, 0x1010) == "NuA::Run+0x10"


def test_outside_code_is_unknown() -> None:
    assert symbolize.resolve(SYMBOLS, 0xFFF) is None
    assert symbolize.resolve(SYMBOLS, 0x7100) is None


def test_text_rewrite_keeps_unknown_and_short_numbers() -> None:
    line = "RECOMP: caller=0x00001004 last 0xBFFFF000 n=0x12 0x1234567890"
    assert symbolize.symbolize_text(SYMBOLS, line) == (
        "RECOMP: caller=0x00001004 [NuA::Run+0x4] last 0xBFFFF000 n=0x12 0x1234567890"
    )


def test_text_rewrite_start_has_no_offset() -> None:
    assert symbolize.symbolize_text(SYMBOLS, "at 0x00003000\n") == "at 0x00003000 [c.cpp:7]\n"
