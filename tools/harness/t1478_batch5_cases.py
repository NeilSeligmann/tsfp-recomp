# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace46 domains for T1478's fifth call-bearing batch.

Contracts, ordinal layouts and gates: docs/evidence/t1478/fifth-domain.md, written and
committed before any proof outcome. Domains are additive after the unchanged random, edge
and feedback streams.
"""

from collections.abc import Callable
from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case
from .t1478_batch4_cases import _TABLE_BASE, _write_record

_INDEX_BASE = 46 << 40
Word = Callable[[int, int], None]
Move = Callable[[int, int], None]
Patches = list[tuple[int, bytes]]

# 00235600 (key lookup through 0015BA10, then flag update on the record's object).
_KEY = 0x47415332
_OTHER_KEY = 0x11111111
_FIELD14 = (0, 1, 0x200, 0x1000, 0x1201, 0xFFFFFFFF)
_FIELD28 = (0, 0x20000000)
_OBJECT_BASE = 0xDA0000
_HANDLE_BASE = 0xD80000
_WINDOWS = tuple(
    (start, count, target) for start in (0, 2) for count in range(4) for target in range(count + 1)
)
_LOOKUP = tuple(
    (mode, window, skip, first14, first28)
    for mode in (0x0A, 0x01)
    for window in _WINDOWS
    for skip in range(3)
    for first14 in range(len(_FIELD14))
    for first28 in range(len(_FIELD28))
)
_LOOKUP_CONTROLS = 4

# 00208AC0 (record 4 type byte through 00227400 and 002273C0).
_TYPE_BYTES = (0, 1, 2, 3, 4, 5, 6, 0x7F, 0x80, 0xFF)
_TYPE_BASES = (0xD50000, 0xD50001)
_TYPE_CONTROLS = 3
# Amendment A1 (written AFTER the default domain gave only 22 verdicts, see fifth-domain.md):
# the 20 grid combinations repeated six times with distinct register/frame draws, appended
# after the unchanged ordinals.
_TYPE_AMEND_CASES = 120

# 0025EF30 (bit test in a per-index record, override flag, constant 35DE60).
_BIT_ARGS = (0, 1, 5, 31, 32, 33, 0x80000003, 0xFFFFFFFF)
_BIT_INDICES = (0, 1, 3)
_BIT_WORDS = (0, 1, 2)  # 0, exactly the tested bit, everything but the tested bit
_BIT_FLAGS = (0, 0x10000, 0xFFFEFFFF)
_BIT_GRID = tuple(product(_BIT_INDICES, _BIT_ARGS, _BIT_WORDS, _BIT_FLAGS))
_BIT_CONTROLS = 3

# 001CD380 (four arguments: bit, index, direct flag, row) and 001CD2B0 (bit number, slot).
_PROFILE = 0xD60000
_WORD_PAIRS = ((0, 0), (1, 0), (0, 1), (2, 2), (1, 1))
_ROW_BITS = (0, 5, 31, 32, 0xFFFFFFFF)
_ROW_INDICES = (0, 1, 7)
_ROW_FLAGS = (0, 1, 0x80000000)
_ROW_ROWS = (0xFFFFFF01, 0, 1, 0x20)
_ROW_GRID = tuple(product(_ROW_BITS, _ROW_INDICES, _ROW_FLAGS, _ROW_ROWS, _WORD_PAIRS))
_ROW_CONTROLS = 3
_MASK_BITS = (0, 1, 31, 32, 33, 63, 64, 0xFFFFFFFF, 0xFFFFFFE0, 0xFFFFFFDF)
_MASK_SLOTS = (0xFFFFFFFF, 0, 1, 2, 0xFFFFFFFE)
_MASK_WORDS = (0, 1, 2)
_SCAN_HITS = (0, 1, 2, 3, 4)
_MASK_GRID = tuple(
    (bit, slot, word_kind, hit)
    for bit in _MASK_BITS
    for slot in _MASK_SLOTS
    for word_kind in _MASK_WORDS
    for hit in (_SCAN_HITS if slot == 0xFFFFFFFF else (0,))
)
_MASK_CONTROLS = 3

# 001CD590 (set one bit in every record's word at +0xBF0).
_LOOP_COUNTS = (0, 1, 2, 3)
_LOOP_SHIFTS = (0, 5, 31, 32, 0x80000003)
_LOOP_WORDS = (0, 0xFFFFFFFF, 0x80000000)
_LOOP_GRID = tuple(
    (count, override, present, word_kind, shift)
    for count in _LOOP_COUNTS
    for override in (0, 1)
    for present in range(8)
    for word_kind in range(len(_LOOP_WORDS))
    for shift in _LOOP_SHIFTS
)
_LOOP_CONTROLS = 4


def _count(va: int) -> int:
    return {
        0x235600: len(_LOOKUP) + _LOOKUP_CONTROLS,
        0x208AC0: len(_TYPE_BYTES) * len(_TYPE_BASES) + _TYPE_CONTROLS + _TYPE_AMEND_CASES,
        0x25EF30: len(_BIT_GRID) + _BIT_CONTROLS,
        0x1CD380: len(_ROW_GRID) + _ROW_CONTROLS,
        0x1CD2B0: len(_MASK_GRID) + _MASK_CONTROLS,
        0x1CD590: len(_LOOP_GRID) + _LOOP_CONTROLS,
    }.get(va, 0)


def t1478_batch5_case_count(va: int) -> int:
    return _count(va)


def make_t1478_batch5_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < _count(va):
        raise ValueError("unsupported T1478 batch5 case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)
    frame = next(data for address, data in case.patches if address == case.esp)

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    def move_entry(new_esp: int, length: int) -> None:
        patches.append((new_esp, frame[:length]))
        regs[4] = new_esp

    builders = {
        0x235600: _lookup_case,
        0x208AC0: _type_case,
        0x25EF30: _bit_case,
        0x1CD380: _row_case,
        0x1CD2B0: _mask_case,
        0x1CD590: _loop_case,
    }
    builders[va](ordinal, case.esp, patches, word, move_entry, regs)
    return replace(case, patches=tuple(patches), regs=tuple(regs))


def _lookup_case(
    ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move, regs: list[int]
) -> None:
    word(0x7356D8, _TABLE_BASE)
    if ordinal < len(_LOOKUP):
        mode, (start, count, target), skip, first14, first28 = _LOOKUP[ordinal]
        specs = [
            (_OTHER_KEY, 0) if position < target else (_KEY, skip if position == target else 0)
            for position in range(count)
        ]
    else:
        control = ordinal - len(_LOOKUP)
        mode, start, count, first14, first28 = 0x0A, 1, 2, 4, 1
        specs = [(_KEY, 0), (_KEY, 0)]
        if control == 0:
            word(0x7356D8, 0x90000000)
        elif control == 1:
            esp = 0x790954
            move_entry(esp, 4)
        elif control == 2:
            start = 0xFFFFFFFF
        else:
            start, count, specs = 0, 0xFFFFFFFF, []
    signed_start = start - (1 << 32) if start >= 1 << 31 else start
    word(esp + 4, _KEY)
    word(0x790950, start)
    word(0x7A2958, count)
    patches.append((0x7DE455, bytes([mode])))
    records = [(signed_start - 1, _KEY, 0)] if signed_start > 0 else []
    records += [(signed_start + index, key, skip) for index, (key, skip) in enumerate(specs)]
    records.append((signed_start + len(specs), _KEY, 0))
    for index, (absolute, key, skip) in enumerate(records):
        _write_record(word, absolute, mode, key, skip, 0x4C)
        handle = _HANDLE_BASE + (absolute & 0xFF) * 0x100
        obj = _OBJECT_BASE + (absolute & 0xFF) * 0x100
        word(handle + 0x7C, obj)
        word(obj + 0x14, _FIELD14[(first14 + index) % len(_FIELD14)])
        word(obj + 0x28, _FIELD28[(first28 + index) % len(_FIELD28)])


def _type_case(
    ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move, regs: list[int]
) -> None:
    grid = len(_TYPE_BYTES) * len(_TYPE_BASES)
    controls = ordinal - grid
    if controls >= _TYPE_CONTROLS:
        ordinal, controls = (controls - _TYPE_CONTROLS) % grid, -1
    base_index, value_index = divmod(ordinal, len(_TYPE_BYTES))
    base = 0xD50000
    value = 4
    if controls < 0:
        base = _TYPE_BASES[base_index]
        value = _TYPE_BYTES[value_index]
    record = base + 4 * 0x4C + 0xB
    word(0x78CECC, 0x90000000 if controls == 0 else base)
    for neighbor in (3, 5):
        patches.append((base + neighbor * 0x4C + 0xB, bytes([5 if value != 5 else 2])))
    patches.append((record, bytes([value])))
    if controls == 1:
        move_entry(record + 4, 4)
    elif controls == 2:
        move_entry(record - 3 + 4, 4)


def _bit_case(
    ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move, regs: list[int]
) -> None:
    index, argument, word_kind, flags = (
        _BIT_GRID[ordinal] if ordinal < len(_BIT_GRID) else (1, 5, 1, 0)
    )
    control = ordinal - len(_BIT_GRID)
    mask = 1 << (argument & 31)
    cell = 0xD70000 + index * 0x238 + 4
    word(0x7B0C7C, 0xD60000)
    word(0xD60000, index)
    word(0x75F5D0, 0xD70000)
    word(cell, (0, mask, ~mask)[word_kind])
    word(0x7DE45C, flags)
    word(esp + 4, argument)
    if control == 0:
        word(0x7B0C7C, 0x90000000)
    elif control == 1:
        word(0x75F5D0, 0x90000000)
    elif control == 2:
        # The argument slot IS the tested record word (bit 5 is set in it).
        move_entry(cell - 4, 4)
        word(cell, 0x20)
        word(0xD60000, index)


def _row_case(
    ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move, regs: list[int]
) -> None:
    control = ordinal - len(_ROW_GRID)
    if control < 0:
        bit, index, flag, row, (direct, indirect) = _ROW_GRID[ordinal]
    else:
        bit, index, flag, row, (direct, indirect) = 5, 1, control % 2, 1, (0, 1)
    mask = 1 << (bit & 31)
    kinds = (0, mask, ~mask)
    word(0x6F1E0C, 0x90000000 if control == 0 or control == 1 else _PROFILE)
    word(_PROFILE + 0xC00 + 4 * index, kinds[direct])
    word(_PROFILE + 4 * (index + 3 * ((row + 0xFF) & 0xFFFFFFFF)), kinds[indirect])
    arguments = (bit, index, flag, row)
    for slot, value in enumerate(arguments):
        word(esp + 4 + 4 * slot, value)
    if control == 2:
        # Saved ESI overwrites the direct-path word before it is tested.
        flag = 0
        target = _PROFILE + 0xC00 + 4 * index
        move_entry(target + 4, 20)
        for slot, value in enumerate((bit, index, 0, row)):
            word(target + 8 + 4 * slot, value)
        word(0x6F1E0C, _PROFILE)


def _mask_case(
    ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move, regs: list[int]
) -> None:
    control = ordinal - len(_MASK_GRID)
    if control < 0:
        bit, slot, word_kind, hit = _MASK_GRID[ordinal]
    else:
        bit, slot, word_kind, hit = 5, (0xFFFFFFFF, 1, 0xFFFFFFFF)[control], 1, 2
    signed_bit = bit - (1 << 32) if bit >= 1 << 31 else bit
    row = signed_bit >> 5
    reduced = signed_bit - (row << 5)
    mask = 1 << (reduced & 31)
    kinds = (0, mask, ~mask)
    word(0x6F1E0C, 0x90000000 if control in (0, 1) else _PROFILE)
    if slot == 0xFFFFFFFF:
        for step in range(1, 5):
            address = _PROFILE + ((row * 4 + 0xC0C + 0x10 * (step - 1)) & 0xFFFFFFFF)
            word(address, kinds[word_kind] if step == hit else 0)
    else:
        signed_slot = slot - (1 << 32) if slot >= 1 << 31 else slot
        word(_PROFILE + (((row + signed_slot * 4) * 4 + 0xBFC) & 0xFFFFFFFF), kinds[word_kind])
    word(esp + 4, bit)
    word(esp + 8, slot)
    if control == 2:
        # Saved EBX overwrites the first scanned word before it is tested.
        move_entry(_PROFILE + 0xC0C + 4, 12)
        word(_PROFILE + 0xC0C + 8, bit)
        word(_PROFILE + 0xC0C + 12, slot)


def _loop_case(
    ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move, regs: list[int]
) -> None:
    control = ordinal - len(_LOOP_GRID)
    if control < 0:
        count, override, present, word_kind, shift = _LOOP_GRID[ordinal]
    else:
        count, override, present, word_kind, shift = (
            (0x80000000, 0, 0, 0, 3),
            (0xFFFFFFFF, 0, 0, 0, 3),
            (2, 0, 7, 1, 3),
            (3, 0, 7, 0, 3),
        )[control]
    word(0x5236C4, count)
    word(0x7DE458, 0x800000 if override else 0)
    word(esp + 4, shift)
    for index in range(3):
        base = 0x7BEDC0 + index * 0x1F54
        word(base, 1 if present >> index & 1 else 0)
        record = 0x7C0050 + index * 0x1F54 if present >> index & 1 else 0x6F11C0
        word(record + 0xBF0, _LOOP_WORDS[word_kind])
    if control == 2:
        # Pushed loop index lands on the loop-count global: ESP-12 == 0x5236C4.
        move_entry(0x5236D0, 4)
        word(0x5236D0 + 4, shift)
