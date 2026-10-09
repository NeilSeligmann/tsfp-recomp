# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared T1480 hextodec inputs; namespace 39, no stream replacement."""

from dataclasses import replace

from .model import REG_NAMES, Case
from .seeding import GUEST_HI, SeedPolicy, make_case

HEXTODEC_INDEX_BASE = 39 << 40
HEXTODEC_ROOTS = {0x3CDA3C: 50}
HEXTODEC_PATTERNS = (0, 1, 4, 5, 0x100, 0x104)
HEXTODEC_LOCALES = (0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
HEXTODEC_TABLE = 0xD50004
HEXTODEC_CASES = 4224


def hextodec_case_count(va: int) -> int:
    return HEXTODEC_CASES if va in HEXTODEC_ROOTS else 0


def make_hextodec_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va not in HEXTODEC_ROOTS or not 0 <= ordinal < HEXTODEC_CASES:
        raise ValueError("unsupported T1480 hextodec case")
    case = make_case(seed, HEXTODEC_INDEX_BASE + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    pattern = None
    if ordinal < 3084:
        value = ordinal // 12 - 1
        locale = (0, 2)[ordinal // 6 % 2]
        pattern = HEXTODEC_PATTERNS[ordinal % 6]
        table = HEXTODEC_TABLE
    elif ordinal < 3132:
        at = ordinal - 3084
        value = (-1, 65)[at // 6 % 2]
        locale = (1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)[at // 12]
        pattern = HEXTODEC_PATTERNS[at % 6]
        table = HEXTODEC_TABLE
    elif ordinal < 3312:
        at = ordinal - 3132
        value = (-2, 256, 257, 0x80000000, 0x7FFFFFFF)[at // 36]
        locale = HEXTODEC_LOCALES[at // 6 % 6]
        pattern = HEXTODEC_PATTERNS[at % 6]
        table = HEXTODEC_TABLE
    elif ordinal < 3384:
        at = ordinal - 3312
        value = (0, -1, -2, 256)[at % 4]
        locale = HEXTODEC_LOCALES[at // 4 % 6]
        table = (0, 0xFFFFFFFF, GUEST_HI - 1)[at // 24]
    else:
        at = ordinal - 3384
        value = (0, 65, 255, -1)[at // 42 % 4]
        locale = HEXTODEC_LOCALES[at // 7 % 6]
        regs[REG_NAMES.index("esi")] = (0, 1, 2, 3, 8, 0x100, 0xFFFF)[at % 7]
        offset = (-4, -8, -12, -16, 4)[at // 168]
        table = (case.esp + offset - 2 * value) & 0xFFFFFFFF
    regs[REG_NAMES.index("eax")] = value & 0xFFFFFFFF
    additions = [
        (0x54D7F8, locale.to_bytes(4, "little")),
        (0x54D7F0, table.to_bytes(4, "little")),
    ]
    if pattern is not None:
        additions.append((HEXTODEC_TABLE - 4, pattern.to_bytes(2, "little") * 260))
    return replace(case, regs=tuple(regs), patches=(*case.patches, *additions))
