# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded copy/DF/alias input states, judged against original bytes."""

from dataclasses import replace
from random import Random

from .model import Case
from .seeding import SeedPolicy, derive_seed, make_case

ANIMATION_COPY_VA = 0x591D0
ANIMATION_COPY_CASES = 512
ANIMATION_COPY_INDEX_BASE = 8 << 40


def has_animation_copy_cases(va: int) -> bool:
    return va == ANIMATION_COPY_VA


def make_animation_copy_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not has_animation_copy_cases(va) or not 0 <= ordinal < ANIMATION_COPY_CASES:
        raise ValueError("unsupported animation copy address or ordinal")
    index = ANIMATION_COPY_INDEX_BASE + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index))
    base = (policy or SeedPolicy()).scratch_base
    source = base + 0x2000 + (ordinal // 32) % 4
    destination = base + 0x4000 + (ordinal // 128) % 4
    shape = ordinal % 32
    count = shape if shape < 16 else 16
    if shape == 15 and ordinal // 32 in (4, 5):
        count = (0xFFFFFFFF, 0x80000000)[ordinal // 32 - 4]
    if shape in (16, 17, 18, 20):
        destination = source + {16: 1, 17: 4, 18: -4, 20: 2}[shape]
    elif shape == 19:
        destination = source
    elif shape in (21, 22):
        destination = case.esp - (8 if shape == 21 else 4)
        count = 4
    elif shape in (23, 24, 25):
        destination = case.esp + {23: 12, 24: 8, 25: 4}[shape]
        count = 4
    elif shape == 26:
        source = case.esp - 6
        count = 8
    elif shape == 27:
        source = case.esp + 14
        count = 8
    elif shape == 28:
        destination = 0
    elif shape == 29:
        source = 0
    elif shape == 30:
        source = 0xFFFFFFFE
    elif shape == 31:
        source = 0xFFFFFFFC
    patches = list(case.patches)
    if 0x10000 + 64 <= source <= 0x1000000 - 128:
        patches.append((source - 64, bytes(rng.randrange(256) for _ in range(192))))
    patches.append(case.patches[0])
    patches.append(
        (
            case.esp + 4,
            b"".join(
                (word & 0xFFFFFFFF).to_bytes(4, "little") for word in (destination, source, count)
            ),
        )
    )
    return replace(case, df=(ordinal // 32) % 2, patches=tuple(patches))
