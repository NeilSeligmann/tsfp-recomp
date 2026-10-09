# SPDX-License-Identifier: GPL-3.0-or-later
"""Dense/sparse active ordinals, full save-frame aliases and genuine fault stages."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

NTH_ACTIVE_VA = 0x38FD0
NTH_ACTIVE_CASES = 640
NTH_ACTIVE_INDEX_BASE = 19 << 40


def has_nth_active_cases(va: int) -> bool:
    return va == NTH_ACTIVE_VA


def make_nth_active_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != NTH_ACTIVE_VA or not 0 <= ordinal < NTH_ACTIVE_CASES:
        raise ValueError("unsupported nth-active address or ordinal")
    case = make_case(seed, NTH_ACTIVE_INDEX_BASE + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    base = 0x600000 + ordinal % 4
    requested = ordinal % 21
    mask = (1 << 20) - 1
    if 420 <= ordinal < 460:
        mask = (0xAAAAA, 0x55555)[ordinal % 2]
        requested = (ordinal - 420) % 20
    elif 460 <= ordinal < 500:
        mask = 1 << (ordinal % 20)
        requested = (ordinal - 460) // 20
    elif 500 <= ordinal < 520:
        mask = 0
    elif 520 <= ordinal < 540:
        requested = (0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 20)[ordinal % 4]
    elif 540 <= ordinal < 560:
        regs[4] = 0x68C6FC + 8
        regs[7] = base + (0x10000 if ordinal % 2 else 0)
    elif 560 <= ordinal < 580:
        base = case.esp - 8 - 0x17F - (ordinal % 20) * 416
    elif 580 <= ordinal < 600:
        base = case.esp + 4 - 0x17F - (ordinal % 20) * 416
    elif 600 <= ordinal < 610:
        base = 0
    elif 610 <= ordinal < 620:
        regs[4] = 0x10000
    elif 620 <= ordinal < 630:
        regs[4] = 0x10004
    elif ordinal >= 630:
        regs[4] = 0xFFFFFC
    patches = list(case.patches)

    def word(a: int, v: int) -> None:
        patches.append((a, v.to_bytes(4, "little")))

    if ordinal < 600:
        for index in range(20):
            value = (
                (1, 3, 0x81, 0xFF)[ordinal % 4]
                if mask & (1 << index)
                else (0, 2, 0x80, 0xFE)[ordinal % 4]
            )
            patches.append((base + index * 416 + 0x17F, bytes([value])))
            if 540 <= ordinal < 560:
                patches.append((regs[7] + index * 416 + 0x17F, bytes([value])))
    word(0x68C6FC, base)
    word(regs[4], (policy or SeedPolicy()).sentinel)
    if ordinal < 630:
        word(regs[4] + 4, requested)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
