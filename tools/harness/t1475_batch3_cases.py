# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace-62 amended domain for the T1475 continuation, batch 3 (0x42640).

Written after the default-domain outcome of 0x42640 (coverage 0.867 < 0.9, 968 AGREE, zero
disagreements) was seen. Declaration: docs/t1475-t1477-continuation-batches.md, "Batch 3".
Additive to the seed stream. Every scenario points the current-pool word [0x6F1E10] at valid
scratch memory so the nested 0x69EA0 store lands in mapped guest memory.
"""

from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

BATCH3_INDEX_BASE = 62 << 40
Patch = tuple[int, bytes]

FLAGS = (0, 0x800000, 0x7FFFFF, 0xFF800000)
STATES = (0, 1, 2, 3, 4, 5, 6, 0xFFFFFFFF)
DELTAS = ((0x500, 0x120), (0x100, 0x200), (0, 0), (0x80000000, 1))
POOL = SCRATCH_BASE + 0x5000
CASES = len(FLAGS) * len(STATES) * len(DELTAS)


def _w(address: int, *values: int) -> Patch:
    return (address, b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values))


def batch3_case_count(va: int) -> int:
    return CASES if va == 0x42640 else 0


def make_batch3_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < batch3_case_count(va):
        raise ValueError("unsupported T1475 batch 3 case")
    case = make_case(seed, BATCH3_INDEX_BASE + ordinal, va, size, policy=policy)
    flag = FLAGS[ordinal % len(FLAGS)]
    state = STATES[ordinal // len(FLAGS) % len(STATES)]
    minuend, subtrahend = DELTAS[ordinal // (len(FLAGS) * len(STATES))]
    patches = [
        _w(0x7DE458, flag),
        _w(0x6B98A4, state),
        _w(0x6B94C0, minuend),
        _w(0x6B989C, subtrahend),
        _w(0x6F1E10, POOL),
    ]
    return replace(case, patches=(*case.patches, *patches))
