# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared additive T1477 string/frame-alias/boundary probes, never random edits."""

from dataclasses import replace

from .model import REG_NAMES, Case
from .seeding import GUEST_HI, SCRATCH_BASE, SeedPolicy, make_case

CRC_STRING_VA = 0x150530
CRC_STRING_CASES = 166
CRC_STRING_INDEX_BASE = 28 << 40
CRC_LENGTHS = (0, 1, 2, 3, 7, 15, 31, 63, 127)
CRC_SOURCE_BASE = SCRATCH_BASE + 0x18000
CRC_ALIAS_ESI = 0x00434241


def _pattern(kind: int, length: int) -> bytes:
    if kind == 0:
        return b"A" * length
    if kind == 1:
        return b"\xff" * length
    if kind == 2:
        return bytes(0x80 if index % 2 == 0 else 1 for index in range(length))
    return bytes(1 + (7 * index % 255) for index in range(length))


def crc_string_case_count(va: int) -> int:
    return CRC_STRING_CASES if va == CRC_STRING_VA else 0


def make_crc_string_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != CRC_STRING_VA or not 0 <= ordinal < CRC_STRING_CASES:
        raise ValueError("unsupported CRC-string address or ordinal")
    case = make_case(seed, CRC_STRING_INDEX_BASE + ordinal, va, size, policy=policy)
    patches = list(case.patches)
    regs = list(case.regs)
    if ordinal < 144:
        length = CRC_LENGTHS[ordinal // 16]
        pointer = CRC_SOURCE_BASE + ordinal % 4
        patches.append((pointer, _pattern(ordinal // 4 % 4, length) + b"\0"))
    elif ordinal < 160:
        slot = (ordinal - 144) % 4
        pointer = case.esp - 4 * (slot + 1)
        # The root's actual save supplies ESI bytes AB C\0; do not prewrite its slot.
        length = case.esp - 4 - pointer
        if length:
            patches.append((pointer, _pattern((ordinal - 144) // 4, length)))
        regs[REG_NAMES.index("esi")] = CRC_ALIAS_ESI
    else:
        pointer = (0, 1, 0xFFFFFFFF, 0xFFFFFFF0, GUEST_HI - 1, GUEST_HI - 32)[ordinal - 160]
        if ordinal >= 164:
            patches.append((pointer, b"A" * (GUEST_HI - pointer)))
    patches.append((case.esp + 4, pointer.to_bytes(4, "little")))
    return replace(case, regs=tuple(regs), patches=tuple(patches))
