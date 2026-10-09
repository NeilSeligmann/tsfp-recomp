# SPDX-License-Identifier: GPL-3.0-or-later
"""Signed-count dword search, lazy key reads, full frame aliases and genuine faults."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

INT_ARRAY_VA = 0x70C90
INT_ARRAY_CASES = 640
INT_ARRAY_INDEX_BASE = 23 << 40


def has_int_array_cases(va: int) -> bool:
    return va == INT_ARRAY_VA


def make_int_array_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != INT_ARRAY_VA or not 0 <= ordinal < INT_ARRAY_CASES:
        raise ValueError("unsupported int-array address or ordinal")
    case = make_case(seed, INT_ARRAY_INDEX_BASE + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    pointer = 0x600000 + ordinal % 4
    count = 1 + ordinal % 64
    selected = (ordinal // 64) % 4
    selected = (0, count // 2, count - 1, count)[selected]
    key = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xA55A1234)[ordinal % 6]
    values = [(key ^ 0x12345678) & 0xFFFFFFFF] * count
    if selected < count:
        values[selected] = key
        if selected + 1 < count:
            values[selected + 1] = key
    if 350 <= ordinal < 400:
        values = [key ^ 0x80000000] * count
        if selected < count:
            values[selected] = key
    elif 400 <= ordinal < 440:
        count = (0, 0xFFFFFFFF, 0x80000000)[ordinal % 3]
        pointer = 0 if ordinal % 4 == 0 else pointer
    elif 440 <= ordinal < 480:
        regs[4] = 0xE80000 + ordinal % 4
    elif 480 <= ordinal < 520:
        pointer = case.esp - 4
        regs[6] = (1, 2, 4, 0, 0xFFFFFFFF)[ordinal % 5]
        key = (policy or SeedPolicy()).sentinel
        count = 3
        values = []
    elif 520 <= ordinal < 560:
        pointer = case.esp - 8 - 4 * (ordinal % 5)
        count = 8
        values = []
    elif 560 <= ordinal < 580:
        regs[4] = 0xFFFFF8
        count = (0, 0xFFFFFFFF, 0x80000000)[ordinal % 3]
        pointer = 0 if ordinal % 4 == 0 else pointer
    elif 580 <= ordinal < 590:
        regs[4] = 0x10000
    elif 590 <= ordinal < 600:
        regs[4] = 0xFFFFFC
    elif 600 <= ordinal < 610:
        pointer = 0x1000000
    elif 610 <= ordinal < 620:
        pointer = 0xFFFFFC
        count = 1
        values = []
    elif 620 <= ordinal < 630:
        regs[4] = 0xFFFFF8
        count = 1
    elif ordinal >= 630:
        count = 0x7FFFFFFF
        key = 0xDEADBEEF
        values = []
    patches = list(case.patches)
    if pointer and pointer < 0x1000000:
        patches.append((pointer, count.to_bytes(4, "little")))
        for index, value in enumerate(values):
            if pointer + 4 + index * 4 < 0x1000000:
                patches.append((pointer + 4 + index * 4, value.to_bytes(4, "little")))
        if ordinal >= 630:
            patches.append((pointer + 4, bytes(0x40000)))
    patches.append((regs[4], (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    if ordinal < 590 or ordinal >= 600:
        patches.append((regs[4] + 4, pointer.to_bytes(4, "little")))
    if regs[4] + 8 < 0x1000000:
        patches.append((regs[4] + 8, key.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
