# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact, mandatory raw x87 observations; no arithmetic or legacy conversion."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class X87State:
    """Eight physical raw80 slots and complete tag/control/status words.

    Empty register contents are deliberately retained. TOP does not encode depth.
    Instruction/data pointers and opcode are not part of this contract.
    """

    physical: tuple[int, ...]
    tags: int
    control: int
    status: int

    def __post_init__(self) -> None:
        if not isinstance(self.physical, tuple) or len(self.physical) != 8:
            raise ValueError("x87 requires exactly eight physical slots")
        for raw in self.physical:
            if type(raw) is not int or not 0 <= raw < 1 << 80:
                raise ValueError("x87 slot must be an unsigned raw80 integer")
        for word in (self.tags, self.control, self.status):
            if type(word) is not int or not 0 <= word <= 0xFFFF:
                raise ValueError("x87 words must be unsigned raw16 integers")

    @property
    def top(self) -> int:
        return (self.status >> 11) & 7

    def tag(self, physical_index: int) -> int:
        if type(physical_index) is not int or not 0 <= physical_index < 8:
            raise ValueError("physical index must be in 0..7")
        return (self.tags >> (physical_index * 2)) & 3

    @property
    def occupied(self) -> tuple[int, ...]:
        return tuple(index for index in range(8) if self.tag(index) != 3)

    def logical(self, index: int) -> int:
        if type(index) is not int or not 0 <= index < 8:
            raise ValueError("logical index must be in 0..7")
        return self.physical[(self.top + index) & 7]


def differences(expected: X87State | None, actual: X87State | None) -> tuple[str, ...]:
    """Fail closed when a required observation is missing, even on both sides."""
    if expected is None or actual is None:
        return ("missing required x87 state",)
    result = [
        f"physical[{index}]"
        for index, (left, right) in enumerate(zip(expected.physical, actual.physical, strict=True))
        if left != right
    ]
    result.extend(
        name
        for name in ("tags", "control", "status")
        if getattr(expected, name) != getattr(actual, name)
    )
    return tuple(result)


def encode_record(state: X87State, *, output: bool = False) -> str:
    """Version1 wire record with exact-width hex physical register slots."""
    name = "X87RAWOUT" if output else "X87RAWIN"
    fields = [name, "1", f"{state.tags:04X}", f"{state.control:04X}", f"{state.status:04X}"]
    fields.extend(f"{raw:020X}" for raw in state.physical)
    return " ".join(fields)


def decode_record(record: str, *, output: bool = False) -> X87State:
    """Reject unknown versions, widths, counts or non-hex fields."""
    fields = record.split()
    name = "X87RAWOUT" if output else "X87RAWIN"
    if len(fields) != 13 or fields[:2] != [name, "1"]:
        raise ValueError("invalid raw x87 record or version")
    for text, width in zip(fields[2:], (4, 4, 4, *([20] * 8)), strict=True):
        if len(text) != width or any(char not in "0123456789abcdefABCDEF" for char in text):
            raise ValueError("invalid raw x87 hexadecimal field")
    tags, control, status = (int(text, 16) for text in fields[2:5])
    return X87State(tuple(int(text, 16) for text in fields[5:]), tags, control, status)
