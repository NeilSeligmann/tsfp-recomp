# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace 54 domains for T1479's third bounded batch (twelve net/session roots).

Contracts and ordinal layouts: docs/evidence/t1479/third-batch-predeclaration.md (written and
committed before any audit, native run or proof outcome). Every ordinal's global/table state
and stack frame derive from the declared tuples below, never from observed results. Registers
and unpatched argument words keep the seed stream values.
"""

import hashlib
from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

BATCH3_INDEX_BASE = 54 << 40

_NET = 0x76B108
_MODE = 0x79094C
_STATE = 0x76B140
_SESSION = 0x774028
_LIMIT = 0x790950
_ROSTER_COUNT = 0x7A2958
_ROSTER = 0x7DE4C0
_RECORDS = 0x7356D8
_SLOT_FLAGS = 0x77728C
_SLOT_POINTERS = 0x777244
_SLOT_STRIDE = 0x124
_RECORD_STRIDE = 0xEA0

_A = 0xD70000  # argument object
_B = 0xD70200  # [A+0x7C] object
_E = 0xD70400  # [A+0x14] object (35AFB0), second argument object (35B140)
_KB = 0xD70300  # [A+0x7C] kind object (35B140)
_P = 0xD70600  # [E+0x150] object
_S = 0xD70800  # session object (774028)
_Q = 0xD70900  # session records, stride 0x40
_R = 0xD80000  # record table base (7356D8) / roster records
_X = 0xD74000  # per-slot entry objects, stride 0x40
_VL = ((5, 5), (4, 5), (6, 5), (0x80000000, 1), (0, 0x80000000), (0x7FFFFFFF, 0xFFFFFFFF))
#: (mode 79094C, state 76B140) combinations that decide 355EC0.
_MODE_STATES = ((0x65, 1), (0x65, 2), (0x64, 1), (0x64, 2))
_KEYS = (0x11, 0x22, 0x33, 0x44)
#: 35AFB0 / 35B140 object cases: P (+0x150 object, or 0), [P+4], +0x20, ([B+8], limit).
_E_CASES = (
    ("null",),
    ("p", 5),
    ("p", 0x80000000),
    ("p", 0),
    ("t8", 5),
    ("t8", 4),
    ("kind", 0x200),
    ("kind", 3),
)

_TYPES5 = ("neg", "match", "other", "idle", "norecord")
_ROSTER_SHAPES = ((0, 0), (1, 0), (1, 1), (2, 1), (1, 3))


def _build_specs() -> dict[int, tuple[int, int]]:
    roster = 0
    for length in range(4):
        roster += 5**length
    roster += 3**4
    return {
        0x2BE9B0: (0, 7 * 3 * 3 * 5 * 2 + 4),
        0x2D28F0: (0, 3 * 8 + 5 * 2 + 1 + 192),
        0x358130: (0, 2 * 2 * 3 * 4 + 3),
        0x358160: (0, 2 * 2 * 3 * 5 * 2 * 2 + 4),
        0x356080: (1, 2 * 6 * 7 + 7 + 1),
        0x356870: (1, 12 + 3 + 3 + 1),
        0x358560: (1, 2 + 6 * 4 + 2),
        0x35AFB0: (1, 2 * 2 * 8 * 4 + 1),
        0x35B140: (2, 2 * 2 * 3 * 8 * 4 + 2),
        0x359910: (1, 48 + 16 + 9 + 1 + 4 + 3 + 120),
        0x2E7270: (1, 3 * roster),
        0x3560E0: (2, 9 * 4 * 3),
    }


_SPEC = _build_specs()
_CLASSES = (
    (0, 0x64, 0, 0, 0, 0),
    (1, 0x65, 0, 0, 0, 0),
    (1, 0x64, 0, 0, 0, 0),
    (1, 0x64, _A, 0, 1, 0),
    (1, 0x64, _A, _Q, 5, 1),
    (1, 0x64, _A, _Q, 1, 1),
    (1, 0x64, _A, _Q, 1, 2),
    (1, 0x64, _A, _Q, 1, 0xFFFFFFFF),
)
#: Player index the original 69640 -> 356800(0) chain yields for each class.
_CLASS_INDEX = (0, 0, 0, 0, 0, 1, 2, 0xFFFFFFFF)


def batch3_case_count(va: int) -> int:
    return _SPEC.get(va, (0, 0))[1]


def roster_scenario(ordinal: int) -> tuple[int, int, tuple[str, ...]]:
    """`(group, length, per-entry types)` of the 2E7270 ordinals; group 0 mode 0x65, 1 mode
    0x64 with records, 2 network off."""
    group, rest = divmod(ordinal, batch3_case_count(0x2E7270) // 3)
    for length, count in enumerate((1, 5, 25, 125)):
        if rest < count:
            digits = []
            for _ in range(length):
                rest, digit = divmod(rest, 5)
                digits.append(_TYPES5[digit])
            return group, length, tuple(digits)
        rest -= count
    digits = []
    for _ in range(4):
        rest, digit = divmod(rest, 3)
        digits.append(_TYPES5[1 + digit])
    return group, 4, tuple(digits)


class _Builder:
    def __init__(self, case: Case, arguments: int) -> None:
        self.case = case
        self.patches = list(case.patches)
        self.regs = list(case.regs)
        frame = next(data for address, data in case.patches if address == case.esp)
        self.frame = frame[: 4 + 4 * arguments]
        self.esp = case.esp

    def word(self, address: int, value: int) -> None:
        self.patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    def half(self, address: int, value: int) -> None:
        self.patches.append((address & 0xFFFFFFFF, (value & 0xFFFF).to_bytes(2, "little")))

    def byte(self, address: int, value: int) -> None:
        self.patches.append((address & 0xFFFFFFFF, bytes([value & 0xFF])))

    def argument(self, slot: int, value: int) -> None:
        self.word(self.esp + 4 + 4 * slot, value)

    def move_entry(self, new_esp: int, *values: int) -> None:
        self.patches.append((new_esp, self.frame))
        self.esp = new_esp
        self.regs[4] = new_esp
        for slot, value in enumerate(values):
            self.word(new_esp + 4 + 4 * slot, value)

    def net_state(self, network: int, mode: int, state: int) -> None:
        self.word(_NET, network)
        self.word(_MODE, mode)
        self.word(_STATE, state)

    def build(self) -> Case:
        return replace(self.case, patches=tuple(self.patches), regs=tuple(self.regs))


def _session(builder: _Builder, count: int, layout: int) -> None:
    """Session object `_S`: count at +0x20, pointers at +0xC.., records `_Q + 0x40 i`."""
    builder.word(_SESSION, _S)
    builder.word(_S + 0x20, count)
    builder.word(_LIMIT, 3)
    keys = _KEYS if layout == 0 else (0x11, 0x22, 0x22, 0x44)
    for index in range(4):
        builder.word(_S + 0xC + 4 * index, _Q + 0x40 * index)
        builder.word(_Q + 0x40 * index + 0x3C, keys[index])


def _object_case(builder: _Builder, kind: tuple, address: int) -> None:
    """Fill the E object at `address` (35AF40/358560 inputs) per `_E_CASES`."""
    builder.word(_LIMIT, 5)
    builder.word(address + 0x7C, _B)
    builder.word(address + 0x150, 0)
    builder.word(address + 0x20, 3)
    builder.word(_B + 8, 5)
    if kind[0] == "p":
        builder.word(address + 0x150, _P)
        builder.word(_P + 4, kind[1])
    elif kind[0] == "t8":
        builder.word(address + 0x20, 8)
        builder.word(_B + 8, kind[1])
    elif kind[0] == "kind":
        builder.word(address + 0x20, kind[1])


def _digits(value: int, *bases: int) -> list[int]:
    out = []
    for base in bases:
        value, digit = divmod(value, base)
        out.append(digit)
    return out


def make_batch3_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    arguments, total = _SPEC.get(va, (0, 0))
    if not 0 <= ordinal < total:
        raise ValueError("unsupported T1479 third-batch case")
    case = make_case(seed, BATCH3_INDEX_BASE + ordinal, va, size, policy=policy)
    builder = _Builder(case, arguments)
    word, byte = builder.word, builder.byte

    if va == 0x2BE9B0:
        grid = 7 * 3 * 3 * 5 * 2
        alias = None
        cls, mode_byte, row, value, flag = 3, 1, 1, 2, 0
        if ordinal < grid:
            cls, mode_index, row_index, value_index, flag = _digits(ordinal, 7, 3, 3, 5, 2)
            cls = (0, 1, 2, 3, 5, 6, 7)[cls]
            mode_byte, row = (0, 1, 255)[mode_index], (0, 1, 255)[row_index]
            value = (0, 1, 2, 3, 0xFFFFFFFF)[value_index]
        else:
            alias = ordinal - grid
        network, mode, session, p_c, p_20, q_3c = _CLASSES[cls]
        word(_NET, network)
        word(_MODE, mode)
        word(_SESSION, session)
        if session:
            word(_A + 0xC, p_c)
            word(_A + 0x20, p_20)
            word(_LIMIT, 3)
            if p_c:
                word(_Q + 0x3C, q_3c)
        base = (0x7BEE40 + _CLASS_INDEX[cls] * 0x1F54) & 0xFFFFFFFF
        byte(0x78ADAD, mode_byte)
        byte(0x78ADAE, row)
        word(0x4D1F84 + row * 32, value)
        word(base + mode_byte * 20 + 0x24, flag)
        if alias is not None:
            builder.move_entry((0x78ADAD, 0x78ADAE, _NET, _MODE)[alias] + 4)
    elif va == 0x2D28F0:
        values = (1, 2, 3, 0, 4, 0xFFFFFFFF, 0x80000000, 0x100)
        reps = (8, 8, 8, 2, 2, 2, 2, 2)
        sequence = [v for v, count in zip(values, reps, strict=True) for _ in range(count)]
        if ordinal < len(sequence):
            word(0x76BBC8, sequence[ordinal])
        elif ordinal == len(sequence):
            word(0x76BBC8, 1)
            builder.move_entry(0x76BBC8 + 4)
        else:
            # Amendment A1 (written AFTER the default domain failed null-agree at 0.9293):
            # 192 more answer-bearing ordinals, 64 each for the values 1, 2 and 3.
            word(0x76BBC8, (1, 2, 3)[(ordinal - len(sequence) - 1) // 64])
    elif va == 0x358130:
        if ordinal < 48:
            network, mode_index, state, flag = _digits(ordinal, 2, 2, 3, 4)
            builder.net_state(network, (0x65, 0x64)[mode_index], state)
            word(0x541E9C, (0, 1, 5, 0xFFFFFFFF)[flag])
        else:
            builder.net_state(1, 0x64, 2)
            word(0x541E9C, 5)
            builder.move_entry((0x541E9C, _NET, _STATE)[ordinal - 48] + 4)
    elif va == 0x358160:
        grid = 2 * 2 * 3 * 5 * 2 * 2
        if ordinal < grid:
            network, mode_index, state, mark, a4, c750 = _digits(ordinal, 2, 2, 3, 5, 2, 2)
            builder.net_state(network, (0x65, 0x64)[mode_index], state)
            word(0x7DE30C, (6, 7, 0, 5, 0xFFFFFFFF)[mark])
            word(0x541EA4, (0, 5)[a4])
            word(0x519750, (0, 0x7FFFFFFF)[c750])
        else:
            builder.net_state(1, 0x64, 2)
            word(0x7DE30C, 7)
            word(0x541EA4, 5)
            word(0x519750, 3)
            builder.move_entry((0x519750, 0x541EA4, 0x7DE30C, _STATE)[ordinal - grid] + 4)
    elif va == 0x356080:
        args = (0x11, 0x22, 0x33, 0x44, 0x55, 0, 0xFFFFFFFF)
        counts = (0, 1, 3, 4, 0xFFFFFFFF, 0x80000000)
        main = 2 * 6 * 7
        if ordinal < main:
            layout, count_index, arg_index = _digits(ordinal, 2, 6, 7)
            builder.net_state(1, 0x64, 2)
            _session(builder, counts[count_index], layout)
            builder.argument(0, args[arg_index])
        elif ordinal < main + 7:
            builder.net_state(0, 0x64, 2)
            word(_SESSION, 0)
            builder.argument(0, args[ordinal - main])
        else:
            builder.net_state(1, 0x64, 2)
            _session(builder, 3, 0)
            builder.move_entry(_NET + 4, 0x22)
    elif va == 0x356870:
        if ordinal < 12:
            pattern, arg_index = divmod(ordinal, 6)
            builder.net_state(1, 0x64, 2)
            word(_SESSION, _S)
            slots = (
                (0, _Q, _Q + 0x40, _Q + 0x80, _Q + 0xC0, 0),
                (_Q + 0xC0, 0, _Q + 0x40, 0, _Q + 0xC0, _Q),
            )[pattern]
            for slot, pointer in zip((-1, 0, 1, 2, 3, 4), slots, strict=True):
                word(_S + 0xC + 4 * slot, pointer)
            for index in range(4):
                word(_Q + 0x40 * index + 0x3C, _KEYS[index] + 0x100 * pattern)
            builder.argument(0, (0, 1, 2, 3, 0xFFFFFFFF, 4)[arg_index])
        elif ordinal < 15:
            builder.net_state(1, 0x64, 2)
            word(_SESSION, 0)
            builder.argument(0, (0, 2, 0xFFFFFFFF)[ordinal - 12])
        elif ordinal < 18:
            builder.net_state(0, 0x64, 2)
            word(_SESSION, _S)
            builder.argument(0, (0, 2, 0xFFFFFFFF)[ordinal - 15])
        else:
            builder.net_state(1, 0x64, 2)
            word(_SESSION, _S)
            word(_S + 0xC + 8, _Q)
            word(_Q + 0x3C, 0x77)
            builder.move_entry(_NET + 4, 2)
    elif va == 0x358560:
        word(_A + 0x7C, _B)
        builder.argument(0, _A)
        if ordinal < 2:
            builder.net_state(0, 0x64, 2)
            word(_B + 8, 5)
            word(_LIMIT, 5)
            builder.argument(0, (_A, _A + 0x40)[ordinal])
            word(_A + 0x40 + 0x7C, _B)
        elif ordinal < 26:
            pair, mode_state = divmod(ordinal - 2, 4)
            builder.net_state(1, *_MODE_STATES[mode_state])
            word(_B + 8, _VL[pair][0])
            word(_LIMIT, _VL[pair][1])
        else:
            builder.net_state(1, 0x64, 2)
            word(_B + 8, 5)
            word(_LIMIT, 5)
            builder.move_entry((_LIMIT, _NET)[ordinal - 26] + 4, _A)
    elif va == 0x35AFB0:
        grid = 2 * 2 * 8 * 4
        if ordinal < grid:
            network, flag, kind, mode_state = _digits(ordinal, 2, 2, 8, 4)
            builder.net_state(network, *_MODE_STATES[mode_state])
            word(0x76B180, flag)
            builder.argument(0, _A)
            word(_A + 0x14, 0 if kind == 0 else _E)
            if kind:
                _object_case(builder, _E_CASES[kind], _E)
        else:
            builder.net_state(1, 0x64, 2)
            word(0x76B180, 0)
            word(_A + 0x14, _E)
            _object_case(builder, _E_CASES[1], _E)
            builder.move_entry(_NET + 4, _A)
    elif va == 0x35B140:
        grid = 2 * 2 * 3 * 8 * 4
        kinds = (0x20, 1, 5)

        def kind_object(kind_word: int, case_index: int) -> None:
            builder.word(_A + 0x7C, _KB)
            builder.word(_KB + 0xC, kind_word)
            if case_index:
                _object_case(builder, _E_CASES[case_index], _E)

        if ordinal < grid:
            network, flag, kind, case_index, mode_state = _digits(ordinal, 2, 2, 3, 8, 4)
            builder.net_state(network, *_MODE_STATES[mode_state])
            word(0x76B16C, flag)
            kind_object(kinds[kind], case_index)
            builder.argument(0, _A)
            builder.argument(1, 0 if case_index == 0 else _E)
        else:
            builder.net_state(1, 0x64, 2)
            word(0x76B16C, 0)
            kind_object(5, 1)
            builder.move_entry((_NET, 0x76B16C)[ordinal - grid] + 4, _A, _E)
    elif va == 0x359910:
        _batch_359910(builder, ordinal)
    elif va == 0x2E7270:
        _roster_2e7270(builder, ordinal)
    else:
        indices = (0, 1, 2, 7, 15, 16, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000)
        values = (0, 1, 0xFFFFFFFF, 0x12345678)
        flags = (0, 0x10, 0xFFFFFFFF)
        index_index, value_index, flag_index = _digits(ordinal, 9, 4, 3)
        index = indices[index_index]
        word(0x77728C + ((index * _SLOT_STRIDE) & 0xFFFFFFFF), flags[flag_index])
        builder.argument(0, index)
        builder.argument(1, values[value_index])
    return builder.build()


def _batch_359910(builder: _Builder, ordinal: int) -> None:
    """Sixteen-slot minimum-weight scan: slots connected per scenario, key and weight sets."""
    word, byte = builder.word, builder.byte
    key = 0x1234
    network, mode = 1, 0x65
    connected: dict[int, tuple[int, int | None]] = {}  # slot: (entry key, entry pointer)
    weights = [(index * 37 + 11) & 0x7FFF for index in range(16)]
    record_zero = False
    base_zero = False
    arg_pointer = _A
    if ordinal < 48:
        slot_index, key_mode, weight_index = _digits(ordinal, 4, 3, 4)
        slot = (0, 1, 7, 15)[slot_index]
        weights[slot] = (0x7FFF, 5, 0x8000, 0)[weight_index]
        other = 0x1235
        connected[slot] = (key if key_mode == 1 else other, _X + 0x40 * slot)
        if key_mode == 2:
            arg_pointer = _X + 0x40 * slot
            connected[slot] = (key, _X + 0x40 * slot)
    elif ordinal < 64:
        pair, mask = _digits(ordinal - 48, 4, 4)
        first, second = ((5, 7), (7, 5), (5, 5), (0x8000, 3))[pair]
        weights[3], weights[9] = first, second
        connected[3] = (key if mask & 1 else 0x1235, _X + 0x40 * 3)
        connected[9] = (key if mask & 2 else 0x1235, _X + 0x40 * 9)
    elif ordinal < 73:
        weight_set, key_mode = _digits(ordinal - 64, 3, 3)
        weights = (
            [(index * 37 + 11) & 0x7FFF for index in range(16)],
            [100 - index * 7 for index in range(16)],
            [9] * 16,
        )[weight_set]
        for slot in range(16):
            equal = key_mode == 1 or (key_mode == 2 and slot == 5)
            connected[slot] = (key if equal else 0x1235, _X + 0x40 * slot)
    elif ordinal == 73:
        pass
    elif ordinal < 78:
        variant = ordinal - 74
        mode = 0x64
        record_zero = variant in (0, 1)
        base_zero = variant in (1, 2)
        connected[2] = (0x1235, _X + 0x40 * 2)
        weights[2] = 5
        if variant == 3:
            record_zero = False
    elif ordinal < 81:
        network = 0
        variant = ordinal - 78
        weights = [100 - index * 7 for index in range(16)]
        for slot in range(16):
            equal = variant == 1 or (variant == 2 and slot == 0)
            connected[slot] = (key if equal else 0x1235, _X + 0x40 * slot)
    else:
        # Amendment A2 (written AFTER the default domain gave 81 verdicts, below the 100
        # gate): 120 hash-derived scenarios, mode 0x65, network on: a connected mask, a
        # key-equal mask and sixteen signed 16-bit weights per ordinal.
        stream = b"".join(
            hashlib.sha256(f"t1479-359910-a2:{ordinal}:{block}".encode()).digest()
            for block in range(2)
        )
        mask = int.from_bytes(stream[0:2], "little")
        equal_mask = int.from_bytes(stream[2:4], "little") & int.from_bytes(stream[4:6], "little")
        weights = [
            int.from_bytes(stream[8 + 2 * slot : 10 + 2 * slot], "little") for slot in range(16)
        ]
        for slot in range(16):
            if (mask >> slot) & 1:
                equal = (equal_mask >> slot) & 1
                connected[slot] = (key if equal else 0x1235, _X + 0x40 * slot)
    word(_NET, network)
    word(_MODE, mode)
    word(_RECORDS, 0 if base_zero else _R)
    for slot in range(16):
        flagged = slot in connected and network == 1
        byte(_SLOT_FLAGS + slot * _SLOT_STRIDE, 1 if flagged else 0)
        if slot in connected:
            entry_key, pointer = connected[slot]
            word(_SLOT_POINTERS + slot * _SLOT_STRIDE, pointer)
            if pointer != arg_pointer:
                word(pointer, entry_key)
        if not base_zero:
            record = _R + slot * _RECORD_STRIDE
            word(record, 0 if record_zero else 0x11223344)
            builder.half(record + 0xA60, weights[slot])
    word(arg_pointer, key)
    builder.argument(0, arg_pointer)


def _roster_2e7270(builder: _Builder, ordinal: int) -> None:
    word, byte = builder.word, builder.byte
    group, length, types = roster_scenario(ordinal)
    network, mode = ((1, 0x65), (1, 0x64), (0, 0x64))[group]
    argument = 0x40
    word(_NET, network)
    word(_MODE, mode)
    word(_RECORDS, _R)
    first, second = _ROSTER_SHAPES[length]
    word(_ROSTER_COUNT, first)
    word(_LIMIT, second)
    for index in range(4):
        roster_id = 0xFFFFFFFF
        slot = index + 2
        word(_ROSTER + 4 * index, roster_id)
        if index >= length:
            continue
        kind = types[index]
        if kind == "neg":
            continue
        roster_id = index + 1
        word(_ROSTER + 4 * index, roster_id)
        record = _R + roster_id * _RECORD_STRIDE
        word(record + 8, slot)
        word(record + 0x84, argument if kind in ("match", "idle", "norecord") else argument + 1)
        flagged = kind != "idle"
        byte(_SLOT_FLAGS + slot * _SLOT_STRIDE, 1 if flagged else 0)
        word(_R + slot * _RECORD_STRIDE, 0 if kind == "norecord" else 0x11223344)
    builder.argument(0, argument)
