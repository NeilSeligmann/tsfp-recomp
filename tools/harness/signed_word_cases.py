# SPDX-License-Identifier: GPL-3.0-or-later
"""Thirteen signed-word first matches, raw keys, save-frame aliases and real faults."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

SIGNED_WORD_VA = 0x64D60
SIGNED_WORD_CASES = 512
SIGNED_WORD_INDEX_BASE = 22 << 40


def has_signed_word_cases(va: int) -> bool:
    return va == SIGNED_WORD_VA


def make_signed_word_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != SIGNED_WORD_VA or not 0 <= ordinal < SIGNED_WORD_CASES:
        raise ValueError("unsupported signed-word address or ordinal")
    case = make_case(seed, SIGNED_WORD_INDEX_BASE + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    selected = ordinal % 14
    key = (0, 1, 0x7FFF, 0xFFFF8000, 0xFFFFFFFF)[(ordinal // 14) % 5]
    words = [0x1234] * 13
    if selected < 13:
        words[selected] = key & 0xFFFF
        if ordinal % 3 == 0:
            words[min(12, selected + 1)] = key & 0xFFFF
    if 350 <= ordinal < 390:
        key = (0x8000, 0xFFFF, 0x10000, 0x80000000)[ordinal % 4]
        words = [key & 0xFFFF] * 13
    elif 390 <= ordinal < 430:
        regs[4] = 0xE80000 + ordinal % 4
    elif 430 <= ordinal < 490:
        field = 0x4D1AC6 + (ordinal % 13) * 68
        regs[4] = field + (4 if ordinal % 2 else 2)
        regs[6] = (0x00008000, 0xFFFF0001, 0xA55AFFFF)[ordinal % 3]
    elif 490 <= ordinal < 500:
        regs[4] = 0x10000
    elif ordinal >= 500:
        regs[4] = 0xFFFFFC
    patches = list(case.patches)
    for index, value in enumerate(words):
        patches.append((0x4D1AC6 + index * 68, value.to_bytes(2, "little")))
    patches.append((regs[4], (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    if ordinal < 500:
        patches.append((regs[4] + 4, key.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
