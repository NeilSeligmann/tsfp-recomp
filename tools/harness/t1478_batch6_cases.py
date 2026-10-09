# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace48 domains for T1478's sixth call-bearing batch.

Contracts, ordinal layouts and gates: docs/evidence/t1478/sixth-domain.md, written and
committed before any proof outcome. Domains are additive after the unchanged random, edge
and feedback streams. Grids marked "x2" or "x6" repeat the same grid with distinct
register/frame draws (the case index, not the grid cell, drives the draw).
"""

from collections.abc import Callable
from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 48 << 40
Word = Callable[[int, int], None]
Move = Callable[[int, int], None]
Patches = list[tuple[int, bytes]]

_KEY = 0x47414D45
_OTHER = 0x11111111
_ARENA = 0xD90000

# 00247A10: reverse scan of 0x5C-byte records through 00156E50 / 00156E80.
_SCAN_PAIRS = tuple(
    (count, target) for count in range(5) for target in (None, *range(count))
)  # 15 pairs
_SCAN_GRID = tuple(product(_SCAN_PAIRS, range(4)))  # skip kind 0 none, 1 flag, 2 pointer, 3 key
_SCAN_REPEAT = 2
_SCAN_CONTROLS = 2

# 00248910: gate on [[arg+150]+114], then a four-record scan of 0x5C-byte records.
_GATE_GRID = (
    *(("gate", gate, None, 0, 0) for gate in (0x1001, 0)),
    *(
        ("scan", 0x1000, target, flag, value)
        for target in (None, 0, 1, 2, 3)
        for flag in (0, 1)
        for value in (0, 0x12345678)
    ),
)  # 22 cells
_GATE_REPEAT = 6
_GATE_CONTROLS = 3

# Table lookup 0009A4C0 as seen by 002346A0 and 002291E0.
_IDS = (0xFFFFFFFF, 0, 5, 0x1F3F, 0x1F40, 0x1F41, 0x209D, 0x209E, 0x20B1, 0x20B2)
_T1 = 0xDB0000
_T2 = 0xDC0000
_ID_GRID = tuple(product(_IDS, range(5), range(2)))  # id, matching field (0 none), filler kind
_ID_REPEAT = 2
_ID_CONTROLS = 3

# 002291E0 domains.
_A_GRID = tuple(
    product((0, 1, 3), (0, 2, 0x80000000), range(5), (0, 3))
)  # p50, multiplier, M relation, index
_B_IDS = (0xFFFFFFFF, 0, 5, 0x1F40, 0x209E)
_B_GRID = tuple(product((0, 0x3F, 0x80), _B_IDS, range(2), range(2)))
_ROW_CONTROLS = 3

# 001B7E80: record 0x29C bytes, mode byte, RNG callee on the other modes.
_RECORD_INDICES = (0xFFFFFFFF, 0, 1, 3)
_RECORD_MODES = (0x0A, 0x00, 0x0B, 0xFF)
_RECORD_FIELDS = (0x7FFFFFFF, 1, 0, 0xFFFFFFFF, 0x80000000)
_RECORD_FLAGS = (0, 2, 0xFFFFFFFD, 3)
_RECORD_GRID = tuple(product(_RECORD_INDICES, _RECORD_MODES, _RECORD_FIELDS, _RECORD_FLAGS))
_RECORD_CONTROLS = 2

# 0025F320: object -> child -> block gate 0x163830, then 0025FA10 key search.
_LINK_GATES = (0, 0x163830, 0x163831)
_LINK_PAIRS = tuple((count, target) for count in range(4) for target in (None, *range(count)))
_LINK_GRID = tuple(product(_LINK_GATES, _LINK_PAIRS, range(3)))
_LINK_REPEAT = 2
_LINK_CONTROLS = 3

# 00275640: per-slot bit 7 change tracking; 00275EF0: calls it for slots 1 and 2.
_TRACK_GRID = tuple(
    product((0, 1, 0xFF), (2, 3, 0xFF), (0, 1, 3), (0, 0x80, 0xFFFFFF7F, 0xFFFFFFFF), (0, 1, 2))
)
_TRACK_CONTROLS = 3
_OUTER_INNER = (
    (1, 2, (0, 0x80), (0, 0)),
    (1, 2, (0x80, 0), (1, 1)),
    (1, 2, (0x80, 0x80), (0, 1)),
    (1, 3, (0x80, 0x80), (0, 0)),
)
_OUTER_GRID = tuple(
    product((7, 8), (0, 1, 0x80000000), (0, 1, 2), (1, 2, 3), range(len(_OUTER_INNER)))
)
_OUTER_CONTROLS = 3


def _count(va: int) -> int:
    return {
        0x247A10: len(_SCAN_GRID) * _SCAN_REPEAT + _SCAN_CONTROLS,
        0x248910: len(_GATE_GRID) * _GATE_REPEAT + _GATE_CONTROLS,
        0x2346A0: len(_ID_GRID) * _ID_REPEAT + _ID_CONTROLS,
        0x2291E0: len(_A_GRID) + len(_B_GRID) + _ROW_CONTROLS,
        0x1B7E80: len(_RECORD_GRID) + _RECORD_CONTROLS,
        0x25F320: len(_LINK_GRID) * _LINK_REPEAT + _LINK_CONTROLS,
        0x275640: len(_TRACK_GRID) + _TRACK_CONTROLS,
        0x275EF0: len(_OUTER_GRID) + _OUTER_CONTROLS,
    }.get(va, 0)


def t1478_batch6_case_count(va: int) -> int:
    return _count(va)


def make_t1478_batch6_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < _count(va):
        raise ValueError("unsupported T1478 batch6 case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)
    frame = next(data for address, data in case.patches if address == case.esp)

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    def move_entry(new_esp: int, length: int) -> None:
        patches.append((new_esp, frame[:length]))
        regs[4] = new_esp

    builders = {
        0x247A10: _scan_case,
        0x248910: _gate_case,
        0x2346A0: _id_case,
        0x2291E0: _row_case,
        0x1B7E80: _record_case,
        0x25F320: _link_case,
        0x275640: _track_case,
        0x275EF0: _outer_case,
    }
    builders[va](ordinal, case.esp, patches, word, move_entry)
    return replace(case, patches=tuple(patches), regs=tuple(regs))


def _byte(patches: Patches, address: int, value: int) -> None:
    patches.append((address, bytes([value & 0xFF])))


def _scan_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    controls = ordinal - len(_SCAN_GRID) * _SCAN_REPEAT
    word(esp + 4, _KEY)
    if controls >= 0:
        # Unmapped table base (0x90000000) or null base, count 2 / 1: both fault in the original.
        word(0x78B720, 0x90000000 if controls == 0 else 0)
        word(0x78B72C, 2 if controls == 0 else 1)
        return
    (count, target), skip = _SCAN_GRID[ordinal % len(_SCAN_GRID)]
    base = _ARENA
    word(0x78B720, base)
    word(0x78B72C, count)
    for index in range(count):
        record = base + 0x5C * index
        obj = _ARENA + 0x10000 + index * 0x400
        matching = target is not None and index <= target
        word(record + 4, 0 if matching and index == target and skip == 1 else 1)
        word(record + 0xC, 0 if matching and index == target and skip == 2 else obj)
        mismatch = not matching or (index == target and skip == 3)
        word(obj + 0x230, _OTHER if mismatch else _KEY)


def _gate_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    arg, state = _ARENA, _ARENA + 0x1000
    table = _ARENA + 0x2000
    controls = ordinal - len(_GATE_GRID) * _GATE_REPEAT
    kind, gate, target, flag, value = (
        _GATE_GRID[ordinal % len(_GATE_GRID)] if controls < 0 else ("scan", 0x1000, 0, 1, 0x77)
    )
    word(esp + 4, 0x90000000 if controls == 0 else arg)
    word(arg + 0x150, 0x90000000 if controls == 1 else state)
    word(state + 0x114, gate)
    word(0x78B720, 0x90000000 if controls == 2 else table)
    if kind == "gate":
        return
    for slot in range(4):
        record = table + 0x5C * slot
        later = target is not None and slot > target
        hit = target is not None and slot >= target
        word(record + 4, flag if slot == target else 1)
        word(record + 0x10, arg if hit else _OTHER)
        word(record + 0xC, value ^ 0xFFFF if later else value)


def _id_value(identifier: int, patches_word: Word, value: int) -> None:
    if 0x1F40 <= identifier < 0x209E:
        patches_word(_T2 + ((identifier - 0x1F40) << 5), value)
    elif identifier != 0xFFFFFFFF and not 0x209E <= identifier < 0x20B2:
        patches_word(_T1 + (identifier << 5), value)


def _lookup_result(identifier: int, value: int) -> int:
    if identifier == 0xFFFFFFFF:
        return 0xFFFFFFFF
    if 0x209E <= identifier < 0x20B2:
        return 0xFFFFFFFE
    return value


def _id_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    controls = ordinal - len(_ID_GRID) * _ID_REPEAT
    identifier, match, filler = _ID_GRID[ordinal % len(_ID_GRID)] if controls < 0 else (5, 2, 0)
    value = 0x1234
    argument, obj = _ARENA, _ARENA + 0x1000
    word(0x715F2C, _T1)
    word(0x715F34, _T2)
    _id_value(identifier, word, value)
    word(obj + 0x678, identifier)
    result = _lookup_result(identifier, value)
    for field in range(1, 5):
        word(
            argument + 4 * field,
            result if field == match else (0xFFFFFFFF if filler == 0 else result + 1),
        )
    word(esp + 4, 0x90000000 if controls == 1 else argument)
    word(esp + 8, 0 if controls == 0 else (0x90000000 if controls == 2 else obj))


def _row_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    record, other = _ARENA, _ARENA + 0x1000
    controls = ordinal - len(_A_GRID) - len(_B_GRID)
    word(esp + 4, 0x90000000 if controls == 0 else record)
    if controls == 1:
        # Branch B (flag bit 6 clear) with an unmapped [7356D8].
        word(record + 0x28, 0)
        word(0x7356D8, 0x90000000)
        return
    if controls >= 0 or ordinal < len(_A_GRID):
        if ordinal < len(_A_GRID):
            p50, multiplier, relation, index = _A_GRID[ordinal]
        else:
            p50, multiplier, relation, index = 1, 2, 2, 0
        product_value = (p50 * multiplier) & 0xFFFFFFFF
        memory = (
            (product_value - 1) & 0xFFFFFFFF,
            product_value,
            (product_value + 1) & 0xFFFFFFFF,
            0,
            0xFFFFFFFF,
        )[relation]
        word(record + 0x28, 0x41)
        word(record + 0x50, p50)
        word(0x4E8CEC, multiplier)
        word(0x7B0C7C, 0x90000000 if controls == 2 else other)
        word(other + 4, index)
        word(0x75C368 + index * 4, memory)
        return
    flags, identifier, match50, match54 = _B_GRID[ordinal - len(_A_GRID)]
    value = 0x1234
    word(record + 0x28, flags)
    word(0x715F2C, _T1)
    word(0x715F34, _T2)
    _id_value(identifier, word, value)
    result = _lookup_result(identifier, value)
    word(0x7356D8, other)
    word(other + 0x678, identifier)
    word(record + 0x50, result if match50 else result + 7)
    word(record + 0x54, result if match54 else result + 9)


def _record_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    controls = ordinal - len(_RECORD_GRID)
    index, mode, field, flags = _RECORD_GRID[ordinal] if controls < 0 else (1, 0x0A, 0, 2)
    word(esp + 4, index)
    word(0x4F9BAC, 0x90000000 if controls == 0 else _ARENA)
    _byte(patches, 0x7DE455, mode)
    if controls == 0:
        return
    record = _ARENA + 0x29C * (index if index < 0x80000000 else 0)
    word(record + 0x1F8, field)
    word(record + 0xC, flags)
    if controls == 1:
        word(0x7497C8, 0xFFFFFFFF)


def _link_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    controls = ordinal - len(_LINK_GRID) * _LINK_REPEAT
    gate, (count, target), kind = (
        _LINK_GRID[ordinal % len(_LINK_GRID)] if controls < 0 else (0x163830, (1, 0), 1)
    )
    obj, child, block = _ARENA, _ARENA + 0x1000, _ARENA + 0x2000
    word(esp + 4, 0x90000000 if controls == 0 else obj)
    word(obj + 0x7C, 0x90000000 if controls == 1 else child)
    word(child + 0xBF0, 0x90000000 if controls == 2 else block)
    word(child + 4, 0x55555555)
    word(block + 0x6C, gate)
    word(block + 0x38, _KEY)
    word(0x78B520, count)
    for index in range(max(count, 1)):
        record = 0x78B2E0 + 0x20 * index
        pointee = _ARENA + 0x3000 + 0x40 * index
        matching = target is not None and index >= target
        record_kind = (kind ^ 3) if target is not None and index > target else kind
        word(record, _KEY if matching else _OTHER)
        word(record + 0x10, 0 if record_kind == 0 else pointee)
        word(pointee + 0x20, 8 if record_kind == 1 else 9)


def _track_slots(word: Word, patches: Patches, index: int, bits: int, old: int) -> None:
    record = _ARENA + 0x1000 + index * 0x100
    table = _ARENA + 0x2000
    word(0x7A2BE4 + index * 8, record)
    word(record + 0x28, bits)
    word(0x74C3B8, table)
    word(table + index * 16 - 8, old)
    word(0x7DE318, 0x600D0000 | index)


def _track_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    controls = ordinal - len(_TRACK_GRID)
    gate, mode, index, bits, old = _TRACK_GRID[ordinal] if controls < 0 else (1, 2, 1, 0x80, 0)
    _byte(patches, 0x7DE402, gate)
    _byte(patches, 0x7DE455, mode)
    word(esp + 4, index)
    _track_slots(word, patches, index, bits, old)
    if controls == 0:
        word(0x74C3B8, 0x90000000)
    elif controls == 1:
        word(0x7A2BE4 + index * 8, 0x90000000)
    elif controls == 2:
        # Amendment A1 (written AFTER the first control overwrote the root's own return
        # slot): ESP == table + 16*index, so the saved ESI lands on the stamped word at
        # table + 16*index - 4 and the pushed slot lands on the old word; no return slot aliases.
        move_entry(_ARENA + 0x2000 + index * 16, 8)
        word(_ARENA + 0x2000 + index * 16 + 4, index)


def _outer_case(ordinal: int, esp: int, patches: Patches, word: Word, move_entry: Move) -> None:
    controls = ordinal - len(_OUTER_GRID)
    selector, counter_state, counter, limit, inner = (
        _OUTER_GRID[ordinal] if controls < 0 else (7, 1, 0xFFFFFFFF, 0, 0)
    )
    gate, mode, bits, olds = _OUTER_INNER[inner]
    word(0x7DE30C, selector)
    word(0x4E7944, counter_state)
    word(0x75F5E4, counter)
    word(0x790950, limit)
    _byte(patches, 0x7DE402, gate)
    _byte(patches, 0x7DE455, mode)
    for slot in (1, 2):
        _track_slots(word, patches, slot, bits[slot - 1], olds[slot - 1])
    if controls == 0:
        word(0x74C3B8, 0x90000000)
    elif controls == 1:
        word(0x7A2BE4 + 8, 0x90000000)
