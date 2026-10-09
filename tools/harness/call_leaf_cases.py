# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared additive branch and alias domains for T1475 guest-call roots."""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 26 << 40
_STATES = (0, 2, 3, 4, 5, 6, 0xFFFFFFFF)
_TIMES = ((0, 0), (1, 0), (0, 1), (0xFFFFFFFF, 1), (0x80000000, 0x7FFFFFFF))
_ELAPSED = tuple(product((0, 0x800000), _STATES, _TIMES, (0xD10000, 0xD10001)))
_RECORD = tuple(product(range(3), (0, 0x8000000, 0x800000, 0x8800000), range(6)))


def call_leaf_case_count(va: int) -> int:
    return {0x425A0: len(_ELAPSED) + 4, 0x18CE0: len(_RECORD) + 6}.get(va, 0)


def make_call_leaf_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < call_leaf_case_count(va):
        raise ValueError("unsupported guest-call leaf case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches = list(case.patches)
    regs = list(case.regs)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    if va == 0x425A0:
        if ordinal < len(_ELAPSED):
            flags, state, (now, start), destination = _ELAPSED[ordinal]
        else:
            flags, state, now, start = 0, 3, 1, 0
            destination = (0, 0xFFFFFFFF, 0x90000000, 0x6B9898)[ordinal - len(_ELAPSED)]
        word(0x7DE458, flags)
        word(0x6B98A4, state)
        word(0x6B94C0, now)
        word(0x6B989C, start)
        word(0x6F1E10, destination)
        if ordinal < len(_ELAPSED):
            word(destination + 12, 0xDEADBEEF)
    else:
        root, table = 0xD11000, 0xD12000
        index, flags, search = _RECORD[ordinal] if ordinal < len(_RECORD) else (0, 0x8000000, 2)
        word(0x4B85C8, root)
        word(root, table)
        word(case.esp + 4, index)
        word(table + index * 0x34 + 0x18, flags)
        word(0x54EF10, (-1, 0, 3, 3, 3, 3)[search])
        for slot in range(3):
            word(0x54EF30 + slot * 16, index if search == slot + 2 else 0xF0000000 + slot)
            word(0x54EF34 + slot * 16, 0xABC00000 + slot)
        if ordinal >= len(_RECORD):
            control = ordinal - len(_RECORD)
            if control == 0:
                word(0x4B85C8, 0x90000000)
            elif control == 1:
                word(root, 0x90000000)
            elif control == 2:
                word(case.esp + 4, 0x10000000)
            elif control == 3:
                word(root, case.esp - 0x20)
                regs[6] = 0x8000000  # saved ESI aliases the final output field
                word(0x54EF10, 0)
            elif control == 4:
                word(0x4B85C8, case.esp - 8)
                word(case.esp - 8, table)
                regs[6] = 0xD13000
                word(0xD13018, 0)
            else:
                word(root, case.esp - 0x1C)  # output aliases saved EBX, restored after store
                regs[3] = 0x8000000
                word(0x54EF10, 0)
    return replace(case, patches=tuple(patches), regs=tuple(regs))
