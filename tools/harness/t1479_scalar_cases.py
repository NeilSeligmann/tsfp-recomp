# SPDX-License-Identifier: GPL-3.0-or-later
"""Additive T1479 global flag/sum and sixteen-slot occupancy fixtures."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

SCALAR_INDEX_BASE = 33 << 40
SUM_ROOT = 0x0035E630
FREE_ROOT = 0x00356320
SUM_ROWS = (
    (0, 0x11223344, 0x55667788, 0x99AABBCC),
    (1, 0, 0, 0),
    (1, 0xFFFFFFFF, 1, 1),
    (1, 0x80000000, 0x7FFFFFFF, 2),
    (2, 0x13579BDF, 0x2468ACE0, 0xFFFFFFFF),
    (0xFFFFFFFF, 7, 11, 13),
)


def t1479_scalar_case_count(va: int) -> int:
    return len(SUM_ROWS) if va == SUM_ROOT else 4 if va == FREE_ROOT else 0


def make_t1479_scalar_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < t1479_scalar_case_count(va):
        raise ValueError("unsupported T1479 scalar root or ordinal")
    case = make_case(seed, SCALAR_INDEX_BASE + ordinal, va, size, policy=policy)
    patches = list(case.patches)
    if va == SUM_ROOT:
        targets = dict(
            zip((0x774024, 0x76BBBC, 0x76BBC0, 0x76BBC4), SUM_ROWS[ordinal], strict=True)
        )
        neighbors = {address + delta for address in targets for delta in (-4, 4)} - set(targets)
        for address in sorted(neighbors):
            patches.append((address, (0x13579BDF ^ address).to_bytes(4, "little")))
        patches.extend((address, value.to_bytes(4, "little")) for address, value in targets.items())
    else:
        for index in range(16):
            address = 0x7741B4 + 0x30C * index
            active = (False, True, index % 2 == 0, index == 15)[ordinal]
            for delta in (-1, 1, 2, 3):
                neighbor = address + delta
                patches.append((neighbor, bytes([(0x5A ^ neighbor) & 0xFF])))
            patches.append((address, bytes([0xA4 | int(active)])))
    patches.append((case.esp - 4, (0xC0DE0001 ^ va).to_bytes(4, "little")))
    return replace(case, patches=tuple(patches))
