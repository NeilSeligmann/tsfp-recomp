# SPDX-License-Identifier: GPL-3.0-or-later
"""String-shaped cases for the CRT leaf replacements in src/game/crt_leaves.c (T418).

Random pointer-shaped inputs hand a string function a count near 2**31 and a buffer of
random bytes, so the word loop, the zero-pad and the match paths are barely reached (0x003C8960
sat at 56% coverage). These cases build real strings in the scratch arena instead: a source
with a NUL at a chosen place, small counts, every alignment, high-bit bytes for the
carry-trick word test, near-matching needles over a two-letter alphabet. Each case is
derived from `(seed, index)` only, like every other case.

Argument layout per function, `(register inputs, stack arguments)`, is encoded in `_BUILDERS`.
"""

from __future__ import annotations

from dataclasses import replace
from random import Random

from .model import Case
from .seeding import (
    SCRATCH_BASE,
    SCRATCH_FILL_BYTES,
    SeedPolicy,
    derive_seed,
    make_case,
)

#: String case ordinals per function.
STRING_CASES = 512
#: Index space above the edge (1 << 40) and feedback streams.
STRING_INDEX_BASE = 3 << 40
STRING_ORDINAL_BITS = 11

REGION_A = 0x000
REGION_B = 0x100
REGION_C = 0x200

#: Bytes that make the zero-word test and the signed compares interesting.
_SPECIAL_BYTES = (0x01, 0x7F, 0x80, 0x81, 0xFE, 0xFF, 0x41, 0x61)
#: Bytes next to the carry-trick boundaries, used for every byte of a "carry-adversarial" string.
_CARRY_BYTES = (0x01, 0x02, 0x7F, 0x80, 0x81, 0xFE, 0xFF)


def _byte(rng: Random, small_alphabet: bool) -> int:
    if small_alphabet:
        return rng.choice((0x61, 0x62, 0x63))
    if rng.random() < 0.3:
        return rng.choice(_SPECIAL_BYTES)
    return rng.randrange(1, 256)


def _text(rng: Random, length: int, small_alphabet: bool = False) -> bytearray:
    if not small_alphabet and rng.random() < 0.4:
        return bytearray(rng.choice(_CARRY_BYTES) for _ in range(length))
    return bytearray(_byte(rng, small_alphabet) for _ in range(length))


def _put(buf: bytearray, offset: int, data: bytes) -> None:
    buf[offset : offset + len(data)] = data


def _build_copy(rng: Random, buf: bytearray) -> tuple[dict[int, int], list[int]]:
    source = REGION_A + rng.randrange(4)
    destination = REGION_C + rng.randrange(4)
    length = rng.choice((0, 1, 2, 3, 4, 5, 7, 8, 9, 12, 16, 21))
    count = rng.choice((0, 1, 2, 3, 4, 5, 6, 8, 9, 11, 13, 16, 17, 24, 33))
    if rng.random() < 0.4:
        # The NUL falls inside the head that aligns the source, with an unaligned destination
        # and more count left: the zero-fill then starts mid-word.
        source = REGION_A + rng.randrange(1, 4)
        length = rng.randrange(0, 4 - source % 4)
        destination = REGION_C + rng.randrange(1, 4)
        count = rng.randrange(length + 2, 41)
    _put(buf, source, bytes(_text(rng, length)) + b"\0")
    if rng.random() < 0.3:
        # More zero and boundary bytes behind the NUL, so the NUL word holds several of them
        # and a carry test that misses such a word copies the tail instead of zero-filling.
        trail = bytes(rng.choice((0, 0, 0, 1, 0x80, 0xFF)) for _ in range(12))
        _put(buf, source + length + 1, trail)
    if rng.random() < 0.15:  # no NUL in range: a pure copy
        _put(buf, source, bytes(_text(rng, 0x60)))
    return {}, [SCRATCH_BASE + destination, SCRATCH_BASE + source, count]


def _build_compare(rng: Random, buf: bytearray) -> tuple[dict[int, int], list[int]]:
    first = REGION_A + rng.randrange(4)
    second = REGION_B + rng.randrange(4)
    text = _text(rng, rng.choice((0, 1, 2, 5, 9, 16)))
    _put(buf, first, bytes(text) + b"\0")
    other = bytearray(text) + b"\0"
    if other and rng.random() < 0.6:
        other[rng.randrange(len(other))] = rng.choice((0, 0x7F, 0x80, 0xFF, 0x01))
    _put(buf, second, bytes(other) + bytes(_text(rng, 4)))
    count = rng.choice((0, 1, 2, 3, 4, 8, 12, 20))
    return {}, [SCRATCH_BASE + first, SCRATCH_BASE + second, count]


def _build_append(rng: Random, buf: bytearray) -> tuple[dict[int, int], list[int]]:
    destination = REGION_C + rng.randrange(4)
    source = REGION_A + rng.randrange(4)
    _put(buf, destination, bytes(_text(rng, rng.choice((0, 1, 2, 3, 4, 5, 7, 8, 13)))) + b"\0")
    _put(buf, source, bytes(_text(rng, rng.choice((0, 1, 2, 4, 5, 9, 14)))) + b"\0")
    count = rng.choice((0, 1, 2, 3, 4, 5, 6, 8, 9, 15, 20))
    return {}, [SCRATCH_BASE + destination, SCRATCH_BASE + source, count]


def _build_unit16_length(rng: Random, buf: bytearray) -> tuple[dict[int, int], list[int]]:
    text = REGION_A + rng.randrange(4)
    data = bytearray()
    for _ in range(rng.choice((0, 1, 2, 3, 6, 11))):
        unit = rng.choice((0x0100, 0x0001, 0x8000, 0x00FF, 0xFFFF, rng.randrange(1, 1 << 16)))
        data += unit.to_bytes(2, "little")
    _put(buf, text, bytes(data) + b"\0\0")
    return {}, [SCRATCH_BASE + text]


def _build_class_lookup(rng: Random, buf: bytearray) -> tuple[dict[int, int], list[int]]:
    value = rng.choice(
        (0xFFFFFFFF, 0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x101, 0xFFFFFFFE, rng.randrange(1 << 32))
    )
    return {}, [value, rng.choice((0xFFFF, 0x0001, 0x0100, 0x8000, rng.randrange(1 << 16)))]


def _build_find_byte(rng: Random, buf: bytearray) -> tuple[dict[int, int], list[int]]:
    text = REGION_A + rng.randrange(4)
    data = _text(rng, rng.choice((0, 1, 2, 3, 4, 5, 8, 11, 17)), rng.random() < 0.3)
    dense = rng.random() < 0.3
    if dense:
        # Aligned words of 0x01 and neighbours searched for 0x81/0x80/0xFF: the xor-and-carry
        # test fires on a word with no real match, so the byte-by-byte tail has to reject it.
        text = REGION_A
        data = bytearray(
            rng.choice((0x01, 0x01, 0x02, 0x7F, 0x80, 0xFE)) for _ in range(rng.choice((4, 8, 12)))
        )
    _put(buf, text, bytes(data) + b"\0")
    roll = rng.random()
    if dense:
        wanted = rng.choice((0x81, 0x80, 0xFF, 0x7F, 0x03, 0x82))
    elif data and roll < 0.6:
        wanted = data[rng.randrange(len(data))]
    elif roll < 0.7:
        wanted = 0
    else:
        wanted = rng.choice((0x7F, 0x80, 0xFF, 0x01, rng.randrange(256)))
    wanted |= rng.choice((0, rng.randrange(1 << 24) << 8))
    return {0: wanted}, [SCRATCH_BASE + text]


def _build_find_substring(rng: Random, buf: bytearray) -> tuple[dict[int, int], list[int]]:
    haystack = REGION_A + rng.randrange(4)
    needle = REGION_B + rng.randrange(4)
    small = rng.random() < 0.6
    text = _text(rng, rng.choice((0, 1, 3, 6, 10, 18, 24)), small)
    _put(buf, haystack, bytes(text) + b"\0")
    if text and rng.random() < 0.7:
        start = rng.randrange(len(text))
        piece = bytearray(text[start : start + rng.choice((1, 2, 3, 4, 5, 6))])
    else:
        piece = _text(rng, rng.choice((0, 1, 2, 3, 4, 5)), small)
    if piece and rng.random() < 0.25:
        piece[rng.randrange(len(piece))] = rng.choice((0x61, 0x62, 0x63, 0xFF))
    _put(buf, needle, bytes(piece) + b"\0")
    return {}, [SCRATCH_BASE + haystack, SCRATCH_BASE + needle]


#: `va -> builder(rng, scratch) -> ({register slot: value}, stack arguments)`.
_BUILDERS = {
    0x003C8960: _build_copy,
    0x003C94F0: _build_compare,
    0x003C9C4E: _build_class_lookup,
    0x003CA6C0: _build_unit16_length,
    0x003C9F80: _build_append,
    0x003C9546: _build_find_byte,
    0x003C8690: _build_find_substring,
}


def has_string_cases(va: int) -> bool:
    return va in _BUILDERS


def make_string_case(
    seed: int, ordinal: int, va: int, size: int, *, policy: SeedPolicy | None = None
) -> Case:
    """One string-shaped case: the random case for this index with real strings placed."""
    if ordinal >= 1 << STRING_ORDINAL_BITS:
        raise ValueError("string ordinal out of range")
    index = STRING_INDEX_BASE + (va << STRING_ORDINAL_BITS) + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    rng = Random(derive_seed(seed, index) ^ 0x5354524E47)
    buf = bytearray(rng.randrange(1, 256) for _ in range(SCRATCH_FILL_BYTES))
    registers, arguments = _BUILDERS[va](rng, buf)
    regs = list(case.regs)
    for slot, value in registers.items():
        regs[slot] = value
    esp_value, frame = case.patches[0]
    frame_bytes = bytearray(frame)
    for position, value in enumerate(arguments):
        frame_bytes[4 + 4 * position : 8 + 4 * position] = value.to_bytes(4, "little")
    patches = ((esp_value, bytes(frame_bytes)), (case.patches[1][0], bytes(buf)), *case.patches[2:])
    return replace(case, regs=tuple(regs), patches=patches)
