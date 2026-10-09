# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace35 domains for T1478's second scalar call batch."""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 35 << 40
_BOOL = tuple(product(range(8), (0, 1, 1023), (0, 1, 2, 0xFFFFFFFF), range(2)))
_COPY = tuple(product(range(8), (0, 1, 1023), range(4)))
_ALIAS = tuple(product(range(4), (-4, 0, 4, 8, 12)))
_LOCK = tuple(
    product((0, 10, 255), (0, 0x38, 0x64, 0xFFFFFFFF), (0, 0x40), range(2), (0, 0x11, 0x12))
)
_PATTERNS = (
    (0, 0, 0),
    (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF),
    (0x80000000, 0x7FFFFFFF, 1),
    (0x11223344, 0x55667788, 0x99AABBCC),
)


def t1478_followup_case_count(va: int) -> int:
    return {
        0x1EC8E0: len(_BOOL) + 4,
        0x24D260: len(_COPY) + len(_ALIAS) + 4,
        0x259800: len(_LOCK) + 4,
    }.get(va, 0)


def make_t1478_followup_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < t1478_followup_case_count(va):
        raise ValueError("unsupported T1478 followup case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    if va == 0x259800:
        mode, level, flags, nested, action = (
            _LOCK[ordinal] if ordinal < len(_LOCK) else (0, 0, 0, 1, 0)
        )
        obj, child = 0xD20000, 0xD30000
        word(case.esp + 4, obj)
        word(obj + 0x14, child)
        word(child + 0x7C, nested)
        word(obj + 0x14F8, action)
        patches.extend(((0x7DE455, bytes([mode])), (0x7DE458, bytes([flags]))))
        word(0x79094C, level)
        if ordinal >= len(_LOCK):
            control = ordinal - len(_LOCK)
            if control == 0:
                word(case.esp + 4, 0x90000000)
            elif control == 1:
                word(obj + 0x14, 0x90000000)
            else:
                new_esp = obj + 0x14FC
                frame = next(data for address, data in case.patches if address == case.esp)
                patches.append((new_esp, frame))
                regs[4], regs[6] = new_esp, 0x11 if control == 2 else 0xFFFFFFFF
                word(new_esp + 4, obj)
    else:
        query = 0x47415330 if va == 0x1EC8E0 else 0x54524350
        mode, generation, fallback, layout, pattern, offset = 1, 0, 0, 0, 3, None
        if va == 0x1EC8E0 and ordinal < len(_BOOL):
            mode, generation, fallback, layout = _BOOL[ordinal]
        elif va == 0x24D260 and ordinal < len(_COPY):
            mode, generation, pattern = _COPY[ordinal]
        elif va == 0x24D260 and ordinal < len(_COPY) + len(_ALIAS):
            pattern, offset = _ALIAS[ordinal - len(_COPY)]
        nodes, base, out = (0xD20000, 0xD20200, 0xD20400), 0xD10000, 0xD40000
        handle = (generation << 22) | 1
        record = base + generation * 0x2E8
        word(0x75D318, 0 if mode == 0 else nodes[0])
        word(0x72F5E8, base)
        word(record + 0x2C, handle ^ 1 if mode == 7 else handle)
        if va == 0x1EC8E0:
            word(0x75BF0C, fallback)
        else:
            for slot, value in enumerate(_PATTERNS[pattern]):
                word(record + 0x64 + 4 * slot, value)
            word(case.esp + 4, out if offset is None else record + 0x64 + offset)
        position = mode - 1 if mode in (1, 2, 3) else 0
        for slot, node in enumerate(nodes):
            key = query + slot - position
            if mode == 4:
                key = (0x80000000, 0xFFFFFFFE, 0xFFFFFFFF)[slot] if layout else query - 3 + slot
            elif mode == 5:
                key = 0x7FFFFFFF if layout else query + 1 + slot
            elif layout:
                key = 0x80000000 if slot < position else query if slot == position else 0x7FFFFFFF
            word(node, 0 if mode == 6 else handle)
            word(node + 4, key)
            word(node + 8, nodes[slot + 1] if slot < 2 else 0)
        first_control = len(_BOOL) if va == 0x1EC8E0 else len(_COPY) + len(_ALIAS)
        if ordinal >= first_control:
            control = ordinal - first_control
            if control == 0:
                word(0x75D318, 0x90000000)
            elif control == 1:
                word(0x72F5E8, 0x90000000)
            elif control == (2 if va == 0x1EC8E0 else 3):
                word(nodes[0] + 4, query - 1)
                word(nodes[0] + 8, nodes[0])
            elif va == 0x24D260:
                word(case.esp + 4, 0x90000000)
            else:
                word(0x75D318, 0x75BF0C)
                word(0x75BF0C, handle)
                word(0x75BF10, query)
                word(0x75BF14, 0)
                word(record + 0x2C, handle ^ 1)
    return replace(case, patches=tuple(patches), regs=tuple(regs))
