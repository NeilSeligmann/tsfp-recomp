# SPDX-License-Identifier: GPL-3.0-or-later
"""Selected-word order, mandatory reads, raw masks, aliases and true read faults."""

from dataclasses import replace
from random import Random

from .model import REG_NAMES, Case
from .seeding import SeedPolicy, derive_seed, make_case

OBJECT_MODE_FLAG_VA = 0x174E0
OBJECT_MODE_FLAG_CASES = 512
OBJECT_MODE_FLAG_INDEX_BASE = 11 << 40


def has_object_mode_flag_cases(va: int) -> bool:
    return va == OBJECT_MODE_FLAG_VA


def make_object_mode_flag_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not has_object_mode_flag_cases(va) or not 0 <= ordinal < OBJECT_MODE_FLAG_CASES:
        raise ValueError("unsupported object-mode flag address or ordinal")
    index = OBJECT_MODE_FLAG_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    shape = ordinal % 32
    bit = 1 << ((ordinal // 32 + shape) % 32)
    object_address = 0x600000 + rng.randrange(128) * 64 + (shape % 4)
    mode = (10, 0, 1, 9, 11, 255)[ordinal % 6]
    if shape < 8:
        low, high = ((bit, 0), (0, bit), (bit, bit), (0, 0))[shape % 4]
        mask_low, mask_high = ((bit, bit), (0, bit), (bit, 0), (0, 0))[shape // 2]
    else:
        low, high, mask_low, mask_high = (rng.getrandbits(32) for _ in range(4))
    regs = list(case.regs)
    patches = list(case.patches)
    patches.append((0x7DE455, bytes([mode])))
    selected = 0x10 if mode == 10 else 0x18

    def word(address: int, value: int) -> None:
        patches.append((address, value.to_bytes(4, "little")))

    if shape in (24, 25, 26):
        # Selected object words coincide with caller masks/return/argument.
        object_address = case.esp + (8, 0, 4)[shape - 24] - selected
    elif shape == 27:
        object_address = 0
        mask_low = mask_high = 0
    elif shape == 28:
        # LOW is readable but mandatory HIGH starts just outside the mapping.
        object_address = 0x00FFFFFC - selected
        mask_low = mask_high = 0
    elif shape == 29:
        regs[REG_NAMES.index("esp")] = 0x00FFFFFC
    elif shape == 30:
        # Low/high masks alias a selected global word; no stores are invented.
        regs[REG_NAMES.index("esp")] = 0x7DE448
        object_address = 0x7DE455 - selected
    if shape not in (27, 28):
        word(object_address + selected, low)
        word(object_address + selected + 4, high)
        if shape not in (24, 25, 26, 30):
            other = 0x18 if selected == 0x10 else 0x10
            word(object_address + other, low ^ 0xFFFFFFFF)
            word(object_address + other + 4, high ^ 0xFFFFFFFF)
    elif shape == 28:
        word(0x00FFFFFC, low)
    esp = regs[REG_NAMES.index("esp")]
    if shape != 29:
        word(esp, (policy or SeedPolicy()).sentinel)
        word(esp + 4, object_address)
        word(esp + 8, mask_low)
        word(esp + 12, mask_high)
    else:
        word(esp, (policy or SeedPolicy()).sentinel)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
