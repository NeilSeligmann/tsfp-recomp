# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared finite record/list domains for T1475's third call-bearing batch."""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 29 << 40
_SELECTED = tuple(product(range(4), range(7), (0, 7, 0xFFFFFFFF), (0, 1, 0xFFFFFFFF)))
_TAGS = (-128, -1, 0, 127)
_KEYS = tuple(product(((1, 2), (2, 1)), (0, 1, 2, 0xFFFFFFFF), range(5), _TAGS, (False, True)))
_BOUNDARIES = tuple(
    product(
        ((0, 0), (1, 0), (0, 1), (0xFFFFFFFF, 1), (0x7FFFFFFF, 1)), (0, 0xFFFFFFFF), (False, True)
    )
)


def call_record_case_count(va: int) -> int:
    return {0x71A60: len(_SELECTED) + 5, 0x657A0: len(_KEYS) + len(_BOUNDARIES) + 2}.get(va, 0)


def make_call_record_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < call_record_case_count(va):
        raise ValueError("unsupported call-record case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches = list(case.patches)
    regs = list(case.regs)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    if va == 0x71A60:
        obj, selected = 0xD20000, 0xD21000
        nodes = (0xD22000, 0xD22200, 0xD22400)
        selector, chain, key, fallback = (
            _SELECTED[ordinal] if ordinal < len(_SELECTED) else (3, 1, 7, 0xFFFFFFFF)
        )
        word(0x7BB1E0, 0 if selector == 0 else selected)
        word(0x7BB1E8, obj if selector != 2 else obj + 4)
        word(selected + 4, int(selector == 1))
        word(selected + 0x5C, key)
        word(
            selected + 0x60,
            1 if chain == 4 else 3 if chain == 5 else 0xFFFFFFFF if chain == 6 else 0,
        )
        word(obj + 0x198, fallback)
        word(obj + 0x178, 0 if chain == 0 else nodes[0])
        for slot, node in enumerate(nodes):
            matches = (chain == 1 and slot == 0) or (chain == 2 and slot == 1) or chain in (4, 5, 6)
            word(node + 0x14, key if matches else key ^ 0xFFFFFFFF)
            word(node + 4, nodes[slot + 1] if slot < 2 else 0)
        word(case.esp + 4, obj)
        if ordinal >= len(_SELECTED):
            control = ordinal - len(_SELECTED)
            if control == 0:
                word(case.esp + 4, 0x90000000)
            elif control == 1:
                word(0x7BB1E0, 0x90000000)
            elif control == 2:
                word(obj + 0x178, 0x90000000)
            elif control == 3:
                word(nodes[0] + 0x14, key ^ 0xFFFFFFFF)
                word(nodes[0] + 4, nodes[0])
            else:
                new_esp = 0x7BB1E4
                # Relocate the original entry frame intact, then set its argument.
                frame = next(data for address, data in case.patches if address == case.esp)
                patches.append((new_esp, frame))
                regs[4], regs[6] = new_esp, 0
                word(new_esp + 4, obj)
    else:
        key = 0x13579BDF
        if ordinal < len(_KEYS):
            (count0, count1), excluded, pattern, tag, different = _KEYS[ordinal]
        elif ordinal < len(_KEYS) + len(_BOUNDARIES):
            (count0, count1), excluded, different = _BOUNDARIES[ordinal - len(_KEYS)]
            pattern, tag = 4, 0
        else:
            count0, count1, excluded, pattern, tag, different = (
                (0x10000, 0x7FFFFFFF)[ordinal - len(_KEYS) - len(_BOUNDARIES)],
                0,
                0xFFFFFFFF,
                0,
                0,
                False,
            )
        word(0x7A2958, count0)
        word(0x790950, count1)
        word(case.esp + 4, excluded)
        word(case.esp + 8, key)
        word(case.esp + 12, tag)
        for slot in range(3):
            record = 0x6F0B08 + slot * 0x68
            matches = pattern == 4 or pattern == slot + 1
            word(record + 0x58, key if matches else key ^ 0xFFFFFFFF)
            actual_tag = ((tag + 1 + 128) % 256 - 128) if different else tag
            patches.append((record + 1, bytes([actual_tag & 0xFF])))
    return replace(case, patches=tuple(patches), regs=tuple(regs))
