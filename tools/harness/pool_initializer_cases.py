# SPDX-License-Identifier: GPL-3.0-or-later
"""T1476 additive AE910 free-list prestates, predeclared in the proof plan."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

POOL_INITIALIZER_VA = 0x000AE910
POOL_INITIALIZER_INDEX_BASE = 30 << 40
POOL_INITIALIZER_CASES = 4
POOL = 0x007329F0
STORAGE = 0x007327B0
STRIDE = 0x24
COUNT = 16


def pool_initializer_case_count(va: int) -> int:
    return POOL_INITIALIZER_CASES if va == POOL_INITIALIZER_VA else 0


def make_pool_initializer_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va != POOL_INITIALIZER_VA or not 0 <= ordinal < POOL_INITIALIZER_CASES:
        raise ValueError("unsupported pool initializer root or ordinal")
    case = make_case(seed, POOL_INITIALIZER_INDEX_BASE + ordinal, va, size, policy=policy)
    expected = {POOL: STORAGE, POOL + 4: STORAGE + STRIDE * (COUNT - 1)}
    expected.update({POOL + 8: COUNT, POOL + 12: COUNT})
    for index in range(COUNT):
        expected[STORAGE + STRIDE * index] = STORAGE + STRIDE * (index - 1) if index else 0
    interior = {
        STORAGE + STRIDE * index + offset
        for index in range(COUNT)
        for offset in range(4, STRIDE, 4)
    }
    guards = (interior | {STORAGE - 4, STORAGE + COUNT * STRIDE, POOL - 4, POOL + 16}) - set(
        expected
    )
    patches = list(case.patches)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    for address in sorted(guards):
        word(address, 0x13579BDF ^ address)
    for address, final in sorted(expected.items()):
        word(address, (0, 0xFFFFFFFF, 0x80000000, final)[ordinal])
    for index in range(1, 9):
        word(case.esp - 4 * index, (0xC0DE0000 | index) ^ va)
    return replace(case, patches=tuple(patches))
