# SPDX-License-Identifier: GPL-3.0-or-later
"""Additive read-only9E260→9A420→9A330 lookup and scalar boundaries."""

from __future__ import annotations

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

SCALAR_CALL_VA = 0x9E260
SCALAR_CALL_PROVIDER_VERSION = 2
SCALAR_CALL_INDICES = (*range(20), -1, 20, -0x80000000, 0x7FFFFFFF)
SCALAR_CALL_CASES = 3 * 8 * len(SCALAR_CALL_INDICES)
SCALAR_CALL_INDEX_BASE = 38 << 40


def make_scalar_call_case(
    seed: int,
    ordinal: int,
    va: int,
    size: int,
    *,
    policy: SeedPolicy | None = None,
) -> Case:
    if va != SCALAR_CALL_VA or not 0 <= ordinal < SCALAR_CALL_CASES:
        raise ValueError("unsupported scalar-call address or ordinal")
    case = make_case(seed, SCALAR_CALL_INDEX_BASE + ordinal, va, size, policy=policy)
    bank = ordinal // (8 * len(SCALAR_CALL_INDICES))
    flags, object_id = (
        (0, 1001),
        (1, 1001),
        (1, 1000),
        (1, 9001),
        (0x801, 9001),
        (0x801, -1),
        (0x400001, -2),
        (0x400001, -1),
    )[(ordinal // len(SCALAR_CALL_INDICES)) % 8]
    index = SCALAR_CALL_INDICES[ordinal % len(SCALAR_CALL_INDICES)]
    argument = (1001, 8000, 8350)[bank]
    record = 0xD00800
    base = (record - (argument if bank == 0 else 0) * 32) & 0xFFFFFFFF
    patches = list(case.patches)
    values = (
        (case.esp + 4, argument),
        ((0x715F2C, 0x715F34, 0x715F38)[bank], base),
        (0x79094C, 0),
        (record, object_id),
        (record + 4, flags),
        (record + 0x1C, index),
    )
    patches.extend(
        (address, (value & 0xFFFFFFFF).to_bytes(4, "little")) for address, value in values
    )
    return replace(case, patches=tuple(patches))
