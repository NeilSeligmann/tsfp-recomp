# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded byte gates, partial registers, argument/return aliases and real faults."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

SLOT_BYTE_VA = 0x25B80
SLOT_BYTE_CASES = 512
SLOT_BYTE_INDEX_BASE = 21 << 40


def has_slot_byte_cases(va: int) -> bool:
    return va == SLOT_BYTE_VA


def make_slot_byte_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != SLOT_BYTE_VA or not 0 <= ordinal < SLOT_BYTE_CASES:
        raise ValueError("unsupported slot predicate address or ordinal")
    case = make_case(seed, SLOT_BYTE_INDEX_BASE + ordinal, va, size, policy=policy)
    slot = tuple(range(11)) + (11, 0x80000000, 0xFFFFFFFF, 0x100, 0xFFFFFFFE)
    slot = slot[ordinal % 16]
    shape = (ordinal // 16) % 8
    address = 0x5655B0 + 48 * (slot if slot < 11 else 0)
    first = (1, 0x80, 0xFF)[ordinal % 3]
    second = (0, 1, 0x80, 0xFF)[(ordinal // 8) % 4]
    if shape == 1:
        first = 0
    if shape == 2:
        second = 0
    regs = list(case.regs)
    if shape == 3:
        regs[4] = 0xFFFFFC
    elif shape == 5:
        regs[4] = address - 4
    elif shape == 6:
        regs[4] = address
    elif shape == 7:
        regs[4] = address - 7
    patches = list(case.patches)
    patches.extend(((address, bytes([first])), (address + 3, bytes([second]))))
    patches.append((regs[4], (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    if shape != 3:
        patches.append((regs[4] + 4, slot.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
