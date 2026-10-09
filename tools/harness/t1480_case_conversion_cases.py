# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared T1480 second-batch inputs; namespace 36, no stream replacement."""

from dataclasses import replace

from .model import REG_NAMES, Case
from .seeding import GUEST_HI, SeedPolicy, make_case

CONVERSION_INDEX_BASE = 36 << 40
CONVERSION_ROOTS = {0x3CD157: 48, 0x3CAD32: 49, 0x3CF214: 49}
CONVERSION_PATTERNS = (0, 1, 2, 3, 8, 0x100)
CONVERSION_LOCALES = (0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
CONVERSION_TABLE = 0xD50004
CONVERSION_CASES = 4224


def conversion_case_count(va: int) -> int:
    return CONVERSION_CASES if va in CONVERSION_ROOTS else 0


def make_conversion_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va not in CONVERSION_ROOTS or not 0 <= ordinal < CONVERSION_CASES:
        raise ValueError("unsupported T1480 conversion case")
    case = make_case(seed, CONVERSION_INDEX_BASE + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    pattern = None
    if ordinal < 3084:
        value = ordinal // 12 - 1
        locale = (0, 2)[ordinal // 6 % 2]
        pattern = CONVERSION_PATTERNS[ordinal % 6]
        table = CONVERSION_TABLE
    elif ordinal < 3132:
        at = ordinal - 3084
        value = (-1, 65)[at // 6 % 2]
        locale = (1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)[at // 12]
        pattern = CONVERSION_PATTERNS[at % 6]
        table = CONVERSION_TABLE
    elif ordinal < 3312:
        at = ordinal - 3132
        value = (-2, 256, 257, 0x80000000, 0x7FFFFFFF)[at // 36]
        locale = CONVERSION_LOCALES[at // 6 % 6]
        pattern = CONVERSION_PATTERNS[at % 6]
        table = CONVERSION_TABLE
    elif ordinal < 3384:
        at = ordinal - 3312
        value = (0, -1, -2, 256)[at % 4]
        locale = CONVERSION_LOCALES[at // 4 % 6]
        table = (0, 0xFFFFFFFF, GUEST_HI - 1)[at // 24]
    else:
        at = ordinal - 3384
        value = (0, 65, 255, -1)[at // 42 % 4]
        locale = CONVERSION_LOCALES[at // 7 % 6]
        regs[REG_NAMES.index("esi")] = (0, 1, 2, 3, 8, 0x100, 0xFFFF)[at % 7]
        offset = (-4, -8, -12, -16, 4)[at // 168]
        table = (case.esp + offset - 2 * value) & 0xFFFFFFFF
    additions = [
        (case.esp + 4, (value & 0xFFFFFFFF).to_bytes(4, "little")),
        (0x54D7F8, locale.to_bytes(4, "little")),
        (0x54D7F0, table.to_bytes(4, "little")),
    ]
    if pattern is not None:
        additions.append((CONVERSION_TABLE - 4, pattern.to_bytes(2, "little") * 260))
    return replace(case, regs=tuple(regs), patches=(*case.patches, *additions))
