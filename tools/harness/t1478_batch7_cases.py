# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace100 domains for T1478's seventh call-bearing batch.

Contracts, ordinal layouts and gates: docs/evidence/t1478/seventh-domain.md, written before
any proof outcome. Domains are additive after the unchanged random, edge and feedback streams.
"""

from collections.abc import Callable
from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 100 << 40
Word = Callable[[int, int], None]
Patches = list[tuple[int, bytes]]

_ARENA = 0xD90000
_UNMAPPED = 0x90000000

# 001DE540: 341A10(ptr = [[75BC04]+4FC], 0.5f) state-3 begin.
_WEATHER_STATES = (0, 1, 2, 3, 4, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF)
_WEATHER_PTR_KINDS = ("record", "null")
_WEATHER_GRID = tuple(product(_WEATHER_PTR_KINDS, _WEATHER_STATES, (0, 0x3F800000)))
_WEATHER_REPEAT = 4
_WEATHER_CONTROLS = 2

# 0022DE00: selector [7DE470] 6/7/8 choose table 50E94C/50EA0C/50EA8C, key searched by 9CC40.
_SELECTORS = (5, 6, 7, 8, 9, 0xFFFFFFFF, 0x80000000)
_SELECT_BASES = {6: 0x50E94C, 7: 0x50EA0C, 8: 0x50EA8C}
_TABLE_KEYS = (10, 20, 30, 40)
_QUERIES = (10, 40, 25, 5, 50, 0x2329, 0xFFFFFFFF)
_COUNTS = (0, 4)
_PLAYER_ROWS = (0, 2)
_SELECT_GRID = tuple(
    (selector, row, query, count)
    for selector in _SELECTORS
    for row in _PLAYER_ROWS
    for query in (_QUERIES if selector in _SELECT_BASES else (10,))
    for count in (_COUNTS if selector in _SELECT_BASES else (4,))
)
_SELECT_REPEAT = 2
_SELECT_CONTROLS = 3


def _count(va: int) -> int:
    return {
        0x1DE540: len(_WEATHER_GRID) * _WEATHER_REPEAT + _WEATHER_CONTROLS,
        0x22DE00: len(_SELECT_GRID) * _SELECT_REPEAT + _SELECT_CONTROLS,
    }.get(va, 0)


def t1478_batch7_case_count(va: int) -> int:
    return _count(va)


def make_t1478_batch7_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < _count(va):
        raise ValueError("unsupported T1478 batch7 case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches = list(case.patches)

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    builders = {0x1DE540: _weather_case, 0x22DE00: _select_case}
    builders[va](ordinal, patches, word)
    return replace(case, patches=tuple(patches))


def _weather_case(ordinal: int, patches: Patches, word: Word) -> None:
    controls = ordinal - len(_WEATHER_GRID) * _WEATHER_REPEAT
    kind, state, duration = (
        _WEATHER_GRID[ordinal % len(_WEATHER_GRID)] if controls < 0 else ("record", 0, 0)
    )
    obj, record = _ARENA, _ARENA + 0x1000
    word(0x75BC04, _UNMAPPED if controls == 0 else obj)
    word(obj + 0x4FC, _UNMAPPED if controls == 1 else (0 if kind == "null" else record))
    word(record + 4, state)
    word(record + 8, 0x40000000)
    word(record + 0xC, duration)


def _select_case(ordinal: int, patches: Patches, word: Word) -> None:
    controls = ordinal - len(_SELECT_GRID) * _SELECT_REPEAT
    selector, row, query, count = (
        _SELECT_GRID[ordinal % len(_SELECT_GRID)] if controls < 0 else (7, 2, 20, 4)
    )
    player, rows, table = _ARENA, _ARENA + 0x1000, _ARENA + 0x2000
    row_value = 2 * row + 1
    word(0x7DE470, selector)
    word(0x7B0C7C, _UNMAPPED if controls == 0 else player)
    word(player, row)
    word(0x75CC90, _UNMAPPED if controls == 1 else rows)
    word(rows + 0x34 * row + 4, row_value)
    for base in _SELECT_BASES.values():
        word(base + (row_value << 5), query)
    word(0x715F2C, _UNMAPPED if controls == 2 else table)
    word(0x7B6664, count)
    for index, key in enumerate(_TABLE_KEYS):
        word(table + (index << 5), key)
