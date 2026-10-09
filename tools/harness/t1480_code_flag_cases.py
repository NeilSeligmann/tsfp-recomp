# SPDX-License-Identifier: GPL-3.0-or-later
"""T1480 predeclared 003E6486 argument domain, additive to the original seed stream."""

from dataclasses import replace

from .model import Case
from .seeding import SeedPolicy, make_case

CODE_FLAG_INDEX_BASE = 40 << 40
CODE_FLAG_ROOTS = {0x3E6486: 6}
#: 0..40 inclusive, then values that agree with 12/14/15 in the low byte or low bits only,
#: wrap-around neighbours and 32-bit extremes (the original compares the full dword).
CODE_FLAG_CODES = (
    *range(41),
    0x10C,
    0x10D,
    0x10E,
    0x10F,
    0x100,
    0x1000C,
    0x0100000C,
    0x0100000E,
    0x8000000C,
    0x8000000F,
    0xFFFF000C,
    0xFFFFFFF4,
    0xFFFFFFFC,
    0xFFFFFFFD,
    0xFFFFFFFE,
    0xFFFFFFFF,
    0x7FFFFFFF,
    0x80000000,
)


#: Amendment A2: the true codes again, 80 ordinals each (registers and frame come from
#: the seed stream per ordinal), so the answer-2 outcome is not rare in the proof.
CODE_FLAG_TRUE_CODES = (12, 14, 15)
CODE_FLAG_TRUE_REPEATS = 80
CODE_FLAG_BASE_CASES = len(CODE_FLAG_CODES)
CODE_FLAG_CASES = CODE_FLAG_BASE_CASES + len(CODE_FLAG_TRUE_CODES) * CODE_FLAG_TRUE_REPEATS


def code_flag_argument(ordinal: int) -> int:
    if ordinal < CODE_FLAG_BASE_CASES:
        return CODE_FLAG_CODES[ordinal]
    return CODE_FLAG_TRUE_CODES[(ordinal - CODE_FLAG_BASE_CASES) % len(CODE_FLAG_TRUE_CODES)]


def code_flag_case_count(va: int) -> int:
    return CODE_FLAG_CASES if va in CODE_FLAG_ROOTS else 0


def make_code_flag_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not code_flag_case_count(va) or not 0 <= ordinal < CODE_FLAG_CASES:
        raise ValueError("unsupported T1480 code-flag case")
    case = make_case(seed, CODE_FLAG_INDEX_BASE + ordinal, va, size, policy=policy)
    argument = (case.esp + 4, code_flag_argument(ordinal).to_bytes(4, "little"))
    return replace(case, patches=(*case.patches, argument))
