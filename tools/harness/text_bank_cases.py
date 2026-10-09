# SPDX-License-Identifier: GPL-3.0-or-later
"""T1476 predeclared text-bank branch, wrapped index and stack-read aliases."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

TEXT_BANK_VA = 0x00082860
TEXT_BANK_INDEX_BASE = 40 << 40
TEXT_BANK_CASES = 18
TABLE = 0x004D9690
BANK0 = 0x00D10000
BANK1 = 0x00D11000
MASK = 0xFFFFFFFF


def text_bank_case_count(va: int) -> int:
    return TEXT_BANK_CASES if va == TEXT_BANK_VA else 0


def make_text_bank_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != TEXT_BANK_VA or not 0 <= ordinal < TEXT_BANK_CASES:
        raise ValueError("unsupported text-bank root or ordinal")
    case = make_case(seed, TEXT_BANK_INDEX_BASE + ordinal, va, size, policy=policy)
    gate, index, key, bank0, bank1 = 0, 0, 0xD87, BANK0, BANK1
    if ordinal < 7:
        gate = (-1, 0, 1, 2, 3, 4, 5)[ordinal]
    elif ordinal < 14:
        key, bank0, bank1 = (
            (0xD87, 0, BANK1),
            (0, BANK0, BANK1),
            (1, BANK0, BANK1),
            (1, BANK0, 0),
            (-1, BANK0, BANK1),
            (0xD86, BANK0, BANK1),
            (0xD88, BANK0, BANK1),
        )[ordinal - 7]
        index = (0, 1, -1)[(ordinal - 7) % 3]
    elif ordinal == 14:
        bank0 = case.esp - 8
    elif ordinal == 15:
        key, bank1 = 1, case.esp - 12
    elif ordinal == 16:
        key, bank1 = 1, case.esp - 4
    else:
        index = (((case.esp - TABLE) // 4) * pow(5, -1, 1 << 30)) % (1 << 30)
        bank0 = 0
    patches = list(case.patches)

    def word(address: int, value: int) -> None:
        patches.append((address & MASK, (value & MASK).to_bytes(4, "little")))

    for address, value in (
        (0x007B9798, gate),
        (0x007B9794, index),
        (0x006B7B08, bank0),
        (0x006B7B10, bank1),
        (0x004C0A44, 0x004766D8),
    ):
        word(address, value)
    if ordinal != 17:
        slot = (TABLE + index * 20) & MASK
        for delta in (-8, -4, 4, 8):
            address = (slot + delta) & MASK
            word(address, 0x13579BDF ^ address)
        word(slot, key)
    if ordinal < 14 and bank0 and key:
        address = (bank0 + key * 4 - 0x361C if key >= 0xD87 else bank1 + key * 4) & MASK
        if key >= 0xD87 or bank1:
            for delta in (-8, -4, 4, 8):
                neighbor = (address + delta) & MASK
                word(neighbor, 0x13579BDF ^ neighbor)
            word(address, 0x00D12000 + ordinal * 4)
    return replace(case, patches=tuple(patches))
