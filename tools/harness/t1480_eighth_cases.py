# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared additive deferred-root structures, exclusive namespace70.

Unpatched seed registers and memory retain their original values. This provider
never patches retail static tables or instructions. See the tracked eighth-batch
predeclaration for the exact corpus and amended-fixture classification.
"""

from dataclasses import replace
from itertools import product

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case
from .t1480_service_field_cases import make_service_field_case, service_field_case_count

EIGHTH_INDEX_BASE = 70 << 40
CONTEXT = SCRATCH_BASE + 0xD000
DATA = SCRATCH_BASE + 0xD400
TABLE = SCRATCH_BASE + 0xD800
POSITION = SCRATCH_BASE + 0xD300
OUTPUT = SCRATCH_BASE + 0xD320
OBJECT = SCRATCH_BASE + 0xE000
MD5_PARAMETERS = tuple(product((0, 1, 63, 64, 127), (0, 1, 2, 63, 64, 65, 128))) + tuple(
    (count, -length - 1) for count, length in product((0, 1, 63, 64, 127), (0, 1, 63, 64, 65))
)
# (levels, second_width, leaf, limit_delta); -1 width denotes a terminal first node.
DECODE_PARAMETERS = tuple(product((0, 1, 2, 8), (-1, 0, 1, 2, 7), (0, 1, 4095), (-1, 0, 1)))
# (first_byte, sample_count); the object table maps every index to first_byte % 17.
WRAPPER_PARAMETERS = tuple(product(range(256), (0, 1, 17, 65535)))
EIGHTH_ROOTS = {
    0x3BC2A0: len(MD5_PARAMETERS),
    0x38F565: len(DECODE_PARAMETERS),
    0x38E491: len(WRAPPER_PARAMETERS),
    0x38D5CB: len(WRAPPER_PARAMETERS),
}


def eighth_case_count(va: int) -> int:
    return service_field_case_count(va) if va == 0x417ADA else EIGHTH_ROOTS.get(va, 0)


def _words(*values: int) -> bytes:
    return b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values)


def _short(value: int) -> bytes:
    return (value & 65535).to_bytes(2, "little")


def make_eighth_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if va == 0x417ADA:
        return make_service_field_case(seed, ordinal, va, size, policy=policy)
    if not 0 <= ordinal < eighth_case_count(va):
        raise ValueError("unsupported T1480 eighth-batch case")
    case = make_case(seed, EIGHTH_INDEX_BASE + ordinal, va, size, policy=policy)
    if va == 0x3BC2A0:
        count, length = MD5_PARAMETERS[ordinal]
        actual = -length - 1 if length < 0 else length
        context = _words(count, 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476) + bytes(64)
        data = bytes((index * 13 + 1) % 255 + 1 for index in range(actual)) + b"\0"
        extra = [
            (case.esp + 4, _words(CONTEXT, DATA, 0xFFFFFFFF if length < 0 else length)),
            (CONTEXT, context),
            (DATA, data),
        ]
    elif va == 0x38F565:
        levels, width, leaf, delta = DECODE_PARAMETERS[ordinal]
        first = 0x8000 | leaf if width == -1 else (width << 12) | 4
        table = _short(first) * 4 + _short(0x8000 | leaf) * 128
        extra = [
            (case.esp + 4, _words(TABLE, levels, OUTPUT, DATA, POSITION, 2 + delta)),
            (TABLE, table),
            (DATA, bytes(32)),
            (POSITION, _words(0)),
            (OUTPUT, b"\xa5\xa5"),
        ]
    else:
        first, sample = WRAPPER_PARAMETERS[ordinal]
        obj = bytearray(0x2100)
        obj[10:12] = _short(sample)
        obj[0x8C:0x90] = _words(DATA)
        obj[0x94:0x98] = _words(4096)
        obj[0x98:0x9C] = _words(0)
        obj[0xDA:0x20DA] = _short(first % 17) * 4096
        args = (OBJECT,) if va == 0x38D5CB else (OBJECT, DATA, POSITION, 4096)
        extra = [
            (case.esp + 4, _words(*args)),
            (OBJECT, bytes(obj)),
            (DATA, bytes([first]) + bytes(511)),
            (POSITION, _words(0)),
        ]
    return replace(case, patches=(*case.patches, *extra))
