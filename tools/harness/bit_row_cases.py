# SPDX-License-Identifier: GPL-3.0-or-later
"""Additive retail-derived 13-bit row cases for T1475, never a random-stream patch.

The authentic caller at 0x002CC8EF..0x002CC94F requests columns 0, 1, 2
from both wrappers. The core at 0x00066280 tests bits 0..12. The leaf at
0x00069900 selects the fallback record for title flag0x800000 or a zero
player slot, otherwise a stride0x1F54 player record. Cross these independent
source domains rather than multiplying a lucky fixed input to meet a gate.
"""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

BIT_ROW_ROOTS = frozenset({0x66280, 0x662D0, 0x662F0})
BIT_ROW_INDEX_BASE = 25 << 40
MASKS = (0, 0x1FFF, 0xAAAA, 0x5555, 0x80000000, *(1 << bit for bit in range(13)))
_VALID_PER_ROW = 3 * 3 * len(MASKS)
_NEGATIVES = 6


def bit_row_case_count(va: int) -> int:
    if va not in BIT_ROW_ROOTS:
        return 0
    return _VALID_PER_ROW * (2 if va == 0x66280 else 1) + _NEGATIVES


def make_bit_row_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    count = bit_row_case_count(va)
    if count == 0 or not 0 <= ordinal < count:
        raise ValueError("unsupported bit-row address or ordinal")
    case = make_case(seed, BIT_ROW_INDEX_BASE + ordinal, va, size, policy=policy)
    valid_count = count - _NEGATIVES
    slot = ordinal % _VALID_PER_ROW
    row = ordinal // _VALID_PER_ROW if va == 0x66280 else int(va == 0x662F0)
    column = slot % 3
    mode = slot // 3 % 3
    mask = MASKS[slot // 9]
    player = 1
    stride = 0x1F54
    fallback = 0x6F11C0
    record = 0x7C0050 + player * stride if mode == 2 else fallback
    flags = 0x800000 if mode == 0 else 0
    active_slot = 0 if mode == 1 else 1
    if ordinal >= valid_count:
        negative = ordinal - valid_count
        row = int(va == 0x662F0)
        column = (0x10000000, 0x20000000, 0xFFFFFFFF)[negative % 3]
        if negative >= 3:
            player = (0x10000000, 0x20000000, 0xFFFFFFFF)[negative % 3]
            column = 0
            flags = 0
        else:
            flags = 0x800000
    patches = list(case.patches)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    word(0x7DE458, flags)
    if ordinal < valid_count:
        word(0x7BEDC0 + player * stride, active_slot)
        # Distinct neighboring rows/columns catch off-by-one address selection.
        base = record + (row + 0xFF) * 12
        for index in range(3):
            word(base + index * 4, mask if index == column else (~mask & 0x1FFF))
    word(case.esp + 4, player)
    if va == 0x66280:
        word(case.esp + 8, row)
        word(case.esp + 12, column)
    else:
        word(case.esp + 8, column)
    return replace(case, patches=tuple(patches))
