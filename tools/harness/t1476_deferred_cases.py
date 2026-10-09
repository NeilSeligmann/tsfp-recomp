# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared T1476 ID/link and snapshot domains, opt-in namespace 71."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

INDEX_BASE = 71 << 40
ID_ROOTS = frozenset({0x9D740, 0x9D780})
SNAPSHOT_ROOTS = frozenset({0xDD130, 0xDCF90})
BANK = 0xD10000
POINTERS = 0xD12000
RECORD = 0xD12100
ENTRIES = 0xD13000
LOOKUP = 0xD15000
BYTES = 0xD15100
SNAPSHOT = 0x70AAAC


def word(value: int) -> bytes:
    return (value & 0xFFFFFFFF).to_bytes(4, "little")


def deferred_case_count(va: int) -> int:
    return 64 if va in ID_ROOTS else 32 if va in SNAPSHOT_ROOTS else 0


def make_deferred_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < deferred_case_count(va):
        raise ValueError("unsupported T1476 deferred fixture")
    case = make_case(seed, INDEX_BASE + ordinal, va, size, policy=policy)
    additions: list[tuple[int, bytes]] = []
    if va in ID_ROOTS:
        first, second = BANK, BANK + 32
        if ordinal < 4:
            first = 0 if ordinal & 1 else first
            second = 0 if ordinal & 2 else second
        additions.extend(
            [
                (case.esp + 4, word(first)),
                (case.esp + 8, word(second)),
                (0x715F2C, word(BANK - 3 * 32)),
                (0x715F30, word(ENTRIES)),
                (0x7B6664, word(100)),
                (0x7B666C, word(1)),
                (0x7B6770, word(POINTERS)),
                (POINTERS, word(RECORD)),
                (0x79094C, word(0)),
            ]
        )
        for index in range(2):
            pointer = BANK + 32 * index
            additions.extend(
                [
                    (pointer, word(1001 if not 4 <= ordinal < 8 else 0)),
                    (pointer + 4, word(1)),
                    (pointer + 8, word(0)),
                ]
            )
        primary, secondary = ordinal % 4, (ordinal // 4) % 4
        additions.extend(
            [(RECORD, word(0)), (RECORD + 4, word(primary)), (RECORD + 8, word(secondary))]
        )
        for index in range(primary + secondary):
            entry = ENTRIES + 20 * index
            left, right = (4, 3) if ordinal & 16 else (3, 4)
            if index != ordinal % 3:
                right = 9
            additions.extend(
                [
                    (RECORD + 0x18 + 4 * index, word(index)),
                    (entry, word(0x10000 if ordinal & 32 else 0)),
                    (entry + 4, word(left)),
                    (entry + 8, word(right)),
                    (entry + 12, word(0xA5000000 | index)),
                    (entry + 16, word(0x5A000000 | index)),
                ]
            )
    else:
        indices = (-1, 0, 1, 7, 8, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
        count = (0, 1, 8, 0x7FFFFFFF)[ordinal // 8]
        for index in range(-7, 8):
            value = indices[ordinal % 8] if index == 5 else 0xA5000000 ^ (index & 0xFF)
            additions.append((SNAPSHOT + 4 * index, word(value)))
        additions.extend(
            [
                (0x4B85C8, word(LOOKUP)),
                (LOOKUP + 4, word(BYTES)),
                (LOOKUP + 12, word(count)),
                (BYTES, bytes((ordinal + 17 * index) & 255 for index in range(16))),
            ]
        )
    return replace(case, patches=(*case.patches, *additions))
