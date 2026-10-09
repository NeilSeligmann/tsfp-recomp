# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite masked color search positions, raw low bytes and live stack aliases."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

COLOR_INDEX_VA = 0x3194E0
COLOR_INDEX_CASES = 512


def has_color_index_cases(va: int) -> bool:
    return va == COLOR_INDEX_VA


def make_color_index_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != COLOR_INDEX_VA or not 0 <= ordinal < COLOR_INDEX_CASES:
        raise ValueError("unsupported color index case")
    case = make_case(seed, (24 << 40) + ordinal, va, size, policy=policy)
    regs = list(case.regs)
    key = (0, 0x12345600, 0x80000000, 0xFFFFFF00)[(ordinal // 65) % 4] | (ordinal & 255)
    selected = ordinal % 65
    values = [(key ^ 0x01000000) | ((n * 19) & 255) for n in range(64)]
    if selected < 64:
        values[selected] = (key & 0xFFFFFF00) | ((ordinal + 37) & 255)
        if ordinal % 3 == 0:
            values[min(63, selected + 1)] = key
    if 300 <= ordinal < 400:
        regs[4] = 0xE80000 + ordinal % 4
    elif 400 <= ordinal < 480:
        regs[4] = 0x783A80 + (ordinal % 63) * 4 + ordinal % 4
    elif ordinal >= 480:
        regs[4] = 0xFFFFFC
    patches = list(case.patches)
    patches.append((0x783A80, b"".join(v.to_bytes(4, "little") for v in values)))
    patches.append((regs[4], (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    if ordinal < 480:
        patches.append((regs[4] + 4, key.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
