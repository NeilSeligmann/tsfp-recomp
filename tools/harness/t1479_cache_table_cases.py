# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace 78 mutable lookup-cache and nested option-record domains.

Pre-outcome contract: docs/evidence/t1479/pointer-list-domains.md.
"""

from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 78
SUPPORTED_VAS = frozenset({0x350DC0})
_TARGETS = tuple((length, target) for length in (0, 1, 3) for target in (None, *range(length)))
_SCAN = tuple(product(range(4), (None, 0, 2), _TARGETS, (0, 1, 5), (0, 1), (0, 255)))
_FALLBACK = tuple(product((1, 10), (0, 0x80000), (-1, 0, 2, 0x7FFF), (-1, 0, 3), (0, 5, 255)))


def cache_table_case_count(va: int) -> int:
    return len(_SCAN) + len(_FALLBACK) + 7 if va in SUPPORTED_VAS else 0


def make_cache_table_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < cache_table_case_count(va):
        raise ValueError("unsupported T1479 cache/table case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    control = ordinal - len(_SCAN) - len(_FALLBACK)
    cache, lookup, length, target, selector, level, byte = 0, 0, 1, 0, 0, 0, 0
    mode, flags, tag, index, dynamic, key = 10, 0, 2, 0, 5, 7
    if ordinal < len(_SCAN):
        cache, lookup, (length, target), selector, level, byte = _SCAN[ordinal]
    elif control < 0:
        mode, flags, tag, index, dynamic = _FALLBACK[ordinal - len(_SCAN)]
        length, target = 0, None
    obj, descriptor, alternate = 0xD90000, 0xDA0000, 0xDA0100
    if control == 6:
        # Root pushes saved EDI over flags, then pushes key over the mode byte.
        fixture.move_frame(0x7DE464)
        key, cache, lookup = 0x101, 3, 0
    fixture.word(
        fixture.esp + 4,
        (0, 0x90000000, obj)[control] if 0 <= control < 3 else 0x90000000 if control == 3 else obj,
    )
    fixture.word(obj + 8, index)
    fixture.half(obj + 0x48, tag)
    fixture.byte(0x7DE455, mode)
    fixture.byte(0x7DE457, selector)
    fixture.word(0x7DE458, 0x40 if 0 <= control < 3 else flags)
    fixture.word(0x79094C, key)
    fixture.word(0x7DE494, 0)
    fixture.word(0x4D2CE4, key if cache in (0, 2) else key + 1)
    fixture.word(0x4D2CE8, mode if cache in (0, 1) else mode + 1)
    fixture.word(0x6F32F0, 0x90000000 if control == 4 else descriptor if cache == 0 else alternate)
    fixture.word(descriptor + 8, 0x1000000 if control == 5 else level)
    fixture.word(alternate + 8, level ^ 1)
    for item in range(3):
        row = 0x515FFC + item * 0x80
        fixture.half(row, 1)
        fixture.word(row + 4, key if item == lookup else key + 100 + item)
        fixture.word(row + 0x20, descriptor)
        fixture.word(row + 0x24, alternate)
    fixture.half(0x515FFC + 3 * 0x80, 0)
    for row_index in (0, 1):
        row = 0x53D2D0 + 0xA4 * row_index
        fixture.word(row, length if row_index == level else 0)
        for item in range(length):
            fixture.half(row + 4 + 8 * item, tag if item == target else tag ^ 0x4000)
            fixture.byte(row + 6 + 8 * item + selector, byte)
    fixture.byte(0x7DE434 + index, dynamic)
    fixture.word(0x53A3DC + 0x48 * tag, (0, 1, 0xFFFFFFFF)[ordinal % 3])
    return fixture.finish()
