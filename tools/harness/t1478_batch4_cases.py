# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace44 domains for T1478's fourth call-bearing batch.

Contracts, ordinal layouts and gates: docs/evidence/t1478/fourth-domain.md, written and
committed before any proof outcome. Domains are additive after the unchanged random,
edge and feedback streams; 00190A80 and 0023F2E0 deliberately add no fixtures.
"""

from collections.abc import Callable
from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 44 << 40
Word = Callable[[int, int], None]
Move = Callable[[int, int], None]
_TABLE = 0x5655B0
_GETTER = 0x7B979C
_INVALID_INDEX = (11, 12, 255, 0x80000000, 0xFFFFFFFF, 0x05555556, 0x15555556, 100)
# 24E40 returns the dword at table+4+48*index (32-bit wrap), so the index-wrap values above
# (0x05555556 * 48 = 0x1_0000_0020) alias real records.
_WORD_PATTERNS = (0, 0xFFFFFFFF, 0xA5000000)
_TABLE_ALIAS_ESP = (0x7B97A0, 0x5655B8, 0x5655B8 + 48)
_TABLE_ALIAS_INDEX = (0, 0, 1)

# Callback registrations: (destination dword, callback VA) in original guest call order.
_CALLBACK_ROWS = {
    0x1829D0: (
        (0x72F4F8, 0x173790),
        (0x72F4FC, 0x17E680),
        (0x72F500, 0x17C2E0),
        (0x72F4F4, 0x180800),
    ),
    0x249880: ((0x72F58C, 0x248D90), (0x72F580, 0x2493E0), (0x72F584, 0x249580)),
}
_CALLBACK_CASES = 4

# Keyed chain (23FF20) fixtures: nodes {handle, key, next}, generation table at 0x72F5E8.
_CHAIN_NODES = (0xD20000, 0xD20200, 0xD20400)
_CHAIN_BASE = 0xD10000
_CHAIN_GENERATIONS = (0, 1, 1023)
_CHAIN_MODES = tuple(range(8))
_CHAIN_LAYOUTS = (0, 1)
_RECORD_WORDS = (0, 1, 0xFFFFFFFF, 0x12345678)
_CHAIN_GRID = tuple(product(_CHAIN_MODES, _CHAIN_GENERATIONS, _CHAIN_LAYOUTS, _RECORD_WORDS))
_CHAIN_CONTROLS = 3
# Amendment A1 (written AFTER the default domains failed gates, see fourth-domain.md):
# 190A80 failed null-agree 1.0 (all three globals start zero, so the original's zero stores
# are unobservable) and 1DE560 failed the >=100-verdict gate (73: five of eight chain modes
# end in an original null-record fault). Both append ordinals AFTER the unchanged ones.
_AMEND_PATTERNS = (1, 0xFFFFFFFF, 0x80000000, 0x12345678)
_AMEND_GLOBAL_CASES = 256
_AMEND_FIELDS = (2, 0x7FFFFFFF, 0x100, 0xFF, 0xFFFF0000, 0x10000)
_AMEND_FOUND_CASES = 3 * 3 * 2 * len(_AMEND_FIELDS)

# 0015BA10 table fixtures: records of 0xEA0 bytes at [7356D8], window [790950, +[7A2958]).
_TABLE_BASE = 0xD40000
_HANDLE_BASE = 0xD80000
_OBJECT_BASE = 0xD90000
_KEYS = {0x1EC980: (0x47415330,), 0x1EC9C0: (0x47415331,), 0x1ECA00: (0x47415330, 0x47415331)}
_MODES = (0x0A, 0x01)
_TYPES = (0x4C, 0x4D)
_GATES = (0, 0x80000000)
_SINGLE = tuple(
    (mode, start, count, target, skip, kind, gate)
    for mode in _MODES
    for start in (0, 2)
    for count in range(4)
    for target in range(count + 1)
    for skip in range(3)
    for kind in _TYPES
    for gate in _GATES
)
_PAIR_POSITIONS = {
    count: tuple(
        (first, second)
        for first in (None, *range(count))
        for second in (None, *range(count))
        if first is None or first != second
    )
    for count in (1, 2, 3)
}
_DOUBLE = tuple(
    (mode, start, count, first, second, skip_first, skip_second, kind_first, kind_second)
    for mode in _MODES
    for start in (0, 2)
    for count in (1, 2, 3)
    for first, second in _PAIR_POSITIONS[count]
    for skip_first in (0, 1)
    for skip_second in (0, 2)
    for kind_first in _TYPES
    for kind_second in _TYPES
)
_TABLE_CONTROLS = 4


def _count(va: int) -> int:
    if va == 0x190A80:
        return _AMEND_GLOBAL_CASES
    if va == 0x190A70:
        return 11 * len(_WORD_PATTERNS) + len(_INVALID_INDEX) + len(_TABLE_ALIAS_ESP)
    if va in _CALLBACK_ROWS:
        return _CALLBACK_CASES
    if va == 0x240560:
        return len(_CHAIN_GRID) + _CHAIN_CONTROLS
    if va == 0x1DE560:
        return len(_CHAIN_GRID) + _CHAIN_CONTROLS + _AMEND_FOUND_CASES
    if va in (0x1EC980, 0x1EC9C0):
        return len(_SINGLE) + _TABLE_CONTROLS
    if va == 0x1ECA00:
        return len(_DOUBLE) + _TABLE_CONTROLS
    return 0


def t1478_batch4_case_count(va: int) -> int:
    return _count(va)


def make_t1478_batch4_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < _count(va):
        raise ValueError("unsupported T1478 batch4 case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)
    frame = next(data for address, data in case.patches if address == case.esp)

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    def move_entry(new_esp: int, length: int) -> None:
        patches.append((new_esp, frame[:length]))
        regs[4] = new_esp

    if va == 0x190A80:
        combo = ordinal % len(_AMEND_PATTERNS) ** 3
        for slot, address in enumerate((0x7B9798, 0x7B9794, 0x7B978C)):
            word(
                address,
                _AMEND_PATTERNS[combo // len(_AMEND_PATTERNS) ** slot % len(_AMEND_PATTERNS)],
            )
    elif va == 0x190A70:
        _table_index_case(ordinal, word, move_entry)
    elif va in _CALLBACK_ROWS:
        _callback_case(ordinal, va, case.esp, word)
    elif va in (0x240560, 0x1DE560):
        _chain_case(ordinal, va, case.esp, word, move_entry)
    else:
        _record_table_case(ordinal, va, patches, word, move_entry)
    return replace(case, patches=tuple(patches), regs=tuple(regs))


def _table_index_case(ordinal: int, word: Word, move_entry: Move) -> None:
    valid = 11 * len(_WORD_PATTERNS)
    if ordinal < valid:
        index, pattern = divmod(ordinal, len(_WORD_PATTERNS))
        word(_TABLE + 4 + 48 * index, _WORD_PATTERNS[pattern] | (index if pattern == 2 else 0))
    elif ordinal < valid + len(_INVALID_INDEX):
        index = _INVALID_INDEX[ordinal - valid]
    else:
        control = ordinal - valid - len(_INVALID_INDEX)
        index = _TABLE_ALIAS_INDEX[control]
        word(_TABLE + 4 + 48 * index, 0xC0DE0000 | control)
        move_entry(_TABLE_ALIAS_ESP[control], 4)
    word(_GETTER, index)


def _callback_case(ordinal: int, va: int, esp: int, word: Word) -> None:
    rows = _CALLBACK_ROWS[va]
    targets = {address for address, _ in rows}
    for address in sorted({a + d for a in targets for d in (-4, 4)} - targets):
        word(address, 0x13579BDF ^ address)
    for address, callback in rows:
        word(address, (0, 0xFFFFFFFF, 0x80000000, callback)[ordinal])
    for index in range(1, 3 * len(rows) + 2):
        word(esp - 4 * index, (0xC0DE0000 | index) ^ va)


def _chain_case(ordinal: int, va: int, esp: int, word: Word, move_entry: Move) -> None:
    query = 0x54524350 if va == 0x240560 else 0x44524157
    control = ordinal - len(_CHAIN_GRID)
    if ordinal < len(_CHAIN_GRID):
        mode, generation, layout, pattern = _CHAIN_GRID[ordinal]
    elif control >= _CHAIN_CONTROLS:
        found = control - _CHAIN_CONTROLS
        mode, generation = 1 + found // 36, _CHAIN_GENERATIONS[found // 12 % 3]
        layout, pattern = found // 6 % 2, _AMEND_FIELDS[found % 6]
        control = -1
    else:
        mode, generation, layout, pattern = 1, 0, 0, 1
    handle = (generation << 22) | 1
    record = _CHAIN_BASE + generation * 0x2E8
    obj = 0xD30000
    position = mode - 1 if mode in (1, 2, 3) else 0
    word(0x75D318, 0 if mode == 0 else _CHAIN_NODES[0])
    word(0x72F5E8, _CHAIN_BASE)
    word(record + 0x2C, handle ^ 1 if mode == 7 else handle)
    if va == 0x240560:
        word(esp + 4, query)
        word(record + 0x7C, pattern)
    else:
        word(record + 0x7C, obj)
        word(obj + 0xAC, pattern)
    for slot, node in enumerate(_CHAIN_NODES):
        key = query + slot - position
        if mode == 4:
            key = (0x80000000, 0xFFFFFFFE, 0xFFFFFFFF)[slot] if layout else query - 3 + slot
        elif mode == 5:
            key = 0x7FFFFFFF if layout else query + 1 + slot
        elif layout:
            key = 0x80000000 if slot < position else query if slot == position else 0x7FFFFFFF
        word(node, 0 if mode == 6 else handle)
        word(node + 4, key)
        word(node + 8, _CHAIN_NODES[slot + 1] if slot < 2 else 0)
    if control == 0:
        word(0x75D318, 0x90000000)
    elif control == 1:
        word(0x72F5E8, 0x90000000)
    elif control == 2:
        # The pushed key slot IS the field the root reads after the lookup returns.
        field = record + 0x7C if va == 0x240560 else obj + 0xAC
        move_entry(field + 4, 4)
        if va == 0x240560:
            word(field + 4 + 4, query)


def _write_record(word: Word, absolute: int, mode: int, key: int, skip: int, kind: int) -> None:
    pointer = (_TABLE_BASE + absolute * 0xEA0 + 0x18) & 0xFFFFFFFF
    handle = _HANDLE_BASE + (absolute & 0xFF) * 0x100
    obj = _OBJECT_BASE + (absolute & 0xFF) * 0x100
    active, inactive = ((pointer - 8, pointer), (pointer, pointer - 8))[mode != 0x0A]
    zero_active = skip == 2
    word(pointer - 0x18, handle)
    word(active, 0)
    word(active + 4, 0 if zero_active else 0x80000000)
    word(inactive, 0xFFFFFFFF if zero_active else 0)
    word(inactive + 4, 0xFFFFFFFF if zero_active else 0)
    word(pointer + 0xC, 8 if skip == 1 else 0)
    word(pointer + 0xBD8, obj)
    word(obj + 8, key)
    word(handle + 0x34, kind)


def _record_table_case(
    ordinal: int, va: int, patches: list[tuple[int, bytes]], word: Word, move_entry: Move
) -> None:
    keys = _KEYS[va]
    first, last = keys[0], keys[-1]
    other = 0x11111111
    word(0x7356D8, _TABLE_BASE)
    if va == 0x1ECA00 and ordinal < len(_DOUBLE):
        mode, start, count, p0, p1, skip0, skip1, kind0, kind1 = _DOUBLE[ordinal]
        gate = 0
        specs = []
        for position in range(count):
            if position == p0:
                specs.append((keys[0], skip0, kind0))
            elif position == p1:
                specs.append((keys[1], skip1, kind1))
            else:
                specs.append((other, 0, 0x4C))
    elif va != 0x1ECA00 and ordinal < len(_SINGLE):
        mode, start, count, target, skip, kind, gate = _SINGLE[ordinal]
        specs = []
        for position in range(count):
            if position < target:
                specs.append((other, 0, 0x4C))
            else:
                later = position > target
                specs.append((keys[0], 0 if later else skip, kind ^ 1 if later else kind))
    else:
        control = ordinal - (len(_DOUBLE) if va == 0x1ECA00 else len(_SINGLE))
        mode, start, count, gate = 0x0A, 1, 2, 0x80000000
        specs = [(keys[0], 0, 0x4C), (keys[-1], 0, 0x4D)]
        if control == 0:
            word(0x7356D8, 0x90000000)
        elif control == 1:
            move_entry(0x790954, 4)
        elif control == 2:
            start = 0xFFFFFFFF
        else:
            start, count, specs = 0, 0xFFFFFFFF, []
    signed_start = start - (1 << 32) if start >= 1 << 31 else start
    word(0x790950, start)
    word(0x7A2958, count)
    word(0x75BF0C, gate)
    patches.append((0x7DE455, bytes([mode])))
    if signed_start > 0:
        _write_record(word, signed_start - 1, mode, first, 0, 0x4C)
    for position, (key, skip, kind) in enumerate(specs):
        _write_record(word, signed_start + position, mode, key, skip, kind)
    _write_record(word, signed_start + len(specs), mode, last, 0, 0x4D)
