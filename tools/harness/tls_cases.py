# SPDX-License-Identifier: GPL-3.0-or-later
"""fs/TLS-shaped cases for replacements that read the fs segment (T469).

Random registers never move the fs segment or the TLS pointer chain, so every random case
reads the same seeded dwords. These cases patch that chain instead, from `(seed, index)`
alone: the slot index, the TLS pointer array entry, the `fs:[0x24]` byte the dead compare
reads, and the faulting shapes (`fs:[4]` nulled, an array entry nulled, a wild index) that
both sides must report as ORACLE-FAULTED rather than judge.
"""

from __future__ import annotations

from dataclasses import replace
from random import Random

from .model import Case
from .seeding import (
    KPCR_BASE,
    KPCR_BYTE_24,
    KPCR_DWORD_28,
    TLS_BLOCK_BASE,
    TLS_BLOCK_STRIDE,
    TLS_INDEX_ADDRESS,
    TLS_SLOT_COUNT,
    TLS_TABLE_BASE,
    SeedPolicy,
    derive_seed,
    make_case,
    tls_value,
)

#: Cases per function.
TLS_CASES = 256
#: Index space above the string cases (3 << 40).
TLS_INDEX_BASE = 5 << 40
TLS_ORDINAL_BITS = 11

#: Functions with fs/TLS cases.
TLS_FUNCTIONS = frozenset({0x0037E9A7})


def has_tls_cases(va: int) -> bool:
    return va in TLS_FUNCTIONS


def _dword(value: int) -> bytes:
    return (value & 0xFFFFFFFF).to_bytes(4, "little")


def make_tls_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    """One case with the TLS chain and the fs bytes the function reads varied."""
    if ordinal >= 1 << TLS_ORDINAL_BITS:
        raise ValueError("tls ordinal out of range")
    index = TLS_INDEX_BASE + (va << TLS_ORDINAL_BITS) + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index) ^ 0x544C53)
    slot = rng.randrange(TLS_SLOT_COUNT)
    patches: list[tuple[int, bytes]] = [(TLS_INDEX_ADDRESS, _dword(slot))]
    patches.append((KPCR_BASE + KPCR_BYTE_24, bytes([rng.choice((0, 1, 2, 3, 0x7F, 0xFF))])))
    patches.append((KPCR_BASE + KPCR_DWORD_28, _dword(rng.randrange(1 << 32))))
    block = TLS_BLOCK_BASE + TLS_BLOCK_STRIDE * slot
    patches.append(
        (block + 4, _dword(rng.choice((tls_value(slot), 0, 0xFFFFFFFF, rng.randrange(1 << 32)))))
    )
    roll = rng.random()
    if roll < 0.10:
        patches.append((KPCR_BASE + 4, _dword(0)))  # unseeded fs:[4]: both sides fault
    elif roll < 0.20:
        patches.append((TLS_TABLE_BASE + 4 * slot, _dword(0)))  # null block: both fault
    elif roll < 0.30:
        patches.append((TLS_INDEX_ADDRESS, _dword(rng.choice((0x4000000, 0xFFFFFFFF, 0x7FFFFFFF)))))
    elif roll < 0.45:
        # A fully different array and block, so a replacement that hard-codes the seeded
        # addresses instead of following the chain disagrees.
        other_array = TLS_TABLE_BASE + 0x400
        other_block = TLS_BLOCK_BASE + 0x400
        patches.append((KPCR_BASE + 4, _dword(other_array)))
        patches.append((other_array + 4 * slot, _dword(other_block)))
        patches.append((other_block + 4, _dword(rng.randrange(1 << 32))))
    return replace(case, patches=(*case.patches, *patches))
