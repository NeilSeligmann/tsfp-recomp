# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace72: authenticated 1B6890 two-source signed-word table domains.

Predeclaration: docs/evidence/t1478/pointer-table-domains.md. No provider activation
or original outcomes are implied by constructing these fixtures.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 72
SUPPORTED_VAS = frozenset({0x1B6890})
_GRID = tuple(product((0, 1), (0, 1, 2), range(4), range(4), range(6)))
_VALUES = (
    (1, 1, -1),
    (-1, -1, 1),
    (32767, 32767, 32767),
    (-32768, -32768, -32768),
    (0, 0, 1),
    (0, 0, 0),
)
_CONTROLS = 4


def actor_source_case_count(va: int) -> int:
    return len(_GRID) + _CONTROLS if va in SUPPORTED_VAS else 0


def make_actor_source_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < actor_source_case_count(va):
        raise ValueError("unsupported T1478 actor-source case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    control = ordinal - len(_GRID)
    active, child, first, second, values = _GRID[ordinal] if control < 0 else (1, 1, 1, 1, 5)
    obj, bank = 0xD90000, 0xDA0000
    if control == 2:
        # Root's final signed-word load aliases the callee return-PC data at entry ESP-20.
        obj = fixture.esp - 0x61E - child * 2 - 20
    fixture.word(fixture.esp + 4, 0x90000000 if control == 0 else obj)
    fixture.word(fixture.esp + 8, 0)
    fixture.byte(obj + 0x54C, active)
    fixture.word(obj + 0x98, 1)
    fixture.word(0x4F9BAC, 0x90000000 if control == 1 else bank)
    fixture.word(0x4FA138, 0x10000000 if control == 3 else 2)
    fixture.word(bank + 2 * 0x29C + 0x14, child)
    for index, kind, offset in ((0, first, 0x4FA134), (1, second, 0x4FA138)):
        fixture.word(offset + 0x3C, 0xFFFFFFFF if kind == 0 else index)
        fixture.word(bank + index * 0x29C + 0x14, child if kind == 1 else child + 9)
        fixture.word(bank + index * 0x29C + 0x1C, child if kind == 2 else 0)
    a, b, c = _VALUES[values]
    fixture.half(obj + 0xDC + child * 2, a)
    fixture.half(obj + 0x20C + child * 2, b)
    fixture.half(obj + 0x61E + child * 2, c)
    return fixture.finish()
