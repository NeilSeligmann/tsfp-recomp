# SPDX-License-Identifier: GPL-3.0-or-later
"""Sixteen fixed flags: all counts/positions, raw bytes, and live stack aliases."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

FIXED_RECORD_VA = 0x356290
FIXED_RECORD_CASES = 512


def has_fixed_record_cases(va: int) -> bool:
    return va == FIXED_RECORD_VA


def make_fixed_record_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != FIXED_RECORD_VA or not 0 <= ordinal < FIXED_RECORD_CASES:
        raise ValueError("unsupported fixed record case")
    case = make_case(seed, (26 << 40) + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    mask = ((ordinal * 0x9E37) ^ (ordinal << 8)) & 65535
    if ordinal < 16:
        mask = 1 << ordinal
    elif ordinal < 32:
        mask = 65535 ^ (1 << (ordinal - 16))
    elif ordinal < 49:
        mask = (1 << (ordinal - 32)) - 1
    flags = [
        ((0x80 if ordinal & 1 else 0xFE) | 1) if mask & (1 << n) else (0xFE if ordinal & 2 else 0)
        for n in range(16)
    ]
    if 256 <= ordinal < 384:
        regs[4] = 0xE80000 + ordinal % 4
    elif 384 <= ordinal < 496:
        regs[4] = 0x77728C + (ordinal % 16) * 292 + ordinal % 4
    elif ordinal >= 496:
        regs[4] = 0xFFFFFC
    patches = list(case.patches)
    patches.extend((0x77728C + n * 292, bytes([v])) for n, v in enumerate(flags))
    patches.append((regs[4], (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
