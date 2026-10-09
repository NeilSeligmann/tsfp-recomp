# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace76 freelist/copy/append and bounded tag-length stream domains.

Pre-outcome contract: docs/evidence/t1479/pointer-list-domains.md.
"""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 76
SUPPORTED_VAS = frozenset({0x3081E0, 0x32A5B0})
_COPY = tuple(product((0, 1), (0, 1), (0, 1, 3), (0, 1), (-1, 0, 1), range(3)))
_CHUNK_TARGETS = tuple((length, target) for length in range(5) for target in (None, *range(length)))
_CHUNK = tuple(product(_CHUNK_TARGETS, (0, 4, 32), range(4), (0x12345678, 0xC7766A)))
_COPY_CONTROLS = 5
_CHUNK_CONTROLS = 5


def copy_chunk_case_count(va: int) -> int:
    if va == 0x3081E0:
        return len(_COPY) + _COPY_CONTROLS
    return len(_CHUNK) + _CHUNK_CONTROLS if va == 0x32A5B0 else 0


def make_copy_chunk_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < copy_chunk_case_count(va):
        raise ValueError("unsupported T1479 copy/chunk case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    df = _copy(fixture, ordinal) if va == 0x3081E0 else 0
    if va == 0x32A5B0:
        _chunk(fixture, ordinal)
    return replace(fixture.finish(), df=df)


def _copy(fixture: PointerFixture, ordinal: int) -> int:
    control = ordinal - len(_COPY)
    free, source_present, length, df, value, alias = (
        _COPY[ordinal] if control < 0 else (1, 1, 1, 0, 1, 0)
    )
    source, owner, next_free, existing = 0xDB0000 + df * 56, 0xD00000, 0xD98000, 0xDD0000
    node = 0xD90000 if alias == 0 else source + (4 if alias == 2 else 0)
    if control == 4:
        owner = 0x762CC8 - 0x3C
    for offset in range(-64, 132, 4):
        fixture.word(source + offset, 0xA0000000 + offset)
    fixture.word(source + 8, value)
    fixture.word(node + 0x14, next_free)
    fixture.word(next_free + 0x14, 0)
    fixture.word(0x762CC8, 0x90000000 if control == 1 else node if free else 0)
    fixture.word(0x762CC0, (0, 1, 0xFFFFFFFF)[ordinal % 3])
    for index in range(length):
        item = existing + index * 0x100
        fixture.word(item + 0x14, existing + (index + 1) * 0x100 if index + 1 < length else 0)
    if control != 4:
        fixture.word(owner + 0x3C, existing if length else 0)
    if control == 3:
        fixture.word(existing + 0x14, existing)
    fixture.word(fixture.esp + 4, 0x90000000 if control == 2 else owner)
    fixture.word(fixture.esp + 8, (0, 0x12345678, 0xFFFFFFFF)[ordinal % 3])
    fixture.word(fixture.esp + 12, (0, 0x7FFF, 0x8000, 0xFFFF)[ordinal % 4])
    fixture.word(fixture.esp + 16, 0x90000000 if control == 0 else source if source_present else 0)
    return df


def _chunk(fixture: PointerFixture, ordinal: int) -> None:
    control = ordinal - len(_CHUNK)
    (length, target), payload, header, key = (
        _CHUNK[ordinal] if control < 0 else ((2, 1), 4, 0, 0x12345678)
    )
    base = 0xD90000
    fixture.word(fixture.esp + 4, 0x90000000 if control == 0 else base)
    fixture.word(fixture.esp + 8, key)
    fixture.word(base, 0xC77667 if header in (0, 2) else 0)
    fixture.word(base + 4, 0x12D if header in (0, 1) else 0)
    cursor = base + 8
    for index in range(length):
        fixture.word(cursor, key if index == target else 0x11111111)
        fixture.word(cursor + 4, payload)
        for offset in range(0, payload, 4):
            fixture.word(cursor + 8 + offset, 0xBAD00000 + index * 64 + offset)
        cursor += 8 + payload
    fixture.word(cursor, 0xC7766A)
    fixture.word(cursor + 4, 0)
    if control == 1:
        fixture.word(base + 8, 0x11111111)
        fixture.word(base + 12, 0x7FD0)
    elif control == 2:
        # -8 rewinds the next-record pointer to itself: COUNT-LIMIT must remain no-verdict.
        fixture.word(base + 8, 0x11111111)
        fixture.word(base + 12, 0xFFFFFFF8)
    elif control == 3:
        fixture.word(base + 8, 0x11111111)
        fixture.word(base + 12, 1)
        fixture.word(base + 17, key)
        fixture.word(base + 21, 0)
    elif control == 4:
        fixture.move_frame(base + 12)
        fixture.word(fixture.esp + 4, base)
        fixture.word(fixture.esp + 8, key)
