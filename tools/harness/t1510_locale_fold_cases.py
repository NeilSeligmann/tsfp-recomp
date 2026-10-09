# SPDX-License-Identifier: GPL-3.0-or-later
"""T1510 amendment fixtures for 003CAD8F (locale fold); namespace 101, additive only.

The default random stream never reaches the ASCII branch of the root, because seeded guest memory
makes the locale flag [772A34] nonzero. The amendment (docs/replace-x87-t1510.md, section
3CAD8F) adds a Cartesian domain: low-16 character x high 16 bits x locale flag x table pointer.
"""

from dataclasses import replace

from .model import Case
from .seeding import GUEST_HI, SeedPolicy, make_case

NAMESPACE = 101
LABEL = "t1510-locale-fold"
ROOT = 0x3CAD8F
LOCALE_FLAG = 0x772A34
TABLE_POINTER = 0x54D7F4
TABLE = 0xD50004
# 0xC0 was added after mutation round 2 (see the doc): with the table at GUEST_HI - 0x100 it
# is the character whose byte pair is mapped at table + c but not at table + 2c
CHARACTERS = (0, 0x40, 0x41, 0x4D, 0x5A, 0x5B, 0x7F, 0xC0, 0xFF, 0x100, 0x101, 0xFFFE, 0xFFFF)
HIGH_HALVES = (0, 0xA5A50000)
FLAGS = (0, 1, 0xFFFFFFFF)
# GUEST_HI - 0x100 was added after mutation round 1 (table + c mapped, table + 2c not, c >= 0x80)
TABLES = (TABLE, 0, GUEST_HI - 1, GUEST_HI - 0x100)
COUNT = len(CHARACTERS) * len(HIGH_HALVES) * len(FLAGS) * len(TABLES)


def locale_fold_case_count(va: int) -> int:
    return COUNT if va == ROOT else 0


def locale_fold_parameters(ordinal: int) -> tuple[int, int, int]:
    """(argument dword, locale flag, table pointer) of one ordinal."""
    if type(ordinal) is not int or not 0 <= ordinal < COUNT:
        raise ValueError("ordinal outside the frozen locale-fold domain")
    character = CHARACTERS[ordinal % len(CHARACTERS)]
    high = HIGH_HALVES[ordinal // len(CHARACTERS) % len(HIGH_HALVES)]
    rest = ordinal // (len(CHARACTERS) * len(HIGH_HALVES))
    return high | character, FLAGS[rest % len(FLAGS)], TABLES[rest // len(FLAGS)]


def make_locale_fold_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != ROOT:
        raise ValueError("unsupported locale-fold root")
    argument, flag, table = locale_fold_parameters(ordinal)
    case = make_case(seed, (NAMESPACE << 40) + ordinal, va, size, policy=policy)
    additions = [
        (case.esp + 4, argument.to_bytes(4, "little")),
        (LOCALE_FLAG, flag.to_bytes(4, "little")),
        (TABLE_POINTER, table.to_bytes(4, "little")),
    ]
    return replace(case, patches=(*case.patches, *additions))
