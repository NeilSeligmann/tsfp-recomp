# SPDX-License-Identifier: GPL-3.0-or-later
"""Signed fallback, exact two-byte indexed load, wrapped/alias/true fault cases."""

from dataclasses import replace
from random import Random

from .model import Case
from .seeding import SeedPolicy, derive_seed, make_case

SLOT_WORD_VA = 0x1E750
SLOT_WORD_CASES = 512
SLOT_WORD_INDEX_BASE = 15 << 40


def has_slot_word_cases(va: int) -> bool:
    return va == SLOT_WORD_VA


def make_slot_word_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != SLOT_WORD_VA or not 0 <= ordinal < SLOT_WORD_CASES:
        raise ValueError("unsupported slot-word address or ordinal")
    index = SLOT_WORD_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    shape = ordinal % 32
    slot = (0, 1, 32767, 0x40000000, 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000, 0xFFFFFFF0)[shape % 8]
    table = 0x600000 + shape % 4
    target = 0x610000 + shape % 4
    fallback = (0, 1, 0x80000000, 0xFFFFFFFF, rng.getrandbits(32))[ordinal % 5]
    value = (0, 1, 32767, 32768, 65535, rng.randrange(65536))[shape % 6]
    regs = list(case.regs)
    if shape == 23:
        regs[4] = 0xFFFFFC
    if shape == 24:
        target = case.esp + 4
        slot = 0
    elif shape == 25:
        table = case.esp + 4
        slot = 0x10000
        target = slot * 53 + 42
    elif shape == 26:
        target = 0xFFFFFE
        slot = 0
    elif shape == 27:
        table = 0
        slot = 0xFFFFFFFF
    elif shape == 28:
        table = 0
        slot = 1
    elif shape == 29:
        target = 0xFFFFFF
        slot = 0
    elif shape == 30:
        target = 0
        slot = 0
    elif shape == 31:
        target = 0x4E8CF4
        slot = 0
    base = (target - ((slot * 52) & 0xFFFFFFFF) - 42) & 0xFFFFFFFF
    patches = list(case.patches)

    def word(a: int, v: int) -> None:
        patches.append((a, (v & 0xFFFFFFFF).to_bytes(4, "little")))

    word(0x4B85C8, table)
    if shape not in (27, 28):
        word(table, base)
    if shape not in (29, 30):
        patches.append((target, value.to_bytes(2, "little")))
    word(0x4E8CF4, fallback)
    word(regs[4], (policy or SeedPolicy()).sentinel)
    if shape != 23:
        word(regs[4] + 4, slot)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
