# SPDX-License-Identifier: GPL-3.0-or-later
"""T1480 fifth-batch amended domains (namespace 45), written after the default-domain
outcomes of 003CAB7B (coverage 0.737) and 0044B4AE (3 to 6 verdicts) were seen."""

from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

FIFTH_INDEX_BASE = 45 << 40

#: 003CAB7B `_itoa(value, buffer, radix)`: value x radix grid, buffer a fixed arena address.
ITOA_VALUES = (
    0,
    1,
    9,
    10,
    15,
    16,
    35,
    36,
    0x7FFFFFFF,
    0x80000000,
    0x80000001,
    0xFFFFFFFF,
    0xFFFFFFFE,
    0xFFFFFF00,
    123456789,
    0xDEADBEEF,
)
ITOA_RADICES = (2, 3, 8, 10, 16, 36)
ITOA_BUFFER = SCRATCH_BASE + 0x8000
ITOA_CASES = len(ITOA_VALUES) * len(ITOA_RADICES)

#: 0044B4AE `(reader, index, selector, output)`: the table rows 0..17 have 1 to 3 bit
#: fields. Selectors below 13 use row index+9 (index 0..8 keeps it inside rows 9..17),
#: selectors from 13 up use row index (0..17). The reader is a valid object: current
#: word, bits remaining, pointer to the next source word.
FIELD_SELECTORS = (0, 12, 13, 0xFFFFFFFF)
FIELD_BITS_LEFT = (0, 1, 2, 3, 4, 16, 32)
FIELD_WORDS = (0xFFFFFFFF, 0x5A5A1234)
FIELD_READER = SCRATCH_BASE + 0x9000
FIELD_SOURCE = SCRATCH_BASE + 0x9100
FIELD_OUTPUT = SCRATCH_BASE + 0xA000
FIELD_PAIRS = tuple(
    (selector, index) for selector in FIELD_SELECTORS for index in range(9 if selector < 13 else 18)
)
FIELD_CASES = len(FIELD_PAIRS) * len(FIELD_BITS_LEFT) * len(FIELD_WORDS)

FIFTH_ROOTS = {0x3CAB7B: ITOA_CASES, 0x44B4AE: FIELD_CASES}


def fifth_case_count(va: int) -> int:
    return FIFTH_ROOTS.get(va, 0)


def itoa_arguments(ordinal: int) -> tuple[int, int, int]:
    value = ITOA_VALUES[ordinal // len(ITOA_RADICES)]
    return value, ITOA_BUFFER, ITOA_RADICES[ordinal % len(ITOA_RADICES)]


def field_parameters(ordinal: int) -> tuple[int, int, int, int]:
    """`(selector, index, bits_left, word)` for one 0044B4AE ordinal."""
    pair, rest = divmod(ordinal, len(FIELD_BITS_LEFT) * len(FIELD_WORDS))
    selector, index = FIELD_PAIRS[pair]
    return (
        selector,
        index,
        FIELD_BITS_LEFT[rest // len(FIELD_WORDS)],
        FIELD_WORDS[rest % len(FIELD_WORDS)],
    )


def make_fifth_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < fifth_case_count(va):
        raise ValueError("unsupported T1480 fifth-batch case")
    case = make_case(seed, FIFTH_INDEX_BASE + ordinal, va, size, policy=policy)
    if va == 0x3CAB7B:
        value, buffer, radix = itoa_arguments(ordinal)
        arguments = [value, buffer, radix]
        return replace(
            case,
            patches=(
                *case.patches,
                (case.esp + 4, b"".join(a.to_bytes(4, "little") for a in arguments)),
            ),
        )
    selector, index, bits_left, word = field_parameters(ordinal)
    arguments = [FIELD_READER, index, selector, FIELD_OUTPUT]
    reader = [word, bits_left, FIELD_SOURCE]
    source = [0x0F0F0F0F, 0xA5A5A5A5, 0x13579BDF, 0xFFFFFFFF]
    return replace(
        case,
        patches=(
            *case.patches,
            (case.esp + 4, b"".join(a.to_bytes(4, "little") for a in arguments)),
            (FIELD_READER, b"".join(a.to_bytes(4, "little") for a in reader)),
            (FIELD_SOURCE, b"".join(a.to_bytes(4, "little") for a in source)),
        ),
    )
