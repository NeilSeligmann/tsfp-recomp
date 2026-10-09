# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace74: 272430 nested pointer banks and 220900/220960 relation tables.

Layouts and immutable root pair-table pins are predeclared in
 docs/evidence/t1478/pointer-table-domains.md. Normal-bank IDs only: the extended
lookup closure is authenticated but is not claimed covered by these domains.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 74
SUPPORTED_VAS = frozenset({0x272430, 0x220900, 0x220960})
_VALUES = (0, 1, 0x80000000, 0xFFFFFFFF)
_NESTED_GRID = tuple(
    (mode, outer, inner, target, value)
    for mode in (1, 2)
    for outer in range(4)
    for inner in range(4)
    for target in (None, *product(range(outer), range(inner)))
    for value in _VALUES
) + tuple((mode, 0, 0, None, value) for mode in (0, 3, 0xFFFFFFFF) for value in _VALUES)
_NESTED_CONTROLS = 5
# Original data at 509720..509778 and 509778..5097A0, not invented endpoint IDs.
_ROOT_PAIRS = {
    0x220900: (
        (1172, 1305),
        (1172, 1307),
        (1171, 1307),
        (1140, 1307),
        (1140, 1308),
        (1175, 1308),
        (1175, 1309),
        (1174, 1310),
        (1062, 1310),
        (1062, 1311),
        (1211, 1212),
    ),
    0x220960: ((1034, 1217), (1046, 1158), (1005, 1006), (1024, 1291), (1186, 1187)),
}
_LINK_MODES = (
    "none",
    "primary",
    "secondary",
    "mixed",
    "flags-missing",
    "reverse",
    "inactive",
    "missing-key",
)
_LINK_GRID = tuple(product(_LINK_MODES, (0, 2), (0x10000, 0x10001, 0xFFFFFFFF), range(4)))
_LINK_CONTROLS = 4


def nested_link_case_count(va: int) -> int:
    if va == 0x272430:
        return len(_NESTED_GRID) + _NESTED_CONTROLS
    return len(_LINK_GRID) + _LINK_CONTROLS if va in _ROOT_PAIRS else 0


def make_nested_link_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < nested_link_case_count(va):
        raise ValueError("unsupported T1478 nested/link case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    if va == 0x272430:
        _nested(fixture, ordinal)
    else:
        _links(fixture, ordinal, va)
    return fixture.finish()


def _nested(fixture: PointerFixture, ordinal: int) -> None:
    control = ordinal - len(_NESTED_GRID)
    mode, outer, inner, target, value = (
        _NESTED_GRID[ordinal] if control < 0 else (1, 1, 1, (0, 0), 0x12345678)
    )
    obj, bank = 0xD90000, 0xDA0000
    fixture.word(obj + 0x84, mode)
    fixture.word(0x74C3B4, 0x90000000 if control == 1 else bank)
    fixture.word(bank + 0x960, 0xFFFFFFFF if control == 3 else outer)
    fixture.word(bank + 0x964, outer)
    for which in (1, 2):
        for group in range(3):
            row = bank + (0 if which == 1 else 0x4B0) + group * 0x12C
            fixture.word(
                row + 8,
                value if mode == which and target is not None and group == target[0] else ~value,
            )
            fixture.half(row + 0xE, 0xFFFF if control == 4 else inner)
            for item in range(3):
                fixture.word(
                    row + 0x14 + item * 0x14,
                    obj
                    if mode == which and target == (group, item)
                    else obj + 0x1000 + group * 16 + item * 4,
                )
    if control == 2:
        # Caller pushes its object argument over the count; the first item still matches.
        fixture.move_frame(bank + 0x964)
    fixture.word(fixture.esp + 4, 0x90000000 if control == 0 else obj)


def _links(fixture: PointerFixture, ordinal: int, va: int) -> None:
    control = ordinal - len(_LINK_GRID)
    mode, offset, flags, _repeat = (
        _LINK_GRID[ordinal] if control < 0 else ("primary", 0, 0x10000, 0)
    )
    pairs = _ROOT_PAIRS[va]
    keys = sorted({key for pair in pairs for key in pair})
    if mode == "missing-key":
        keys.remove(pairs[0][0])
    identifiers = {key: index for index, key in enumerate(keys)}
    bank, pointer_table, record, entries = 0xDA0000, 0xDC0000, 0xDD0000, 0xDE0000
    fixture.word(0x715F2C, 0x90000000 if control == 0 else bank)
    fixture.word(0x715F30, 0x90000000 if control == 2 else entries)
    fixture.word(0x7B6664, 0xFFFFFFFF if control == 3 else len(keys))
    fixture.word(0x7B666C, 1)
    fixture.word(0x7B6770, 0x90000000 if control == 1 else pointer_table)
    fixture.word(pointer_table, record)
    # Missing IDs become bank-32 in the root: a mapped, explicitly inactive guard row.
    fixture.word(bank - 32, 0xFFFFFFFF)
    fixture.word(bank - 28, 0)
    for index, key in enumerate(keys):
        fixture.word(bank + index * 32, key)
        fixture.word(bank + index * 32 + 4, 0 if mode == "inactive" else 1)
        fixture.half(bank + index * 32 + 8, 0)
    for index, (left, right) in enumerate(pairs):
        a, b = identifiers.get(left, -1), identifiers.get(right, -1)
        if mode == "reverse":
            a, b = b, a
        fixture.word(entries + index * 20, flags & ~0x10000 if mode == "flags-missing" else flags)
        fixture.word(entries + index * 20 + 4, a)
        fixture.word(entries + index * 20 + 8, b)
    if mode == "none":
        primary, secondary = (), ()
    elif mode == "secondary":
        primary, secondary = (), tuple(range(len(pairs)))
    elif mode == "mixed":
        primary, secondary = tuple(range(0, len(pairs), 2)), tuple(range(1, len(pairs), 2))
    else:
        primary, secondary = tuple(range(len(pairs))), ()
    fixture.word(record, offset)
    fixture.word(record + 4, len(primary))
    fixture.word(record + 8, len(secondary))
    for index in range(offset):
        fixture.word(record + 0x18 + index * 4, 0xFFFFFFFF)
    for index, entry in enumerate((*primary, *secondary)):
        fixture.word(record + 0x18 + (offset + index) * 4, entry)
