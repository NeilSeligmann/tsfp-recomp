# SPDX-License-Identifier: GPL-3.0-or-later
"""T1476 0008E920 predeclared counter-chain domain (namespace 81).

Written after the sixth-batch rejection of 0008E920 (default domain: 0 verdicts, namespace 47
fixture: 60 verdicts, below the unloosened 100 gate), BEFORE any run of this domain. Additive
to the seed stream: registers and unpatched argument words keep the seed stream values.
Chain lengths 1..4, every negative-counter mask, four amounts, plus a negative pre-check
chain per amount: (2+4+8+16 + 1) * 4 = 124 cases per seed."""

from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

CHAIN_INDEX_BASE = 81 << 40
CHAIN_OBJECT = SCRATCH_BASE + 0xB000
CHAIN_BASE = SCRATCH_BASE + 0xB400
CHAIN_AMOUNTS = (0, 3, 0x7FFFFFFF, 0xFFFFFFFF)
CHAIN_PATTERNS = tuple((length, mask) for length in (1, 2, 3, 4) for mask in range(1 << length))
CHAIN_CASES = (len(CHAIN_PATTERNS) + 1) * len(CHAIN_AMOUNTS)
CHAIN_ROOTS = {0x8E920: CHAIN_CASES}


def chain_case_count(va: int) -> int:
    return CHAIN_ROOTS.get(va, 0)


def _words(*values: int) -> bytes:
    return b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values)


def chain_parameters(ordinal: int) -> tuple[int, int, int]:
    """`(length, negative_mask, amount)`; length 0 is the negative pre-check case."""
    pattern, amount = divmod(ordinal, len(CHAIN_AMOUNTS))
    if pattern == len(CHAIN_PATTERNS):
        return 0, 0, CHAIN_AMOUNTS[amount]
    length, mask = CHAIN_PATTERNS[pattern]
    return length, mask, CHAIN_AMOUNTS[amount]


def chain_patches(ordinal: int) -> list[tuple[int, bytes]]:
    length, mask, _ = chain_parameters(ordinal)
    block = bytearray(0xC8)
    block[0x4B] = 0xFF
    block[0x4C] = 0xFF
    block[0x14:0x18] = CHAIN_BASE.to_bytes(4, "little")
    chain = bytearray(0x30 * 6)
    if length == 0:
        chain[0x10:0x14] = _words(0xFFFFFFFF)
    for entry in range(length):
        counter = 0x80000005 if mask >> entry & 1 else 5
        chain[0x30 * entry : 0x30 * entry + 4] = _words(counter)
        chain[0x30 * entry + 0x10 : 0x30 * entry + 0x14] = _words(1)
    if length:
        chain[0x30 * length + 0x10 : 0x30 * length + 0x14] = _words(0xFFFFFFFF)
    return [(CHAIN_OBJECT, bytes(block)), (CHAIN_BASE, bytes(chain))]


def make_chain_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < chain_case_count(va):
        raise ValueError("unsupported T1476 chain case")
    case = make_case(seed, CHAIN_INDEX_BASE + ordinal, va, size, policy=policy)
    _, _, amount = chain_parameters(ordinal)
    extra = [
        (case.esp + 4, _words(CHAIN_OBJECT)),
        (case.esp + 12, _words(amount)),
        *chain_patches(ordinal),
    ]
    return replace(case, patches=(*case.patches, *extra))
