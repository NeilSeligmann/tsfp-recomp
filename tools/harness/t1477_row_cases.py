# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace42 domain for T1477's 0x160B80 row-key lookup root.

Layout and rationale: docs/evidence/t1477/third-checkpoint-domain.md (written before outcomes).
"""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

ROW_INDEX_BASE = 42 << 40
ROW_ROOTS = (0x160B80,)

_OBJECT = 0xD20000
_HOLDER = 0xD21000
_TABLE1 = 0xD40000
_TABLE2 = 0xD44000
_COUNT1 = 8
_COUNT2 = 4
_ROW_KEYS = tuple(1001 + 100 * index for index in range(_COUNT1)) + (0xFFFFFFFF, 0x8000)
_KEYS = (
    0,
    1000,
    0xFFFFFFFF,
    0x80000000,
    1001,
    1301,
    1701,
    1002,
    1702,
    9000,
    9001,
    9301,
    9002,
    0x7FFFFFFF,
)
_FLAGS = (0x800, 0, 0x8, 0xFFFFFFFF)
_PAIRS = ((0, 1001), (0, 1002), (7, 1701), (8, 0xFFFF), (9, 0x8000), (8, 0x7FFF))
_MODES = (0, 9, 11, 0xFF)
_MAIN = len(_KEYS) * len(_FLAGS) * len(_PAIRS)
ROW_CASES = _MAIN + 2 + len(_MODES) * 2 + 7 + 3


def row_case_count(va: int) -> int:
    return ROW_CASES if va in ROW_ROOTS else 0


def make_row_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not row_case_count(va) or not 0 <= ordinal < ROW_CASES:
        raise ValueError("unsupported T1477 row case")
    case = make_case(seed, ROW_INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)
    frame = next(data for address, data in case.patches if address == case.esp)[:8]

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    mode, null_holder, count1, table1 = 10, False, _COUNT1, _TABLE1
    key, flags, (row, entry_word) = 1001, 0x800, _PAIRS[0]
    obj_arg, holder, entry_esp = _OBJECT, _HOLDER, None
    if ordinal < _MAIN:
        key_index, rest = divmod(ordinal, len(_FLAGS) * len(_PAIRS))
        flag_index, pair_index = divmod(rest, len(_PAIRS))
        key, flags, (row, entry_word) = _KEYS[key_index], _FLAGS[flag_index], _PAIRS[pair_index]
    elif ordinal < _MAIN + 2:
        null_holder, (row, entry_word) = True, (_PAIRS[0], _PAIRS[3])[ordinal - _MAIN]
    elif ordinal < _MAIN + 2 + len(_MODES) * 2:
        index = ordinal - _MAIN - 2
        mode, (row, entry_word) = _MODES[index // 2], (_PAIRS[0], _PAIRS[3])[index % 2]
    else:
        control = ordinal - _MAIN - 2 - len(_MODES) * 2
        if control == 0:
            obj_arg = 0x90000000
        elif control == 1:
            holder = 0x90000000
        elif control == 2:
            table1 = 0x90000000
        elif control == 3:
            count1 = 0xFFFFFFFF
        elif control == 4:
            count1 = 0xFFFFFFFE
        elif control == 5:
            count1 = 0
        elif control == 6:
            row = 0x08000000
        elif control == 7:
            entry_esp = _TABLE1 + 4
        elif control == 8:
            entry_esp, key = _TABLE1 + 36, 1101
        else:
            entry_esp = _TABLE1 + 16
    table = bytearray()
    for index in range(_COUNT1 + 2):
        table += _ROW_KEYS[index].to_bytes(4, "little") + flags.to_bytes(4, "little")
        table += entry_word.to_bytes(4, "little") + bytes(20)
    patches.append((_TABLE1, bytes(table)))
    table = bytearray()
    for index in range(_COUNT2):
        table += (9001 + 100 * index).to_bytes(4, "little") + flags.to_bytes(4, "little")
        table += entry_word.to_bytes(4, "little") + bytes(20)
    patches.append((_TABLE2, bytes(table)))
    word(0x715F2C, table1)
    word(0x715F34, _TABLE2)
    word(0x7B6664, count1)
    word(0x7B65C0, _COUNT2)
    patches.append((0x7DE455, bytes([mode])))
    word(_OBJECT + 0x678, row)
    word(_OBJECT + 0xBF0, 0 if null_holder else holder)
    word(_HOLDER + 0x10, key)
    word(case.esp + 4, obj_arg)
    if entry_esp is not None:
        patches.append((entry_esp, frame))
        word(entry_esp + 4, obj_arg)
        regs[4] = entry_esp
    return replace(case, patches=tuple(patches), regs=tuple(regs))
