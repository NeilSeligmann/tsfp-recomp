# SPDX-License-Identifier: GPL-3.0-or-later
"""All 500 first-free positions, occupied limit, aliases and genuine read faults."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

FREE_SLOT_VA = 0x31040
FREE_SLOT_CASES = 640
FREE_SLOT_INDEX_BASE = 18 << 40


def has_free_slot_cases(va: int) -> bool:
    return va == FREE_SLOT_VA


def make_free_slot_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != FREE_SLOT_VA or not 0 <= ordinal < FREE_SLOT_CASES:
        raise ValueError("unsupported free-slot address or ordinal")
    case = make_case(seed, FREE_SLOT_INDEX_BASE + ordinal, va, size, policy=policy)
    base = 0x600000 + ordinal % 4
    first_free = ordinal if ordinal < 500 else 500
    if 520 <= ordinal < 540:
        first_free = 0
    elif 540 <= ordinal < 560:
        first_free = 499
    elif 560 <= ordinal < 580:
        base = 0x687EB4 - 20 * (ordinal % 20)
        first_free = 499
    elif 580 <= ordinal < 600:
        base = case.esp - 20 * (ordinal % 20)
        first_free = 499
    elif 600 <= ordinal < 620:
        base = 0
    elif 620 <= ordinal < 630:
        base = 0xFFFFFF
    elif ordinal >= 630:
        base = 0xFFFFFFF0
    occupied = (1, 3, 0x81, 0xFF)[ordinal % 4]
    free = (0, 2, 0x80, 0xFE)[ordinal % 4]
    patches = list(case.patches)
    if ordinal < 600:
        for index in range(500):
            patches.append((base + 20 * index, bytes([free if index == first_free else occupied])))
    elif ordinal < 630 and ordinal >= 620:
        patches.append((base, bytes([occupied])))
    patches.append((0x687EB4, base.to_bytes(4, "little")))
    patches.append((case.esp, (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    return replace(case, patches=tuple(patches), df=ordinal % 2)
