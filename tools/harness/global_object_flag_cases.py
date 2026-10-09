# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent global gates, raw masks, mandatory reads and bypass faults."""

from dataclasses import replace
from random import Random

from .model import REG_NAMES, Case
from .seeding import SeedPolicy, derive_seed, make_case

GLOBAL_OBJECT_FLAG_VA = 0x12EC0
GLOBAL_OBJECT_FLAG_CASES = 512
GLOBAL_OBJECT_FLAG_INDEX_BASE = 13 << 40


def has_global_object_flag_cases(va: int) -> bool:
    return va == GLOBAL_OBJECT_FLAG_VA


def make_global_object_flag_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != GLOBAL_OBJECT_FLAG_VA or not 0 <= ordinal < GLOBAL_OBJECT_FLAG_CASES:
        raise ValueError("unsupported global-object flag address or ordinal")
    index = GLOBAL_OBJECT_FLAG_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    shape = ordinal % 32
    flags = (0, 0x40, 0xBF, 0xFF)[ordinal // 32 % 4]
    state = (100, 56, 102, 0, 101, 103, 0xFFFFFFFF, 0x80000000)[shape % 8]
    mode = (10, 0, 9, 11, 255)[ordinal // 8 % 5]
    value = (0, 0x02000000, 0xFFFFFFFF, 0xFDFFFFFF, rng.getrandbits(32))[ordinal % 5]
    obj = 0x600000 + shape % 4
    regs = list(case.regs)
    if shape == 24:
        obj = case.esp + 4 - 0x28
    elif shape == 25:
        obj = 0x7DE458 - 0x28
    elif shape == 26:
        obj = 0x79094C - 0x28
    elif shape == 27:
        obj = 0
    elif shape == 28:
        obj = 0xFFFFD6
    elif shape == 29:
        obj = 0xFFFFFFD8
    elif shape == 30:
        regs[REG_NAMES.index("esp")] = 0xFFFFFC
    elif shape == 31:
        obj = 0x7DE455 - 0x28
    patches = list(case.patches)

    def word(a: int, v: int) -> None:
        patches.append((a, v.to_bytes(4, "little")))

    if shape not in (27, 28, 29, 30):
        word(obj + 0x28, value)
    patches.append((0x7DE458, bytes([flags])))
    word(0x79094C, state)
    patches.append((0x7DE455, bytes([mode])))
    esp = regs[REG_NAMES.index("esp")]
    word(esp, (policy or SeedPolicy()).sentinel)
    if shape != 30:
        word(esp + 4, obj)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
