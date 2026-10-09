# SPDX-License-Identifier: GPL-3.0-or-later
"""T1480/T1476 seventh-batch amended domains (namespace 49), written after the default-domain
outcomes of 003C3C50 (coverage 0.596), 003C3D20 (coverage 0.778 on seed 20261006) and
000DB480 (coverage 0.727) were seen. Additive to the seed stream: registers and unpatched
argument words keep the seed stream values."""

import hashlib
from dataclasses import replace

from .model import Case
from .seeding import SCRATCH_BASE, SeedPolicy, make_case

SEVENTH_INDEX_BASE = 49 << 40

# --- 003C3C50 / 003C3D20 (object, data, length): keyed object, MD5 trailer buffers -----------
MD5_OBJECT = SCRATCH_BASE + 0xD000
MD5_DATA = SCRATCH_BASE + 0xD400
MD5_LENGTHS = (8, 9, 16, 31, 64)
#: 0 = trailer is the first 8 digest bytes, 1 = first byte corrupted, 2 = last byte corrupted
#: (the writer's trailers are all 0xA5 so its output is observable).
MD5_TRAILERS = (0, 1, 2)
#: (keyed, second_word, length): unkeyed, second word zero (verify only), short, negative.
MD5_GUARDS = ((0, 2, 0x20), (1, 0, 0x20), (1, 2, 7), (1, 2, 0xFFFFFFF0))
MD5_VERIFY_CASES = len(MD5_LENGTHS) * len(MD5_TRAILERS) + len(MD5_GUARDS)
MD5_WRITE_CASES = len(MD5_LENGTHS) + len(MD5_GUARDS)

# --- 000DB480 (object, first_key, second_key): valid keyed list -----------------------------
LIST_OBJECT = SCRATCH_BASE + 0xD800
LIST_NODES = SCRATCH_BASE + 0xD900
LIST_NODE_STRIDE = 0x40
#: (first, second) keys of node 0..2 when the list has three nodes.
LIST_KEYS = ((0x11, 0x21), (0x12, 0x22), (0x13, 0x23))
LIST_QUERIES = tuple(
    (length, query) for length in (1, 2, 3) for query in (*range(length), "none", "partial")
)
LIST_CASES = len(LIST_QUERIES) + 1

SEVENTH_ROOTS = {0x3C3C50: MD5_VERIFY_CASES, 0x3C3D20: MD5_WRITE_CASES, 0xDB480: LIST_CASES}


def seventh_case_count(va: int) -> int:
    return SEVENTH_ROOTS.get(va, 0)


def _words(*values: int) -> bytes:
    return b"".join((value & 0xFFFFFFFF).to_bytes(4, "little") for value in values)


def md5_parameters(va: int, ordinal: int) -> tuple[int, int, int, int]:
    """`(keyed, second_word, length, trailer_variant)`; the writer gets poisoned trailers (3)."""
    main = len(MD5_LENGTHS) * (len(MD5_TRAILERS) if va == 0x3C3C50 else 1)
    if ordinal >= main:
        keyed, second, length = MD5_GUARDS[ordinal - main]
        return keyed, second, length, 0
    if va == 0x3C3C50:
        length_index, variant = divmod(ordinal, len(MD5_TRAILERS))
        return 1, 2, MD5_LENGTHS[length_index], MD5_TRAILERS[variant]
    return 1, 2, MD5_LENGTHS[ordinal], 3


def md5_buffer(length: int, variant: int) -> bytes:
    """The data buffer: a deterministic prefix, then (for the verifier) the trailer."""
    prefix = bytes((length * 13 + index * 7 + 1) & 0xFF for index in range(max(length - 8, 0)))
    trailer = bytearray(hashlib.md5(prefix).digest()[:8])
    if variant == 1:
        trailer[0] ^= 0x01
    elif variant == 2:
        trailer[7] ^= 0x80
    elif variant == 3:
        trailer = bytearray(b"\xa5" * 8)
    return prefix + bytes(trailer) if length >= 8 else bytes(8)


def _md5_patches(va: int, ordinal: int) -> list[tuple[int, bytes]]:
    keyed, second, length, variant = md5_parameters(va, ordinal)
    data = md5_buffer(length, variant) if length < 0x80000000 else bytes(8)
    return [(MD5_OBJECT, _words(keyed, second)), (MD5_DATA, data)]


def list_parameters(ordinal: int) -> tuple[int, int, int]:
    """`(length, first_key, second_key)`; length 0 is the empty list."""
    if ordinal == len(LIST_QUERIES):
        return 0, 1, 1
    length, query = LIST_QUERIES[ordinal]
    if query == "none":
        return length, 0x77, 0x88
    if query == "partial":
        return length, LIST_KEYS[0][0], 0x88
    return length, *LIST_KEYS[query]


def _list_patches(ordinal: int) -> list[tuple[int, bytes]]:
    length = list_parameters(ordinal)[0]
    node = [LIST_NODES + LIST_NODE_STRIDE * index for index in range(length)]
    head = node[0] if length else 0
    tail = node[-1] if length else 0
    patches = [(LIST_OBJECT + 0x230, _words(head, tail, length))]
    for index, address in enumerate(node):
        previous = node[index - 1] if index else 0
        following = node[index + 1] if index + 1 < length else 0
        patches.append((address, _words(*LIST_KEYS[index], 0, 0, 0, previous, following)))
    return patches


def make_seventh_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    if not 0 <= ordinal < seventh_case_count(va):
        raise ValueError("unsupported T1480 seventh-batch case")
    case = make_case(seed, SEVENTH_INDEX_BASE + ordinal, va, size, policy=policy)
    if va == 0xDB480:
        _, first, second = list_parameters(ordinal)
        extra = [
            (case.esp + 4, _words(LIST_OBJECT, first, second)),
            *_list_patches(ordinal),
        ]
    else:
        _, _, length, _ = md5_parameters(va, ordinal)
        extra = [
            (case.esp + 4, _words(MD5_OBJECT, MD5_DATA, length)),
            *_md5_patches(va, ordinal),
        ]
    return replace(case, patches=(*case.patches, *extra))
