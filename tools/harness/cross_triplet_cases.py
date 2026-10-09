# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-selected signed-count triplet search, save-frame aliases and true faults."""

from dataclasses import replace
from random import Random

from .model import Case
from .seeding import SeedPolicy, derive_seed, make_case

CROSS_TRIPLET_VA = 0x18000
CROSS_TRIPLET_CASES = 512
CROSS_TRIPLET_INDEX_BASE = 14 << 40


def has_cross_triplet_cases(va: int) -> bool:
    return va == CROSS_TRIPLET_VA


def make_cross_triplet_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != CROSS_TRIPLET_VA or not 0 <= ordinal < CROSS_TRIPLET_CASES:
        raise ValueError("unsupported cross-triplet address or ordinal")
    index = CROSS_TRIPLET_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    shape = ordinal % 64
    count = (0, -1, 1, 3, 6, 8, 2, 4)[shape % 8]
    c = rng.randrange(8)
    b = (c + 1 + shape % 7) % 8
    base = 0x600000 + rng.randrange(64) * 256 + shape % 4
    args = [rng.getrandbits(32) for _ in range(3)]
    regs = list(case.regs)
    if shape == 48:
        base = case.esp - 12
        count = 1
        args = [regs[7], regs[6], regs[3]]
    elif shape == 49:
        regs[4] = 0x54E56C
        regs[7] = 0
        b = 0
        count = 3
    elif shape == 50:
        regs[4] = 0x54E5EC
        regs[3] = 0xDEAD0000
        count = 3
    elif shape == 51:
        base = case.esp + 4
        count = 1
    elif shape == 52:
        base = 0x54E560
        count = 1
    elif shape == 53:
        base = 0x54EEA4 + c * 4 - 4
        count = 3
    elif shape == 54:
        b = (0x1000000 - 0x54EEE8) // 4
        count = 1
    elif shape == 55:
        count = 1
        base = 0xFFFFFA
    elif shape == 56:
        count = 1
        base = 0
    elif shape == 57:
        count = -1
        b = (0x1000000 - 0x54EEE8) // 4
    elif shape in (58, 59, 60):
        regs[4] = 0x10000 + 4 * (shape - 58)
        count = 0
    elif shape == 61:
        regs[4] = 0xFFFFF4
        count = 1
    elif shape == 62:
        count = 1
        base = 0xFFFFFFFA
    elif shape == 63:
        c = (0x1000000 - 0x54EEA4) // 4
    patches = list(case.patches)

    def word(a: int, v: int) -> None:
        patches.append((a, (v & 0xFFFFFFFF).to_bytes(4, "little")))

    if shape not in (54, 55, 56, 57, 58, 59, 60, 61, 62, 63):
        match = (shape // 8) % max(1, count)
        for i in range(max(0, count)):
            fields = [v ^ (0 if i == match else 1) for v in args]
            if shape // 8 == 3:
                fields[1] ^= 1
            elif shape // 8 == 4:
                fields[2] ^= 1
            elif shape // 8 == 5:
                fields[0] ^= 1
            result = 0 if shape // 8 == 2 else 0xBEEF0000 + i
            for off, v in zip((0, 4, 8, 16), fields + [result], strict=True):
                word(base + i * 28 + off, v)
        if shape // 8 == 2 and count > 1:
            for off, v in zip((0, 4, 8, 16), args + [0x12345678], strict=True):
                word(base + (count - 1) * 28 + off, v)
    word(0x54E5E8, c)
    word(0x54E560, b)
    if shape != 63:
        word(0x54EEA4 + c * 4, count)
    if shape not in (54, 57):
        word(0x54EEE8 + b * 4, base)
    esp = regs[4]
    word(esp, (policy or SeedPolicy()).sentinel)
    for n, v in enumerate(args):
        if shape != 61 or n < 2:
            word(esp + 4 + n * 4, v)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
