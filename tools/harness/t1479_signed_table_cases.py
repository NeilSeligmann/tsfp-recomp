# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace 79 signed-index pointer tables, stores and byte extensions.

Pre-outcome contract: docs/evidence/t1479/pointer-list-domains.md.
"""

from itertools import product

from . import t1535_menu_index_cases as menu_index
from . import t1538_hud_slot_cases as hud_slot
from .model import Case
from .seeding import SeedPolicy
from .t1478_pointer_fixture import PointerFixture

NAMESPACE = 79
SUPPORTED_VAS = frozenset({0x338150, 0x2D2C10, 0x2D2C40, menu_index.ROOT, hud_slot.ROOT})
_STORE = tuple(
    product((-2, -1, 0, 1, 7, 0x40000000), (0, 0xFFFFFFFF, 0x12345678), (0, 0xFFFFFFFF, 0x12345678))
)
_READ = tuple(
    product(
        (0, 6, 7, 0xFFFFFFFF),
        (0, 0x7F, 0x80, 0xFF),
        (0, 1, 0x80000000, 0xFFFFFFFF),
        (-1, 0, 20, 21, 22, 0x40000000, 0x80000000),
    )
)


def signed_table_case_count(va: int) -> int:
    if va == hud_slot.ROOT:
        return hud_slot.hud_slot_case_count(va)
    if va == menu_index.ROOT:
        return menu_index.menu_index_case_count(va)
    if va == 0x338150:
        return len(_STORE) + 4
    return len(_READ) + 4 if va in SUPPORTED_VAS else 0


def make_signed_table_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va == hud_slot.ROOT:
        return hud_slot.make_hud_slot_case(seed, ordinal, va, size, policy=policy)
    if va == menu_index.ROOT:
        return menu_index.make_menu_index_case(seed, ordinal, va, size, policy=policy)
    if not 0 <= ordinal < signed_table_case_count(va):
        raise ValueError("unsupported T1479 signed-table case")
    fixture = PointerFixture(NAMESPACE, seed, ordinal, va, size, policy)
    if va == 0x338150:
        _store(fixture, ordinal)
    else:
        _read(fixture, ordinal)
    return fixture.finish()


def _store(fixture: PointerFixture, ordinal: int) -> None:
    control = ordinal - len(_STORE)
    index, before, global_before = _STORE[ordinal] if control < 0 else (0, 0x12345678, 0)
    base = 0xD90000
    if control == 0:
        base = 0x90000000
    elif control == 1:
        index = 0x10000000
    elif control == 2:
        base = fixture.esp - 4 - 12
    elif control == 3:
        base = fixture.esp + 4 - 12
    if index >= 0 and control not in (0, 1):
        fixture.word((base + index * 20 + 12) & 0xFFFFFFFF, before)
    fixture.word(0x4CA9B4, base)
    fixture.word(0x765E4C, global_before)
    fixture.word(fixture.esp + 4, index)


def _read(fixture: PointerFixture, ordinal: int) -> None:
    control = ordinal - len(_READ)
    mode, byte, value, index = _READ[ordinal] if control < 0 else (6, 0x80, 0x12345678, 0)
    if control == 0:
        index = 0x10000000
    signed = index if index < 0x80000000 else index - 0x100000000
    table = 0x510788 if signed < 21 else 0x513D04
    address = (table + index * 4) & 0xFFFFFFFF
    pointer = 0 if control == 1 else 0x90000000 if control == 2 else 0xD90000
    if control != 0:
        fixture.word(address, pointer)
    if control == 3:
        fixture.move_frame(address + 4)
    fixture.word(0xD90004, value)
    fixture.word(0xD90008, value ^ 0xFFFFFFFF)
    fixture.word(0x78AD2C, mode)
    fixture.word(0x78AD40, index)
    fixture.byte(0x78ADAE, byte)
    fixture.byte(0x78ADAC, byte)
