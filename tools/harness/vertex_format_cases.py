# SPDX-License-Identifier: GPL-3.0-or-later
"""Prioritized flag groups, raw irrelevant bits, aliases and genuine argument faults."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

VERTEX_FORMAT_VA = 0x1E8D0
VERTEX_FORMAT_CASES = 640
VERTEX_FORMAT_INDEX_BASE = 20 << 40
RELEVANT_BITS = (0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17)
RELEVANT_MASK = sum(1 << b for b in RELEVANT_BITS)
MAX_FLAGS = sum(1 << b for b in (0, 3, 4, 5, 6, 7, 8, 12, 13, 14, 16, 17))


def has_vertex_format_cases(va: int) -> bool:
    return va == VERTEX_FORMAT_VA


def make_vertex_format_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != VERTEX_FORMAT_VA or not 0 <= ordinal < VERTEX_FORMAT_CASES:
        raise ValueError("unsupported vertex-format address or ordinal")
    case = make_case(seed, VERTEX_FORMAT_INDEX_BASE + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    flags = (ordinal * 0x9E3779B9) ^ case.regs[0]
    flags &= 0xFFFFFFFF
    if ordinal < 32:
        flags = 1 << ordinal
    elif ordinal < 64:
        flags = (1 << (ordinal - 32)) ^ 0xFFFFFFFF
    elif ordinal < 84:
        high, low = ((6, 15), (2, 7), (9, 12), (10, 13), (11, 14))[(ordinal - 64) // 4]
        choice = (ordinal - 64) % 4
        flags = ((choice & 1) << high) | (((choice >> 1) & 1) << low)
    elif ordinal < 96:
        flags = (0, 0xFFFFFFFF, MAX_FLAGS, RELEVANT_MASK)[ordinal % 4]
    elif 512 <= ordinal < 560:
        flags = (flags & ~RELEVANT_MASK) | (MAX_FLAGS if ordinal % 2 else 0)
    elif 560 <= ordinal < 600:
        regs[4] = 0xE80000 + ordinal % 4
        regs[2], regs[3], regs[6], regs[7] = regs[4] + 4, regs[4], regs[4] + 4, regs[4]
    elif ordinal >= 600:
        regs[4] = 0xFFFFFC
    patches = list(case.patches)
    patches.append((regs[4], (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    if ordinal < 600:
        patches.append((regs[4] + 4, flags.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
