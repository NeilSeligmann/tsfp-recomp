# SPDX-License-Identifier: GPL-3.0-or-later
"""T1476 additive constant-callback prestates; preserve the original random stream.

The root bodies push literal callback, slot and power-of-two mask values for
0x0009E2D0. Destination rows and order below come from those original calls;
see docs/t1476-callback-proof-plan.md for return PCs and frame justification.
"""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

CALLBACK_INDEX_BASE = 27 << 40
CALLBACK_CASES = 4
# (destination dword, callback VA), in original guest call order.
CALLBACK_ROWS: dict[int, tuple[tuple[int, int], ...]] = {
    0x000AE8B0: ((0x0072F388, 0x000AC940), (0x0072F38C, 0x000A92F0)),
    0x000AEA20: (
        (0x0072F550, 0x000AE940),
        (0x0072F55C, 0x000AE9B0),
        (0x0072F554, 0x000AE980),
    ),
    0x000B2FA0: ((0x0072F404, 0x000B2D70), (0x0072F4C4, 0x000B2DB0)),
    0x000C3510: ((0x0072F3EC, 0x000C2E70), (0x0072F3F8, 0x000C30A0)),
    0x000D9210: ((0x0072F334, 0x000D8E30), (0x0072F338, 0x000D9030)),
}


def callback_case_count(va: int) -> int:
    return CALLBACK_CASES if va in CALLBACK_ROWS else 0


def make_callback_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va not in CALLBACK_ROWS or not 0 <= ordinal < CALLBACK_CASES:
        raise ValueError("unsupported callback root or ordinal")
    case = make_case(seed, CALLBACK_INDEX_BASE + ordinal, va, size, policy=policy)
    rows = CALLBACK_ROWS[va]
    targets = {address for address, _ in rows}
    neighbors = {address + delta for address in targets for delta in (-4, 4)} - targets
    patches = list(case.patches)

    def word(address: int, value: int) -> None:
        patches.append((address, (value & 0xFFFFFFFF).to_bytes(4, "little")))

    for address in sorted(neighbors):
        word(address, 0x13579BDF ^ address)
    for address, callback in rows:
        value = (0, 0xFFFFFFFF, 0x80000000, callback)[ordinal]
        word(address, value)
    for index in range(1, 3 * len(rows) + 2):
        word(case.esp - 4 * index, (0xC0DE0000 | index) ^ va)
    # No saved root return PC, register, or pre-existing base-case patch changes.
    return replace(case, patches=tuple(patches))
