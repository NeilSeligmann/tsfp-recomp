# SPDX-License-Identifier: GPL-3.0-or-later
"""Namespace-63 amended domain for the T1477 continuation, batch 4 (0x15C4E0).

Written after the default-domain outcome of 0x15C4E0 (904 AGREE, zero disagreements, but
coverage 0.390 < 0.9 and null-agree 0.84) was seen. Declaration: docs/t1475-t1477-continuation-
batches.md, "Batch 4". Additive to the seed stream. Every scenario builds a table of three
0xEA0-byte records in mapped scratch, each pointing (through the +0x7C object chain the nested
0x15C3D0 follows) at valid scratch objects, so the loop body and the nested call run.
"""

from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

BATCH4_INDEX_BASE = 63 << 40
Patch = tuple[int, bytes]

MODES = (0, 0x0A)
COUNTS = (0, 1, 2, 3)
LIMITS = (0, 1, 0xFFFFFFFF, 5)
# Record shapes: 0 skip bit 2 set, 1 low mask pair (0x10/0x14), 2 high mask pair (0x18/0x1C),
# 3 empty masks with +8 below the limit, 4 at the limit, 5 huge.
SHAPES = (
    (0, 1, 2),
    (3, 4, 5),
    (2, 3, 4),
    (5, 5, 1),
    (1, 2, 0),
    (4, 3, 5),
)
CASES = len(MODES) * len(COUNTS) * len(LIMITS) * len(SHAPES)
TABLE = SCRATCH_BASE + 0x10000
OBJECTS = SCRATCH_BASE + 0x4000
STRIDE = 0xEA0


def _w(address: int, *values: int) -> Patch:
    return (address, b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values))


def batch4_case_count(va: int) -> int:
    return CASES if va == 0x15C4E0 else 0


def make_batch4_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < batch4_case_count(va):
        raise ValueError("unsupported T1477 batch 4 case")
    case = make_case(seed, BATCH4_INDEX_BASE + ordinal, va, size, policy=policy)
    shape = SHAPES[ordinal % len(SHAPES)]
    rest = ordinal // len(SHAPES)
    limit = LIMITS[rest % len(LIMITS)]
    rest //= len(LIMITS)
    count = COUNTS[rest % len(COUNTS)]
    mode = MODES[rest // len(COUNTS)]
    signed_limit = limit - (1 << 32) if limit & 0x80000000 else limit
    patches = [
        _w(0x7A2958, count),
        _w(0x790950, limit),
        _w(0x7356D8, TABLE),
        (0x7DE455, bytes([mode])),
        _w(0x74C37C, ordinal % 3),
    ]
    for index, kind in enumerate(shape):
        base = TABLE + index * STRIDE
        outer = OBJECTS + index * 0x400
        inner = outer + 0x100
        target = outer + 0x200
        masks = {1: (0x10, 0x14), 2: (0x18, 0x1C)}.get(kind)
        field_8 = {3: signed_limit - 1, 4: signed_limit, 5: 0x7FFFFFFF}.get(kind, 0)
        patches += [
            _w(base, outer),
            _w(base + 8, field_8),
            _w(base + 0x2C, 2 if kind == 0 else 0),
            _w(base + 0x10, 0, 0, 0, 0),
            _w(outer + 0x7C, inner),
            _w(inner + 0x2C, 4 if index % 2 == 0 else 0),
            _w(inner, target),
            _w(inner + 8, field_8),
            _w(inner + 0x10, 0),
            _w(inner + 0x28, 0),
        ]
        if masks is not None:
            patches.append(_w(base + masks[0], 1 + index))
    return replace(case, patches=(*case.patches, *patches))
