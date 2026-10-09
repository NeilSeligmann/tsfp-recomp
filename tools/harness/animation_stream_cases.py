# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite original/native resolver state fixtures, never production stream state."""

from dataclasses import replace
from random import Random

from .model import REG_NAMES, Case
from .seeding import SeedPolicy, derive_seed, make_case

ANIMATION_STREAM_VA = 0x60390
ANIMATION_STREAM_CASES = 512
ANIMATION_STREAM_INDEX_BASE = 7 << 40


def has_animation_stream_cases(va: int) -> bool:
    return va == ANIMATION_STREAM_VA


def make_animation_stream_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not has_animation_stream_cases(va) or not 0 <= ordinal < ANIMATION_STREAM_CASES:
        raise ValueError("unsupported animation stream address or ordinal")
    index = ANIMATION_STREAM_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    base = (policy or SeedPolicy()).scratch_base
    header, ranges, offsets, buffer = (base + n for n in (0x2000, 0x3000, 0x4000, 0x5000))
    regs = list(case.regs)
    patches: list[tuple[int, bytes]] = []

    def word(address: int, value: int) -> None:
        patches.append((address & 0xFFFFFFFF, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    shape = ordinal % 32
    key = rng.choice((1, 9, 10, 19, 20, 39, 50, -1, -10, -0x80000000, 0x7FFFFFFF))
    chunk = rng.choice((0, 1, 32, 64, 0xFFFFFFFF, 0x80000000))
    resident_offset = rng.getrandbits(32)
    logical = [0, 1, 2, 3]
    starts = [100, 200, 300, 400, 500, 600, 700, 800]
    lengths = [10] * 8
    pending = [0] * 4
    if shape in (0, 30):
        key = 0
    elif 1 <= shape <= 4:
        slot = shape - 1
        key = starts[slot] + rng.choice((0, 1, 9))
    elif shape in (6, 7, 19):
        key = 101
        pending[0] = 0x80000000 if shape == 19 else 1
        if shape == 7:
            starts[1] = starts[0]
    elif shape in (8, 24):
        key, starts[0], lengths[0] = -1, -10, 20
    elif shape == 9:
        key, starts[0], lengths[0] = 0x7FFFFFFE, 0x7FFFFFFD, 10
    elif shape == 10:
        key, starts[0], lengths[0] = -1, -0x80000000, 0xFFFFFFFF
    elif shape == 11:
        key, logical[0] = 301, 2
    elif shape == 12:
        key, header = 99, case.esp - 8
    elif shape == 13:
        key, ranges = 17, case.esp - 12
        regs[REG_NAMES.index("edi")] = 16
        regs[REG_NAMES.index("ebx")] = 3
    elif shape == 14:
        key, starts[0], offsets = 1, 1, case.esp
    elif shape == 16:
        key, header = 0, 0
    elif shape == 17:
        key, ranges = 1, 0
    elif shape == 18:
        key, starts[0], offsets = 1, 1, 0
    elif shape == 20:
        key, logical[0] = -1, 0xFFFFFFFF
        word(ranges - 8, -2)
        word(ranges - 4, 5)
    elif shape == 22:
        key, starts[0], header = 1, 1, case.esp - 32
    elif shape == 23:
        key, starts[0], buffer = 1, 1, 0xFFFFFFF0
    elif shape == 25:
        key, starts[0] = 1, 1
        header += 1
        ranges += 1
        offsets += 1
    elif shape in (26, 27):
        key, starts[2], chunk = 1, 1, 0 if shape == 26 else 0xFFFFFFFF
    elif shape == 28:
        key, starts, lengths = -19, [-40, -30, -20, -10, 0, 10, 20, 30], [10] * 8
    elif shape == 29:
        key, ranges = 1, 0xFFFFFFFC
    elif shape == 31:
        key, starts, lengths = 1, [1] * 8, [0] * 8
    if shape == 30:
        ranges = 0
    word(case.esp + 4, key)
    word(0x6D7818, header)
    word(0x6D7834, ranges)
    word(0x6D7838, offsets)
    word(0x7DE530, buffer)
    if shape not in (12, 16, 22):
        word(header + 4, resident_offset)
        word(header + 0x14, chunk)
    elif shape == 22:
        word(header + 4, resident_offset)
    for slot in range(4):
        word(0x7DE540 + 4 * slot, logical[slot])
        word(0x7DE520 + 4 * slot, pending[slot])
    if shape not in (13, 17, 29, 30):
        for block in range(8):
            word(ranges + block * 8, starts[block])
            word(ranges + block * 8 + 4, lengths[block])
    # Arbitrary signed keys can wrap into globals/code; seed the selected read
    # only for deliberately mapped keys. Fault cases remain genuine no-verdicts.
    if shape != 18 and offsets != case.esp:
        address = (offsets + 4 * key) & 0xFFFFFFFF
        if 0x10000 <= address < 0x1000000 - 4:
            word(address, rng.getrandbits(32))
    return replace(case, regs=tuple(regs), patches=(*case.patches, *patches))
