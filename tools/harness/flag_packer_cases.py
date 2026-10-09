# SPDX-License-Identifier: GPL-3.0-or-later
"""Raw truth combinations, staged reads and live stack/structure aliases."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

FLAG_PACKER_VA = 0x31DCD0
FLAG_PACKER_CASES = 512


def has_flag_packer_cases(va: int) -> bool:
    return va == FLAG_PACKER_VA


def make_flag_packer_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != FLAG_PACKER_VA or not 0 <= ordinal < FLAG_PACKER_CASES:
        raise ValueError("unsupported flag packer case")
    case = make_case(seed, (25 << 40) + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    pointer = 0x600000 + ordinal % 4
    raw = (1, 0xFFFFFFFF, 0x80000000, 0x100, 0x10000, 0x7FFFFFFF, 0x1000000, 0xFF)[
        (ordinal // 16) % 8
    ]
    values = [raw if ordinal & (1 << n) else 0 for n in range(4)]
    if 256 <= ordinal < 384:
        regs[4] = 0xE80000 + ordinal % 4
    elif 384 <= ordinal < 480:
        pointer = regs[4] - (ordinal % 6) * 4 + ordinal % 4
    elif 480 <= ordinal < 496:
        regs[4] = 0xFFFFFC
    elif ordinal < 504 and ordinal >= 496:
        pointer = 0xFFFFF0
    elif 504 <= ordinal < 508:
        pointer = 0xFFF0
    elif ordinal >= 508:
        pointer = 0xFFF8
    patches = list(case.patches)
    for offset, value in zip((0x10, 0xC, 4, 8), values, strict=True):
        address = (pointer + offset) & 0xFFFFFFFF
        if 0x10000 <= address <= 0xFFFFFC:
            patches.append((address, value.to_bytes(4, "little")))
    patches.append((regs[4], (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    if ordinal < 480 or ordinal >= 496:
        patches.append((regs[4] + 4, pointer.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
