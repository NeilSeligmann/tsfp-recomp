# SPDX-License-Identifier: GPL-3.0-or-later
"""Seed complete request tables, untouched fields, caller frames and faults."""

from dataclasses import replace
from random import Random

from .model import REG_NAMES, Case
from .seeding import SeedPolicy, derive_seed, make_case

REQUEST_RESET_VA = 0x291A0
REQUEST_RESET_CASES = 256
REQUEST_RESET_INDEX_BASE = 10 << 40


def has_request_reset_cases(va: int) -> bool:
    return va == REQUEST_RESET_VA


def make_request_reset_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not has_request_reset_cases(va) or not 0 <= ordinal < REQUEST_RESET_CASES:
        raise ValueError("unsupported request reset address or ordinal")
    index = REQUEST_RESET_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    # Includes guards before/after all 200 68-byte records and queue globals.
    raw = bytearray(rng.randbytes(200 * 68 + 128))
    for slot in range(200):
        for offset in (0x14, 0x3C, 0x40):
            start = 32 + slot * 68 + offset
            raw[start : start + 4] = rng.choice((1, 0xFFFFFFFF, 0x80000000)).to_bytes(4, "little")
    patches = list(case.patches)
    patches.append((0x661350, bytes(raw)))
    regs = list(case.regs)
    shape = ordinal % 16
    if shape in (8, 9, 10, 11):
        # RET slot aliases an untouched table word; reset cannot restore/change it.
        regs[REG_NAMES.index("esp")] = 0x661370 + (shape - 8) * 68 + 0x20
    elif shape == 12:
        # Genuine RET target destruction: zeroed link field, no fake restoration.
        regs[REG_NAMES.index("esp")] = 0x661370 + 17 * 68 + 0x3C
    elif shape == 13:
        regs[REG_NAMES.index("esp")] = 0x00FFFFFF
    elif shape == 14:
        # Caller frame in untouched field adjacent to final queue stores.
        regs[REG_NAMES.index("esp")] = 0x6648D8
    elif shape == 15:
        regs[REG_NAMES.index("esp")] = 0x661370 + 199 * 68 + 0x24
    esp = regs[REG_NAMES.index("esp")]
    if shape != 13:
        patches.append((esp, (policy or SeedPolicy()).sentinel.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
