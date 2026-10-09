# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared namespace37 domains for T1478's third scalar call batch.

Contracts and ordinal layout: docs/evidence/t1478/third-domain.md (written before outcomes).
"""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SeedPolicy, make_case

_INDEX_BASE = 37 << 40
_TABLE = 0x5655B0
_RECORD_SIZE = 48
_GETTER = 0x7B979C
_BYTE_PAIRS = ((0, 0), (0, 0xFF), (0xFF, 0), (0xFF, 0xFF))
_VALID = tuple(product(range(11), _BYTE_PAIRS))
_INVALID = (11, 12, 255, 100, 0x80000000, 0xFFFFFFFF)
_SLOT_ALIAS = tuple(product((0, 10), (7, 8)))
_WORDS = (0, 0xFFFFFFFF, 0x80000000, 0x01010101)
_SHIFTS = tuple(product(range(32), _WORDS, (0, 1)))
_WIDE_SHIFTS = tuple(product((32, 63, 0x80000000, 0xFFFFFFFF), _WORDS))
_FLAGS = (0, 0x40, 0x800000, 0x800040, 0xFFFFFFFF)
_STATES = (0xFFFFFFFF, 0, 5, 6, 7, 8, 9, 0x80000000, 0x7FFFFFFF)
#: Amendment A1 (written AFTER the 22DE90 default domain failed the null-agree gate at 0.9929):
#: the nine answer-1 combinations, 24 ordinals each; registers and frame vary per ordinal.
_TRUE_FLAGS = (0, 0x40, 0x800000)
_TRUE_STATES = (6, 7, 8)
_RECORD = 0xD20000
_COUNTS = {
    0x190690: len(_VALID) + len(_INVALID) + len(_SLOT_ALIAS) + 1,
    0x1CD560: len(_SHIFTS) + len(_WIDE_SHIFTS) + 4,
    0x22DE90: len(_FLAGS) * len(_STATES) + 2 + len(_TRUE_FLAGS) * len(_TRUE_STATES) * 24,
}


def t1478_scalar_case_count(va: int) -> int:
    return _COUNTS.get(va, 0)


def make_t1478_scalar_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < t1478_scalar_case_count(va):
        raise ValueError("unsupported T1478 scalar case")
    case = make_case(seed, _INDEX_BASE + ordinal, va, size, policy=policy)
    patches, regs = list(case.patches), list(case.regs)
    frame = next(data for address, data in case.patches if address == case.esp)
    frame = frame[: 8 if va == 0x1CD560 else 4]

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    def move_entry(new_esp: int) -> None:
        patches.append((new_esp, frame))
        regs[4] = new_esp

    if va == 0x190690:
        offset = None
        if ordinal < len(_VALID):
            index, (first, fourth) = _VALID[ordinal]
        elif ordinal < len(_VALID) + len(_INVALID):
            index, first, fourth = _INVALID[ordinal - len(_VALID)], 0xFF, 0xFF
        elif ordinal < len(_VALID) + len(_INVALID) + len(_SLOT_ALIAS):
            index, offset = _SLOT_ALIAS[ordinal - len(_VALID) - len(_INVALID)]
            first, fourth = 0xFF, 0xFF
        else:
            index, first, fourth = 0, 0xFF, 0xFF
        record = _TABLE + _RECORD_SIZE * index
        if index < 11:
            patches.extend(((record, bytes([first])), (record + 3, bytes([fourth]))))
        word(_GETTER, index)
        if ordinal >= len(_VALID) + len(_INVALID):
            move_entry(record + offset if offset is not None else 0x7B97A0)
    elif va == 0x1CD560:
        override = 0
        if ordinal < len(_SHIFTS):
            shift, pattern, override = _SHIFTS[ordinal]
        elif ordinal < len(_SHIFTS) + len(_WIDE_SHIFTS):
            shift, pattern = _WIDE_SHIFTS[ordinal - len(_SHIFTS)]
        else:
            shift, pattern = 1, 0xFFFFFFFF
        control = ordinal - len(_SHIFTS) - len(_WIDE_SHIFTS)
        pointer = {0: 0x90000000, 1: 0xFFFFF410}.get(control, _RECORD)
        word(0x6F1E0C, pointer)
        word(0x75AEF4, override)
        word(_RECORD + 0xBF0, pattern)
        word(case.esp + 4, shift)
        if control == 2:
            move_entry(0x6F1E10)
            word(0x6F1E10 + 4, shift)
        elif control == 3:
            # The argument slot IS the record word (shift 0xFFFFFFFF tests bit 31 of itself).
            move_entry(_RECORD + 0xBF0 - 4)
            word(_RECORD + 0xBF0, 0xFFFFFFFF)
    else:
        count = len(_FLAGS) * len(_STATES)
        if ordinal == count:
            move_entry(0x7DE45C)
        elif ordinal == count + 1:
            move_entry(0x7DE474)
        if ordinal < count:
            flags, state = _FLAGS[ordinal // len(_STATES)], _STATES[ordinal % len(_STATES)]
        elif ordinal < count + 2:
            flags, state = 0, 7
        else:
            combo = (ordinal - count - 2) % (len(_TRUE_FLAGS) * len(_TRUE_STATES))
            flags, state = _TRUE_FLAGS[combo // len(_TRUE_STATES)], _TRUE_STATES[combo % 3]
        word(0x7DE458, flags)
        word(0x7DE470, state)
    return replace(case, patches=tuple(patches), regs=tuple(regs))
