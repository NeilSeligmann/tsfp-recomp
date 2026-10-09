# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace73: 1BA1B0 / 1BFA60 signed predecessor and actor-bank domains.

Predeclaration: docs/evidence/t1478/pointer-table-domains.md. All loops have a
predeclared negative-byte sentinel; no outcome-dependent search is performed.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 73
SUPPORTED_VAS = frozenset({0x1BA1B0, 0x1BFA60})
_GRID = tuple(product((0, 1), (0, 1), (0, 1), (5, 2, -1), (-1, 0, 1, 2), (0, 2), (0, 3, 20)))
_CONTROLS = 4


def predecessor_case_count(va: int) -> int:
    return len(_GRID) + _CONTROLS if va in SUPPORTED_VAS else 0


def make_predecessor_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < predecessor_case_count(va):
        raise ValueError("unsupported T1478 predecessor case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    control = ordinal - len(_GRID)
    flag, object_gate, global_gate, target, index, limit, delta = (
        _GRID[ordinal] if control < 0 else (1, 1, 0, 5, 0, 2, 0)
    )
    obj, bank = 0xD90000, 0xDA0000
    fixture.word(0x7B0C48, 0x90000000 if control == 2 else bank)
    fixture.word(0x790950, limit)
    fixture.word(0x7DE458, 0x1000 if global_gate else 0)
    for row in range(-2, 9):
        fixture.byte(0x4FA128 + row * 0x3C, 0x80)
        fixture.word(0x4FA160 + row * 0x3C, delta)
    for row in range(target + 1, 6):
        fixture.byte(0x4FA128 + row * 0x3C, 0)
    fixture.byte(0x4FA128 + 6 * 0x3C, 4 if flag else 0)
    fixture.word(obj + 8, index)
    fixture.word(obj + 0x80, object_gate)
    fixture.word(obj + 0x94, 6 if va == 0x1BA1B0 else 0xABCDEFFF)
    fixture.word(bank + index * 0x1584 + 0xC, (0, 0x4000, 0xFFFFFFFF)[ordinal % 3])
    fixture.byte(bank + index * 0x1584 + delta + 0xE3C, 0x55)
    if control == 3:
        fixture.move_frame(obj + 0x94 + (8 if va == 0x1BA1B0 else 4))
    fixture.word(fixture.esp + 4, 0x90000000 if control == 0 else obj)
    fixture.word(fixture.esp + 8, 0x10000000 if control == 1 else 6)
    if control == 1 and va == 0x1BA1B0:
        fixture.word(obj + 0x94, 0x10000000)
    return fixture.finish()
