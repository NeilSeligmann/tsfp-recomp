# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace 56 domains for T1479's fifth bounded batch (six table/record roots).

Contracts and ordinal layouts: docs/evidence/t1479/fifth-batch-predeclaration.md (written and
committed before any audit, native run or proof outcome). Every ordinal's state and stack frame
derive from the declared tuples below, never from observed results. Registers and unpatched
argument words keep the seed stream values.
"""

import hashlib

from .model import Case
from .seeding import SeedPolicy, make_case
from .t1479_batch3_cases import _Builder, _digits

BATCH5_INDEX_BASE = 56 << 40

_NET = 0x76B108
_MODE = 0x79094C
_SESSION = 0x774028
_LIMIT = 0x790950
_A = 0xD70000
_Q = 0xD70900
_PLAYERS = 0x7BEE40
_PLAYER_STRIDE = 0x1F54

#: 2C3390: (network, mode, session object, [S+0xC], [S+0x20], [Q+0x3C]) -> player index class.
_CLASSES = (
    (0, 0x64, 0, 0, 0, 0),
    (1, 0x65, 0, 0, 0, 0),
    (1, 0x64, _A, _Q, 1, 1),
    (1, 0x64, _A, _Q, 1, 2),
    (1, 0x64, _A, _Q, 1, 0xFFFFFFFF),
)
_CLASS_INDEX = (0, 0, 1, 2, 0xFFFFFFFF)
#: (offset in the player object, global address) of the six comparisons, in call order.
_COMPARES = (
    (0x120, 0x5236BC),
    (0x118, 0x5236B4),
    (0x11C, 0x5236B8),
    (0x12C, 0x7613D8),
    (0x124, 0x5236C0),
    (0x130, 0x7613DC),
)
_VALUE_SETS = (
    (0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666),
    (0, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF, 1, 0xFFFFFFFE),
    (5, 5, 5, 5, 5, 5),
)

#: 2E6F80 roster entry patterns, roster id -> record bytes, Q words.
_ROSTER_COUNTS = ((0, 0), (1, 0), (2, 1), (1, 3))
_ROSTER_PATTERNS = (
    (0, 1, 2, 3),
    (0xFFFFFFFF, 0, 0, 1),
    (2, 2, 2, 2),
    (3, 0xFFFFFFFF, 1, 0),
    (1, 1, 0xFFFFFFFF, 0xFFFFFFFF),
)
_RECORD_BASE = 0x6F0B08
_Q_WORDS = {0: 1000, 1: 0x7FFF, 2: 0x8000}
_B0 = (0, 1, 0xFF)

#: 2B2390 per-entry byte patterns (the byte at +0x28 of each entry's object).
_FLAG_PATTERNS = (
    (0, 0, 0),
    (1, 0, 0),
    (0, 1, 0),
    (0, 0, 1),
    (1, 1, 1),
    (2, 2, 2),
    (3, 0, 0),
    (0xFE, 0xFE, 0xFE),
    (0xFF, 0, 0),
    (0, 0xFF, 0),
    (2, 0, 1),
    (0xFE, 0, 0x81),
)
_COUNTS = (0, 1, 2, 3, 0xFFFFFFFF)

#: 33ACB0 per-player value triples, slot triples and the table-equal flag.
_VALUE_TRIPLES = ((0, 1, 2), (1, 5, 0xFFFFFFFF), (2, 2, 2), (0xFFFFFFFF, 0, 1))
_SLOT_TRIPLES = ((0, 1, 2), (1, 1, 3), (2, 0, 2))

#: 321560 second-table entry patterns: (word, flag) for two entries.
_ENTRY_PATTERNS = (((0, 1), (1, 0)), ((1, 1), (2, 1)), ((5, 1), (0, 2)), ((2, 0), (1, 1)))

_SPEC = {
    0x2C3390: (0, 7 * 5 * 3 + 2),
    0x2E6F80: (1, 4 * 5 * 2 * 4 * 3),
    0x2B2390: (0, 5 * len(_FLAG_PATTERNS) + 200),
    0x2FE0F0: (3, 2 * 4 * 2 * 2 + 4 * 5 + 4),
    0x33ACB0: (0, 4 * 4 * 3 * 2),
    0x321560: (1, 4 * 8 * 3 * 4),
}


def batch5_case_count(va: int) -> int:
    return _SPEC.get(va, (0, 0))[1]


def _player_base(cls: int) -> int:
    return (_PLAYERS + _CLASS_INDEX[cls] * _PLAYER_STRIDE) & 0xFFFFFFFF


def make_batch5_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    arguments, total = _SPEC.get(va, (0, 0))
    if not 0 <= ordinal < total:
        raise ValueError("unsupported T1479 fifth-batch case")
    case = make_case(seed, BATCH5_INDEX_BASE + ordinal, va, size, policy=policy)
    builder = _Builder(case, arguments)
    word, byte = builder.word, builder.byte

    if va == 0x2C3390:
        grid = 7 * 5 * 3
        alias = None
        if ordinal < grid:
            mismatch, cls, value_set = _digits(ordinal, 7, 5, 3)
        else:
            alias = ordinal - grid
            mismatch, cls, value_set = 6, 3, 0
        network, mode, session, session_c, session_20, record_3c = _CLASSES[cls]
        word(_NET, network)
        word(_MODE, mode)
        word(_SESSION, session)
        if session:
            word(_A + 0xC, session_c)
            word(_A + 0x20, session_20)
            word(_LIMIT, 3)
            word(_Q + 0x3C, record_3c)
        base = _player_base(cls)
        values = list(_VALUE_SETS[value_set])
        for step, (offset, address) in enumerate(_COMPARES):
            value = values[step]
            word(base + offset, value)
            word(address, value ^ 1 if mismatch == step else value)
        if alias == 0:
            word(base + 0x120, 0x2C3395)
            word(0x5236BC, 0x2C3395)
            builder.move_entry(0x5236BC + 4)
        elif alias == 1:
            word(base + 0x130, 0x2C33F4)
            word(0x7613DC, 0x2C33F4)
            builder.move_entry(0x7613DC + 4)
    elif va == 0x2E6F80:
        count, pattern, tag, flag, b0 = _digits(ordinal, 4, 5, 2, 4, 3)
        first, second = _ROSTER_COUNTS[count]
        word(0x7A2958, first)
        word(_LIMIT, second)
        word(0x7DE458, (0, 0x20, 0x10, 0x30)[flag])
        queue_tag = (5, 6)[tag]
        byte(_A, _B0[b0])
        byte(_A + 1, queue_tag)
        for entry, value in _Q_WORDS.items():
            builder.half(_A + 2 + 2 * entry, value)
        builder.half(_A + 0x4E, 0xFFFB)
        for index, roster_id in enumerate(_ROSTER_PATTERNS[pattern]):
            word(0x7DE4C0 + 4 * index, roster_id)
            if roster_id < 0x80000000:
                record = _RECORD_BASE + roster_id * 0x68
                byte(record, (0, 1, 2, 0xFF)[roster_id % 4])
                byte(record + 1, 5 if roster_id % 2 == 0 else 6)
        builder.argument(0, _A)
    elif va == 0x2B2390:
        main = 5 * len(_FLAG_PATTERNS)
        if ordinal < main:
            count_index, pattern = divmod(ordinal, len(_FLAG_PATTERNS))
            count, flags = _COUNTS[count_index], _FLAG_PATTERNS[pattern]
        else:
            # Amendment A1 (written AFTER null-agree 0.9712): answer-0 ordinals with a hash
            # count 1..3, one hash-chosen entry with bit 0 set and hash bytes with bit 0 clear.
            stream = b"".join(
                hashlib.sha256(f"t1479-batch5-a1:{ordinal}:{block}".encode()).digest()
                for block in range(2)
            )
            count = 1 + stream[0] % 3
            chosen = stream[1] % count
            flags = tuple(
                (stream[2 + index] | 1) if index == chosen else (stream[2 + index] & 0xFE)
                for index in range(3)
            )
        word(0x7A2BCC, count)
        for index in range(3):
            entry = 0x7A2960 + 8 * index
            word(entry + 4, _A + 0x40 * index)
            byte(_A + 0x40 * index + 0x28, flags[index])
    elif va == 0x2FE0F0:
        flag_grid = 2 * 4 * 2 * 2
        index_values = (0, 1, 3, 0xFFFFFFFF)
        source, dest, flag = _A, _A + 0x100, 0
        if ordinal < flag_grid:
            flag_index, bit, idx, player = _digits(ordinal, 2, 2, 4, 2)
            flag = (1, 0xFFFFFFFF)[flag_index]
            word(0x7DE458, (0, 0x800000)[bit])
            word(0x7BEDC0, player)
            word(0x7DE470, index_values[idx])
            record = (
                (0x6F11C0 if (bit or not player) else 0x7C0050) + index_values[idx] * 24 + 0x490
            )
            for k in range(6):
                word(record + 4 * k, 0xB0000000 + 0x1111 * (k + 1) + idx)
        elif ordinal < flag_grid + 20:
            pattern, overlap = divmod(ordinal - flag_grid, 5)
            dest = (source + (0x100, 4, 8, 0xC, 0xFFFFFFFC)[overlap]) & 0xFFFFFFFF
            fill = (
                (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16),
                (0xFF,) * 16,
                (0, 0x80, 0x7F, 0x81) * 4,
                tuple(range(0xF0, 0x100)),
            )[pattern]
            for k, value in enumerate(fill):
                byte(source + k, value)
        else:
            overlap = ordinal - flag_grid - 20
            flag = 1
            word(0x7DE458, 0x800000)
            word(0x7DE470, 1)
            record = 0x6F11C0 + 24 + 0x490
            for k in range(6):
                word(record + 4 * k, 0xB0000000 + 0x1111 * (k + 1))
            dest = (record + (4, 8, 0x14, 0xFFFFFFFC)[overlap]) & 0xFFFFFFFF
        builder.argument(0, source)
        builder.argument(1, dest)
        builder.argument(2, flag)
    elif va == 0x33ACB0:
        players, values_index, slots_index, equal = _digits(ordinal, 4, 4, 3, 2)
        base = 0xD80000
        word(_LIMIT, players)
        word(0x7B0C48, base)
        for player in range(3):
            value = _VALUE_TRIPLES[values_index][player]
            slot = _SLOT_TRIPLES[slots_index][player]
            word(0x784034 + 4 * player, value)
            owner = _A + 0x400 * player
            esi = _A + 0x1000 + 0x400 * player
            word(base + player * 0x1584 + 0x14, owner)
            word(owner + 0x7C, esi)
            word(esi + 8, slot)
            word(0x4D1E28 + ((value + 1) & 0xFFFFFFFF) * 20, 0x1000 + player)
            byte(0x7DE424 + slot, ((value + 1) & 0xFF) if equal else 0x5A)
    else:
        n1, match, n2, entry_pattern = _digits(ordinal, 4, 8, 3, 4)
        world = 0xDA0000
        wanted = 0x77
        word(0x7844A8, world)
        word(world + 0xDD10, n1)
        word(world + 0x81A4, n2)
        for index in range(3):
            word(world + 0xDD98 + 0x8C * index, wanted if (match >> index) & 1 else wanted + 1)
        for index, (entry_word, entry_flag) in enumerate(_ENTRY_PATTERNS[entry_pattern]):
            builder.half(world + 0x81B4 + 0xB4 * index, entry_word)
            word(world + 0x8250 + 0xB4 * index, entry_flag)
        builder.argument(0, wanted)
    return builder.build()
