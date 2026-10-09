# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen T1550 synthetic programs and independent scalar reference data.

Importing this module never constructs or executes an emulator. Game declarations
and custom capability are deliberately untouched.
"""

from __future__ import annotations

from dataclasses import replace

from .model import Case
from .seeding import make_case

SEEDS = (20261001, 20261006)
NAMESPACE = 93
PROGRAMS = {
    0x10000: bytes.fromhex("0fb605000002008b0d040002008b118b04851000020089150400dc00c3"),
    0x10080: bytes.fromhex("e83b0000008b35080002008b1d0c000200891d0800dc00a118000200c3"),
    0x100C0: bytes.fromhex("8b0d040002008b11a110000200c3"),
    0x100F0: bytes.fromhex("8b4424048b4c2408a30c00dc00c20800"),
}
GUARD_PROGRAMS = {
    0x10200: bytes.fromhex("c605070201009040c3"),
    0x10220: bytes.fromhex("c7042400100100c3"),
    0x10240: bytes.fromhex("83c404c3"),
    0x10260: bytes.fromhex("bc00000000c3"),
}
BYTE_VALUES = (0, 1, 2, 3, 127, 128, 254, 255)
POINTERS = (0xDC0040, 0, 0x1000000, 0xFFFFFFFF)
PAYLOADS = (0, 1, 0x80000000, 0xFFFFFFFF)
PROVIDER_COUNT = 128
ORDINARY_TUPLE_COUNT = 776
LOGICAL_ROWS = 7136
PLANNED_ATTEMPTS = 7144
SUBJECT_COMPARISONS = 6984


def dword(value: int) -> bytes:
    return value.to_bytes(4, "little")


def baseline_patches() -> tuple[tuple[int, bytes], ...]:
    return (
        (0x20000, b"\x01"),
        (0x20004, dword(0xDC0040)),
        (0x20008, dword(0x13579BDF)),
        (0x2000C, dword(0x2468ACE1)),
        (0x20010, b"".join(dword(0x6A000001 + i) for i in range(256))),
        (0xDC0040, dword(0x45236789)),
    )


def make_provider_case(seed: int, ordinal: int) -> Case:
    if type(ordinal) is not int or not 0 <= ordinal < PROVIDER_COUNT:
        raise ValueError("T1550 provider ordinal outside frozen domain")
    byte_index, tail = divmod(ordinal, 16)
    pointer_index, payload_index = divmod(tail, 4)
    byte = BYTE_VALUES[byte_index]
    case = make_case(seed, (NAMESPACE << 40) + ordinal, 0x10000, len(PROGRAMS[0x10000]))
    return replace(
        case,
        patches=case.patches
        + (
            (0x20000, bytes((byte,))),
            (0x20004, dword(POINTERS[pointer_index])),
            (0x20010 + 4 * byte, dword(0x6B000000 | ordinal)),
            (0xDC0040, dword(PAYLOADS[payload_index])),
        ),
    )
