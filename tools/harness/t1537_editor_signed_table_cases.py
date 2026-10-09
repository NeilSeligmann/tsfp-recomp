# SPDX-License-Identifier: GPL-3.0-or-later
"""Inactive namespace77 delegate for the predeclared 2FE4E0 editor table."""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 77
SUPPORTED_VAS = frozenset({0x2FE4E0})
PROFILE = 0xD00000
QUERIES = (0xFFFF8000, 0xFFFFFFFF, 0, 0x7FFF, 0x8000, 0x80000000)
SHAPES = tuple((count, target) for count in (0, 1, 3) for target in (None, *range(count)))
DOMAIN = tuple(product(SHAPES, QUERIES, (False, True)))


def editor_signed_table_case_count(va: int) -> int:
    return len(DOMAIN) + 2 if va in SUPPORTED_VAS else 0


def make_editor_signed_table_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < editor_signed_table_case_count(va):
        raise ValueError("unsupported T1537 editor signed-table case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    if ordinal >= len(DOMAIN):
        fixture.word(0x7844A8, 0 if ordinal == len(DOMAIN) else 0x90000000)
        fixture.word(fixture.esp + 4, 0xFFFFFFFF)
        return fixture.finish()
    (count, target), query, repeated = DOMAIN[ordinal]
    fixture.word(0x7844A8, PROFILE)
    fixture.half(PROFILE + 0x306E, count)
    for index in range(count):
        match = target is not None and (index == target or repeated and index > target)
        fixture.half(PROFILE + 0x307A + 12 * index, query if match else query ^ 0x4000)
    fixture.word(fixture.esp + 4, query)
    return fixture.finish()
