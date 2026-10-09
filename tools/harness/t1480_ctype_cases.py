# SPDX-License-Identifier: GPL-3.0-or-later
"""T1480 predeclared classifier domains, additive to the original seed stream."""

from dataclasses import replace

from .model import Case
from .seeding import GUEST_HI, SeedPolicy, make_case

CTYPE_INDEX_BASE = 34 << 40
CTYPE_CASES = 1728
CTYPE_ROOTS = {0x3C887E: 46, 0x3C88AC: 41, 0x3C88D5: 41, 0x3C88FE: 46, 0x3C892C: 46}
CTYPE_LOCALES = (0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)
CTYPE_PATTERNS = (0, 0xFFFF, 0x100)
CTYPE_TABLE = 0xD50004


def ctype_case_count(va: int) -> int:
    return CTYPE_CASES if va in CTYPE_ROOTS else 0


def ctype_parameters(ordinal: int) -> tuple[int, int, int, int | None]:
    """Return character, locale, table pointer, optional uniform table word."""
    if not 0 <= ordinal < CTYPE_CASES:
        raise ValueError("unsupported T1480 classifier ordinal")
    if ordinal < 1542:
        value = ordinal // 6 - 1
        locale = (0, 2)[ordinal // 3 % 2]
        pattern = CTYPE_PATTERNS[ordinal % 3]
    elif ordinal < 1566:
        at = ordinal - 1542
        value = (-1, 65)[at // 3 % 2]
        locale = (1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)[at // 6]
        pattern = CTYPE_PATTERNS[at % 3]
    elif ordinal < 1656:
        at = ordinal - 1566
        value = (-2, 256, 257, 0x80000000, 0x7FFFFFFF)[at // 18]
        locale = CTYPE_LOCALES[at // 3 % 6]
        pattern = CTYPE_PATTERNS[at % 3]
    else:
        at = ordinal - 1656
        return (
            (0, -1, -2, 256)[at % 4] & 0xFFFFFFFF,
            CTYPE_LOCALES[at // 4 % 6],
            (0, 0xFFFFFFFF, GUEST_HI - 1)[at // 24],
            None,
        )
    return value & 0xFFFFFFFF, locale, CTYPE_TABLE, pattern


def make_ctype_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not ctype_case_count(va):
        raise ValueError("unsupported T1480 classifier address")
    value, locale, table, pattern = ctype_parameters(ordinal)
    case = make_case(seed, CTYPE_INDEX_BASE + ordinal, va, size, policy=policy)
    additions = [
        (case.esp + 4, value.to_bytes(4, "little")),
        (0x54D7F8, locale.to_bytes(4, "little")),
        (0x54D7F0, table.to_bytes(4, "little")),
    ]
    if pattern is not None:
        additions.append((CTYPE_TABLE - 4, pattern.to_bytes(2, "little") * 260))
    return replace(case, patches=(*case.patches, *additions))
