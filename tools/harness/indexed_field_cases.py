# SPDX-License-Identifier: GPL-3.0-or-later
"""Branch-register, null shortcut, two-byte boundary and modular pointer controls."""

from dataclasses import replace
from random import Random

from .model import REG_NAMES, Case
from .seeding import SeedPolicy, derive_seed, make_case

INDEXED_FIELD_VA = 0x12BA0
INDEXED_FIELD_CASES = 512
INDEXED_FIELD_INDEX_BASE = 12 << 40


def has_indexed_field_cases(va: int) -> bool:
    return va == INDEXED_FIELD_VA


def make_indexed_field_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != INDEXED_FIELD_VA or not 0 <= ordinal < INDEXED_FIELD_CASES:
        raise ValueError("unsupported indexed-field address or ordinal")
    index = INDEXED_FIELD_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    shape = ordinal % 32
    flags = (0, 0x10, 0xEF, 0xFF)[ordinal // 32 % 4]
    base = (0, 1, 0x600000, 0xFFFFFFF0, 0x80000000, 0xFFFFFFFF)[shape % 6]
    element = (0, 1, 32767, 32768, 65535, rng.randrange(65536))[shape % 6]
    context, record, selector = 0x600000, 0x600100, 0x600200 + shape % 4
    regs = list(case.regs)
    if shape == 23:
        regs[REG_NAMES.index("esp")] = 0xFFFFF4
        base = 0
    if shape == 24:
        selector = case.esp + 2
    elif shape == 25:
        selector = record - 2
    elif shape == 26:
        selector = 0xFFFFF8  # Last two mapped bytes, no four-byte overread.
    elif shape == 27:
        selector = 0xFFFFFFFA
        base = 0
    elif shape == 28:
        context = 0
    elif shape == 29:
        record = 0
    elif shape == 30:
        selector = 0xFFFFF9
        base = 1
    elif shape == 31:
        selector = 0
        base = 1
    patches = list(case.patches)

    def word(address: int, value: int) -> None:
        patches.append((address, value.to_bytes(4, "little")))

    if shape != 28:
        patches.append((context + 12, bytes([flags])))
    if shape != 29:
        word(record + 4, base)
    if shape not in (27, 30, 31):
        patches.append((selector + 6, element.to_bytes(2, "little")))
    esp = regs[REG_NAMES.index("esp")]
    word(esp, (policy or SeedPolicy()).sentinel)
    word(esp + 4, context)
    word(esp + 8, record)
    if shape != 23:
        word(esp + 12, selector)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
