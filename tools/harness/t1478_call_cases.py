# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared T1478 finite byte/lookup domains; retain original random streams."""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 32 << 40
_BYTES = tuple(product(range(11), range(256)))
_REJECTED = (11, 12, 255, 0x80000000, 0xFFFFFFFF)
_CHAINS = tuple(product((-2, 0, 7), range(3), (0, 0xFFFFFFFF), range(8)))


def t1478_call_case_count(va: int) -> int:
    return {0x190AC0: len(_BYTES) + len(_REJECTED) + 2, 0x18AC60: len(_CHAINS) + 4}.get(va, 0)


def make_t1478_call_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < t1478_call_case_count(va):
        raise ValueError("unsupported T1478 call case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    if va == 0x190AC0:
        if ordinal < len(_BYTES):
            index, value = _BYTES[ordinal]
            patches.append((0x5655B0 + 48 * index, bytes([value])))
            word(case.esp + 4, index)
        elif ordinal < len(_BYTES) + len(_REJECTED):
            word(case.esp + 4, _REJECTED[ordinal - len(_BYTES)])
        else:
            index = (0, 10)[ordinal - len(_BYTES) - len(_REJECTED)]
            new_esp = 0x5655B0 + 48 * index + 8
            frame = next(data for address, data in case.patches if address == case.esp)
            patches.append((new_esp, frame))
            regs[4] = new_esp
            word(new_esp + 4, index)
    else:
        query, generation_index, initial, mode = (
            _CHAINS[ordinal] if ordinal < len(_CHAINS) else (7, 0, 0, 1)
        )
        nodes, record_base, nested = (0xD20000, 0xD20200, 0xD20400), 0xD10000, 0xD30000
        handle = (generation_index << 22) | 1
        record = record_base + generation_index * 0x2E8
        word(case.esp + 4, query)
        word(0x75D318, 0 if mode == 0 else nodes[0])
        word(0x72F5E8, record_base)
        word(record + 0x2C, handle ^ 1 if mode == 7 else handle)
        word(record + 0x7C, nested)
        word(nested + 0x114, initial)
        position = mode - 1 if mode in (1, 2, 3) else 0
        for slot, node in enumerate(nodes):
            key = query + slot - position
            if mode == 4:
                key = query - 3 + slot
            elif mode == 5:
                key = query + 1 + slot
            word(node, 0 if mode == 6 else handle)
            word(node + 4, key)
            word(node + 8, nodes[slot + 1] if slot < 2 else 0)
        if ordinal >= len(_CHAINS):
            control = ordinal - len(_CHAINS)
            if control == 0:
                word(0x75D318, 0x90000000)
            elif control == 1:
                word(0x72F5E8, 0x90000000)
            elif control == 2:
                word(record + 0x7C, 0x90000000)
            else:
                word(nodes[0] + 4, query - 1)
                word(nodes[0] + 8, nodes[0])
    return replace(case, patches=tuple(patches), regs=tuple(regs))
