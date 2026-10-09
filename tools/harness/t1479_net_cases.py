# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace43 domains for T1479's six net/profile-state call roots.

Contracts and ordinal layouts: docs/evidence/t1479/second-batch-predeclaration.md (written
before outcomes). Every ordinal's global/table state and stack frame derive from the
declared tuples below, never from observed results.
"""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

NET_INDEX_BASE = 43 << 40
_NETWORK = 0x76B108
_MODE = 0x79094C
_SESSION = 0x774028
_LIMIT = 0x790950
_PLAYERS = 0x7BEE40
_PLAYER_STRIDE = 0x1F54
_P = 0xD50000
_Q = 0xD51000

#: (net global 76B108, mode 79094C, session 774028 pointer, P+0xC, P+0x20, Q+0x3C): the eight
#: states the original 69640 -> 356800(0) chain can reach, as resulting player index 0/1/2/-1.
_CLASSES = (
    (0, 0x64, 0, 0, 0, 0),
    (1, 0x65, 0, 0, 0, 0),
    (1, 0x64, 0, 0, 0, 0),
    (1, 0x64, _P, 0, 1, 0),
    (1, 0x64, _P, _Q, 5, 1),
    (1, 0x64, _P, _Q, 1, 1),
    (1, 0x64, _P, _Q, 1, 2),
    (1, 0x64, _P, _Q, 1, 0xFFFFFFFF),
)
_CLASS_INDEX = (0, 0, 0, 0, 0, 1, 2, 0xFFFFFFFF)
_SPEC = {
    0x3568C0: (1, 388),
    0x35E6A0: (3, 327),
    0x2BFEC0: (1, 972),
    0x2EF160: (0, 542),
    0x35EAF0: (0, 97),
    0x356990: (0, 51 + 192),
}
_SLOTS = (0, 1, 15, 16, 17, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF)
_FLAG_BYTES = (0, 1, 0xFE)
_RECORD_MODES = range(4)
_COMMAND_RESULTS = (1, 0, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 5)
_AB = ((0, 0), (0x11223344, 0x55667788), (0xFFFFFFFF, 0x80000000))
_FIELDS = (0, 3, 9, 10, 0x7FFFFFFF, 0x80000000)
_ARGS = (0xFFFFFFFF, 0, 1, 6, 7, 8, 9, 10, 0x7FFFFFFF, 0x80000000)
_TABLE_VALUES = (0, 1, 2, 3, 0xFFFFFFFF)
_FLAG_WORDS = (0, 1, 0x80000000)


def t1479_net_case_count(va: int) -> int:
    return _SPEC.get(va, (0, 0))[1]


def player_base(cls: int) -> int:
    return (_PLAYERS + _CLASS_INDEX[cls] * _PLAYER_STRIDE) & 0xFFFFFFFF


def make_t1479_net_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    arguments, total = _SPEC.get(va, (0, 0))
    if not 0 <= ordinal < total:
        raise ValueError("unsupported T1479 net case")
    case = make_case(seed, NET_INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)
    frame = next(data for address, data in case.patches if address == case.esp)
    frame = frame[: 4 + 4 * arguments]
    moved: list[int] = []

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    def byte(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, bytes([value & 0xFF])))

    def move_entry(new_esp: int, *values: int) -> None:
        moved.append(new_esp)
        patches.append((new_esp, frame))
        for slot, value in enumerate(values):
            word(new_esp + 4 + 4 * slot, value)
        regs[4] = new_esp

    def player_class(cls: int, *, extra_p: int | None = None) -> None:
        network, mode, session, p_c, p_20, q_3c = _CLASSES[cls]
        word(_NETWORK, network)
        word(_MODE, mode)
        word(_SESSION, session)
        if session:
            word(_P + 0xC, p_c if extra_p is None else extra_p)
            word(_P + 0x20, p_20)
            word(_LIMIT, 3)
            if p_c:
                word(_Q + 0x3C, q_3c)

    if va == 0x3568C0:
        grid = len(_SLOTS) * len(_FLAG_BYTES) * 2 * len(_RECORD_MODES)
        slot, flag, mode, record = 0, 1, 0x64, 2
        network, aliased, table = 1, None, 0xD60000
        if ordinal < 2 * grid:
            network, rest = divmod(ordinal, grid)
            slot_index, rest = divmod(rest, len(_FLAG_BYTES) * 2 * len(_RECORD_MODES))
            flag_index, rest = divmod(rest, 2 * len(_RECORD_MODES))
            mode_index, record = divmod(rest, len(_RECORD_MODES))
            slot, flag, mode = _SLOTS[slot_index], _FLAG_BYTES[flag_index], (0x65, 0x64)[mode_index]
        else:
            control = ordinal - 2 * grid
            if control == 0:
                slot, record, table = 1, 1, 0x90000000
            elif control == 1:
                aliased, flag = 0x777290, 0
            elif control == 2:
                aliased = 0x7356DC
            else:
                aliased, network, mode = 0x76B10C, 0, 0x65
        record_offset = (slot * 0xEA0) & 0xFFFFFFFF
        if record == 0:
            table = 0
        elif record == 3:
            table = (-record_offset) & 0xFFFFFFFF
        word(_NETWORK, network)
        word(_MODE, mode)
        if slot < 16 or slot >= 0x80000000:
            byte(0x77728C + ((slot * 0x124) & 0xFFFFFFFF), flag)
        word(0x7356D8, table)
        if record in (1, 2) and table != 0x90000000:
            word(table + record_offset, 0 if record == 1 else 0x11223344)
        word(0x7DE30C, 0x5A5A5A5A)
        word(case.esp + 4, slot)
        if aliased is not None:
            move_entry(aliased, slot)
    elif va == 0x35E6A0:
        grid = 3 * 3 * 2 * len(_COMMAND_RESULTS) * len(_AB)
        mode, state, network, third, (first, second) = 0x65, 2, 1, 1, _AB[1]
        alias = None
        if ordinal < grid:
            mode_index, rest = divmod(ordinal, grid // 3)
            state_index, rest = divmod(rest, grid // 9)
            network, rest = divmod(rest, len(_COMMAND_RESULTS) * len(_AB))
            third_index, ab_index = divmod(rest, len(_AB))
            mode, state = (0x65, 0x64, 0)[mode_index], (2, 0, 1)[state_index]
            third, (first, second) = _COMMAND_RESULTS[third_index], _AB[ab_index]
        else:
            alias = ordinal - grid
            mode = (0x64, 0x64, 0x65)[alias]
            state = 2
        word(_MODE, mode)
        word(0x76B140, state)
        word(_NETWORK, network)
        for address in (0x76BBBC, 0x76BBC0, 0x76BBC4):
            word(address, 0xAAAA5555)
        word(case.esp + 4, first)
        word(case.esp + 8, second)
        word(case.esp + 12, third)
        if alias == 0:
            move_entry(0x76BBB8, 0x11223344, 0x55667788, 1)
            word(0x76BBBC, 0x11223344)
            word(0x76BBC0, 0x55667788)
            word(0x76BBC4, 1)
        elif alias == 1:
            move_entry(0x76B144, first, second, third)
        elif alias == 2:
            move_entry(0x790950, first, second, third)
    elif va == 0x2BFEC0:
        main = 2 * len(_CLASSES) * len(_FIELDS) * len(_ARGS)
        byte_value, cls, field, argument = 0x13, 0, 3, 6
        extra_p, entry_alias = None, None
        if ordinal < main:
            byte_index, rest = divmod(ordinal, len(_CLASSES) * len(_FIELDS) * len(_ARGS))
            cls, rest = divmod(rest, len(_FIELDS) * len(_ARGS))
            field_index, arg_index = divmod(rest, len(_ARGS))
            byte_value, field, argument = (
                (0x13, 0x12)[byte_index],
                _FIELDS[field_index],
                _ARGS[arg_index],
            )
        elif ordinal < main + 8:
            index = ordinal - main
            byte_value, argument = (0x14, 0xFF, 0x00, 0x93)[index // 2], (0, 9)[index % 2]
            field = 0
        elif ordinal == main + 8:
            cls, extra_p = 5, 0x90000000
        else:
            entry_alias = ordinal - main - 9
        if entry_alias in (1, 2):
            cls = entry_alias
        player_class(cls, extra_p=extra_p)
        if entry_alias in (1, 2):
            word(_NETWORK, 0)
        byte(0x78ADAE, byte_value)
        base = player_base(cls)
        word(base + 0xC, field)
        word(case.esp + 4, argument)
        if entry_alias == 0:
            move_entry(base + 0xC + 4, 8)
        elif entry_alias is not None:
            move_entry(0x76B110, 8)
    elif va == 0x2EF160:
        classes = (0, 3, 5, 6)
        main = len(classes) * 3 * 3 * len(_TABLE_VALUES) * len(_FLAG_WORDS)
        cls, mode_byte, row, table_value, flag_word, alias = 0, 1, 1, 2, 0, None
        if ordinal < main:
            rest = ordinal
            class_index, rest = divmod(rest, 3 * 3 * len(_TABLE_VALUES) * len(_FLAG_WORDS))
            mode_index, rest = divmod(rest, 3 * len(_TABLE_VALUES) * len(_FLAG_WORDS))
            row_index, rest = divmod(rest, len(_TABLE_VALUES) * len(_FLAG_WORDS))
            value_index, flag_index = divmod(rest, len(_FLAG_WORDS))
            cls, mode_byte, row = (
                classes[class_index],
                (0, 1, 255)[mode_index],
                (0, 1, 255)[row_index],
            )
            table_value, flag_word = _TABLE_VALUES[value_index], _FLAG_WORDS[flag_index]
        else:
            alias = ordinal - main
            cls, mode_byte, row, table_value = 0, 0, 0, 2
        player_class(cls)
        byte(0x7DE455, mode_byte)
        byte(0x7DE456, row)
        word(0x4D1F84 + row * 32, table_value)
        flag_address = player_base(cls) + mode_byte * 20 + 0x24
        word(flag_address, flag_word)
        if alias == 0:
            move_entry(flag_address + 4)
        elif alias == 1:
            word(_NETWORK, 0)
            move_entry(0x76B110)
    elif va == 0x35EAF0:
        main = 4 * 4 * 6
        gate, done, value, alias = 1, 0, 0x7F, False
        if ordinal < main:
            gate_index, rest = divmod(ordinal, 4 * 6)
            done_index, value_index = divmod(rest, 6)
            gate, done, value = (
                (1, 0, 2, 0xFFFFFFFF)[gate_index],
                (0, 1, 0x100, 0xFFFFFFFF)[done_index],
                (0, 1, 0x7F, 0xFD, 0xFE, 0xFF)[value_index],
            )
        else:
            alias = True
        word(0x774024, gate)
        word(0x76BD6C, done)
        byte(0x76BD68, value)
        if alias:
            move_entry(0x774028)
    else:
        network, mode, state, mark, alias = 1, 0x65, 2, 0xA, None
        main = 2 * 3 * 2 * 4
        if ordinal < main:
            mode_index, rest = divmod(ordinal, 3 * 2 * 4)
            state_index, rest = divmod(rest, 2 * 4)
            network, mark_index = divmod(rest, 4)
            mode, state, mark = (
                (0x65, 0x64)[mode_index],
                (2, 0, 3)[state_index],
                (0xA, 0, 0xB, 0xFFFFFFFF)[mark_index],
            )
        elif ordinal < main + 3:
            alias = ordinal - main
            mode, state, mark = 0x64, 2, 0xA
            network = 0 if alias == 0 else 1
        else:
            # Amendment A1 (written AFTER the default domain failed null-agree at 0.9704):
            # the two answer-1 states, 96 ordinals each; registers and frame vary per ordinal.
            mode, state, mark = (0x65, 0x64)[(ordinal - main - 3) // 96], 2, 0xA
        word(_MODE, mode)
        word(0x76B140, state)
        word(_NETWORK, network)
        word(0x76B1A4, mark)
        if alias == 0:
            move_entry(0x76B10C)
        elif alias == 1:
            move_entry(0x76B144)
        elif alias == 2:
            move_entry(0x76B1A8)
    return replace(case, patches=tuple(patches), regs=tuple(regs))
