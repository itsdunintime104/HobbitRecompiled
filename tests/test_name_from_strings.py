"""Checks on naming functions from Class::Member strings."""

from __future__ import annotations

import pytest

from tools.hobbit import name_from_strings as nfs


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("NuFoo::Bar", "NuFoo::Bar"),
        ("NuFoo::~NuFoo", "NuFoo::~NuFoo"),
        ("A::B::c", "A::B::c"),
        ("NuFoo::Bar: failed %d", "NuFoo::Bar"),
        ("NuFoo::Bar(%d) failed", "NuFoo::Bar"),
        ("NuFoo::Bar failed", None),
        ("NuFoo", None),
        ("see NuFoo::Bar", None),
        ("::Bar", None),
    ],
)
def test_candidate_name(text: str, expected: str | None) -> None:
    assert nfs.candidate_name(text) == expected


@pytest.mark.parametrize(
    ("member", "expected"),
    [("m_pool", True), ("s_pool", True), ("mPool", True), ("sPool", True),
     ("Method", False), ("mutate", False), ("size", False)],
)
def test_is_data_member(member: str, expected: bool) -> None:
    assert nfs.is_data_member(member) is expected


def test_unique_function_name_gets_string_confidence() -> None:
    candidates = {0x100: "NuFoo::Bar"}
    names, conflicts = nfs.assign_names(candidates, {0x2000: {0x100}})
    assert names == {
        "0x2000": {"name": "NuFoo::Bar", "confidence": "string", "evidence": "0x100"}
    }
    assert conflicts == 0


def test_shared_function_string_is_a_conflict() -> None:
    candidates = {0x100: "NuFoo::Bar"}
    names, conflicts = nfs.assign_names(candidates, {0x2000: {0x100}, 0x3000: {0x100}})
    assert names == {}
    assert conflicts == 2


def test_two_function_names_in_one_function_is_a_conflict() -> None:
    candidates = {0x100: "NuFoo::Bar", 0x110: "NuFoo::Baz"}
    names, conflicts = nfs.assign_names(candidates, {0x2000: {0x100, 0x110}})
    assert names == {}
    assert conflicts == 1


def test_data_member_gives_class_label_that_may_repeat() -> None:
    candidates = {0x100: "NuPool::m_memoryPool", 0x110: "NuPool::s_count"}
    names, conflicts = nfs.assign_names(candidates, {0x2000: {0x100}, 0x3000: {0x110}})
    assert names == {
        "0x2000": {"name": "NuPool", "confidence": "class", "evidence": "0x100"},
        "0x3000": {"name": "NuPool", "confidence": "class", "evidence": "0x110"},
    }
    assert conflicts == 0


def test_two_classes_in_one_function_is_a_conflict() -> None:
    candidates = {0x100: "NuA::m_x", 0x110: "NuB::m_y"}
    names, conflicts = nfs.assign_names(candidates, {0x2000: {0x100, 0x110}})
    assert names == {}
    assert conflicts == 1


def test_function_name_wins_over_class_label() -> None:
    candidates = {0x100: "NuFoo::Bar", 0x110: "NuFoo::m_x"}
    names, _ = nfs.assign_names(candidates, {0x2000: {0x100, 0x110}})
    assert names["0x2000"]["confidence"] == "string"
    assert names["0x2000"]["name"] == "NuFoo::Bar"
