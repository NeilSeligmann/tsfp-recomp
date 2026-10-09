# SPDX-License-Identifier: GPL-3.0-or-later
"""Request-index units, idle/busy words and stack-alias input controls."""

from dataclasses import replace
from random import Random

from .model import REG_NAMES, Case
from .seeding import SeedPolicy, derive_seed, make_case

REQUEST_SLOT_VA = 0x294B0
REQUEST_SLOT_CASES = 512
REQUEST_SLOT_INDEX_BASE = 9 << 40


def has_request_slot_cases(va: int) -> bool:
    return va == REQUEST_SLOT_VA


def make_request_slot_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not has_request_slot_cases(va) or not 0 <= ordinal < REQUEST_SLOT_CASES:
        raise ValueError("unsupported request-slot address or ordinal")
    index = REQUEST_SLOT_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    shape = ordinal % 32
    request_index = (
        (0, 1, 2, 198, 199, 200, 201, 255, 511, 512, 65535)[shape]
        if shape < 11
        else rng.randrange(200)
    )
    request_id = request_index | (rng.choice((0, 1, 0x8000, 0xFFFF)) << 16)
    patches = list(case.patches)
    regs = list(case.regs)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    # Correct word and plausible wrong-unit words disagree by construction.
    busy = rng.choice((0, 1, 2048, 0x80000000, 0xFFFFFFFF))
    for slot in range(200):
        word(0x66139C + slot * 68, 0xAA550000 | slot)
    if request_index < 200:
        word(0x66139C + request_index * 44, 0 if busy else 0xFF)
        word(0x66139C + request_index * 68, busy)
    word(case.esp + 4, request_id)
    if shape in (24, 25, 26):
        # Caller frame itself lives within a selected request slot, with exact
        # RET sentinel and argument; no helper writes or restores are invented.
        request_index = 17 + shape - 24
        address = 0x66139C + request_index * 68
        entry_esp = address + (8, 0, -4)[shape - 24]
        regs[REG_NAMES.index("esp")] = entry_esp
        request_id = request_index | 0xFFFF0000
        word(address, busy)
        word(entry_esp, (policy or SeedPolicy()).sentinel)
        word(entry_esp + 4, request_id)
    elif shape in (27, 28):
        regs[REG_NAMES.index("esp")] = 0x00FFFFFC if shape == 27 else 0xFFFFFFFF
        if shape == 27:
            word(0x00FFFFFC, (policy or SeedPolicy()).sentinel)
    return replace(case, regs=tuple(regs), patches=tuple(patches), df=ordinal % 2)
