# SPDX-License-Identifier: GPL-3.0-or-later
"""Ordered two-output stores, captured size, aliases and genuine faults."""

from dataclasses import replace
from random import Random

from .model import Case
from .seeding import SeedPolicy, derive_seed, make_case

BUFFER_OUTPUT_VA = 0x251F0
BUFFER_OUTPUT_CASES = 512
BUFFER_OUTPUT_INDEX_BASE = 17 << 40


def has_buffer_output_cases(va: int) -> bool:
    return va == BUFFER_OUTPUT_VA


def make_buffer_output_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != BUFFER_OUTPUT_VA or not 0 <= ordinal < BUFFER_OUTPUT_CASES:
        raise ValueError("unsupported buffer-output address or ordinal")
    index = BUFFER_OUTPUT_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    shape = ordinal % 16
    rng = Random(derive_seed(seed, index))
    value = (0, 1, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF, rng.getrandbits(32))[ordinal % 6]
    regs = list(case.regs)
    first, second = 0x600000 + ordinal % 4, 0x610000 + ordinal % 4
    if shape == 1:
        second = first
    elif shape == 2:
        first = 0x5655A8
    elif shape == 3:
        second = 0x5655A8
    elif shape == 4:
        first = case.esp + 4
    elif shape == 5:
        first = case.esp + 8
    elif shape == 6:
        second = case.esp + 4
    elif shape == 7:
        second = case.esp + 8
    elif shape == 8:
        first = case.esp
    elif shape == 9:
        second = case.esp
    elif shape in (10, 12):
        first = 0
    if shape in (11, 12):
        second = 0
    if shape == 13:
        regs[4] = 0xFFFFFC
    elif shape == 14:
        regs[4] = 0xFFFFF8
    elif shape == 15:
        first, second = 0x610001, 0x610002
    patches = list(case.patches)

    def word(a: int, v: int) -> None:
        patches.append((a, v.to_bytes(4, "little")))

    word(0x5655A8, value)
    word(regs[4], (policy or SeedPolicy()).sentinel)
    if shape != 13:
        word(regs[4] + 4, first)
    if shape not in (13, 14):
        word(regs[4] + 8, second)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
