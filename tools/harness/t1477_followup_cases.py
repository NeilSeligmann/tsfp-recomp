# SPDX-License-Identifier: GPL-3.0-or-later
"""T1477 predeclared equality/interval domains; additive and source-derived."""

import zlib
from dataclasses import replace

from .model import REG_NAMES, Case
from .seeding import GUEST_HI, SeedPolicy, make_case
from .t1477_crc_cases import CRC_LENGTHS, CRC_SOURCE_BASE, _pattern

FOLLOWUP_INDEX_BASE = 31 << 40
CRC_EQUALS_VA = 0x150560
INTERVAL_VA = 0x115F10
CRC_EQUALS_CASES = 427
INTERVAL_CASES = 38
RECORD_BASE = 0xD08000
LINK_BASE = 0xD14000
REMAP_BASE = 0xD15000
OPTIONAL_TABLE = 0xD16000


def followup_case_count(va: int) -> int:
    return {CRC_EQUALS_VA: CRC_EQUALS_CASES, INTERVAL_VA: INTERVAL_CASES}.get(va, 0)


def _expected(payload: bytes, relation: int) -> int:
    return zlib.crc32(payload) ^ (0, 1, 0x80000000)[relation]


def _equals_patches(case: Case, ordinal: int) -> list[tuple[int, bytes]]:
    patches = []
    if ordinal < 384:
        length = CRC_LENGTHS[1 + ordinal // 48]
        kind, alignment, relation = ordinal // 12 % 4, ordinal // 3 % 4, ordinal % 3
        pointer = CRC_SOURCE_BASE + alignment
        payload = _pattern(kind, length)
        patches.append((pointer, payload))
        expected = _expected(payload, relation)
    elif ordinal < 396:
        pointer = CRC_SOURCE_BASE + (ordinal - 384) // 3
        length, expected = 0, _expected(b"", (ordinal - 384) % 3)
    elif ordinal < 411:
        slot, relation = divmod(ordinal - 396, 3)
        pointer, length = case.esp - 20 + slot * 4, 4
        value = (
            case.regs[REG_NAMES.index("edi")],
            case.regs[REG_NAMES.index("esi")],
            0x15056F,
            pointer,
            length,
        )[slot]
        expected = _expected(value.to_bytes(4, "little"), relation)
    elif ordinal < 419:
        slot, relation = divmod(ordinal - 411, 2)
        pointer = (0, 1, 0xFFFFFFFF, 0xFFFFFFF0)[slot]
        length, expected = 0, relation
    else:
        slot = ordinal - 419
        pointer = (0, 1, 0xFFFFFFFF, 0xFFFFFFF0, GUEST_HI - 1, GUEST_HI - 32, 0, 0)[slot]
        length = (1, 1, 1, 1, 2, 33, 0x80000000, 0xFFFFFFFF)[slot]
        expected = 0
        if slot in (4, 5):
            patches.append((pointer, b"A" * (GUEST_HI - pointer)))
    patches.extend(
        (case.esp + offset, value.to_bytes(4, "little"))
        for offset, value in ((4, pointer), (8, length), (12, expected))
    )
    return patches


def _interval_patches(case: Case, ordinal: int) -> list[tuple[int, bytes]]:
    patches = []

    def word(address: int, value: int) -> None:
        patches.append((address, value.to_bytes(4, "little")))

    row, mode = (ordinal % 4, ordinal // 4) if ordinal < 24 else (0, 0)
    argument = 202 + row
    if 24 <= ordinal < 30:
        argument = (0, 201, 206, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF)[ordinal - 24]
    elif ordinal >= 30:
        row = (ordinal - 30) % 4
        argument = 202 + row
    word(case.esp + 4, argument)
    word(0x79094C, 101 if mode == 0 else 100)
    word(0x76B108, 0 if mode == 1 else 1)
    word(0x774028, 0 if mode == 2 else OPTIONAL_TABLE)
    word(0x790950, 3)
    word(OPTIONAL_TABLE + 0x20, 4 if mode == 4 else 3)
    word(0x7B0C48, 0 if 30 <= ordinal < 34 else RECORD_BASE)
    for neighbor in range(4):
        word(
            RECORD_BASE + neighbor * 0x1584 + 0x14,
            0 if ordinal >= 34 else LINK_BASE + neighbor * 0x80,
        )
        word(LINK_BASE + neighbor * 0x80 + 0x34, 11 + neighbor * 7)
        word(OPTIONAL_TABLE + 0xC + neighbor * 4, 0 if mode == 3 else REMAP_BASE + neighbor * 0x80)
        word(REMAP_BASE + neighbor * 0x80 + 0x3C, 3 - neighbor)
    return patches


def make_followup_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    count = followup_case_count(va)
    if not count or not 0 <= ordinal < count:
        raise ValueError("unsupported T1477 follow-up address or ordinal")
    case = make_case(seed, FOLLOWUP_INDEX_BASE + ordinal, va, size, policy=policy)
    additions = (
        _equals_patches(case, ordinal) if va == CRC_EQUALS_VA else _interval_patches(case, ordinal)
    )
    return replace(case, patches=(*case.patches, *additions))
