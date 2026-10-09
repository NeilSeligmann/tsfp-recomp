# SPDX-License-Identifier: GPL-3.0-or-later
"""x87 entry and exit state for the differential harness (T356).

The oracle (Unicorn) holds the x87 stack in 80-bit extended registers. The subject holds it
in C `double`. The harness therefore only seeds values that are exact doubles and reports the
exit stack as double bit patterns, so a faithful replacement compares equal and a
conversion that depends on extended precision shows up as a divergence rather than being
hidden. Pure functions, no Unicorn, so the unit tests need no emulator.
"""

from __future__ import annotations

import math
import struct
from random import Random
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .model import Case

#: Default control word: round to nearest, all exceptions masked, extended precision.
DEFAULT_CONTROL = 0x037F
#: The four rounding-control settings (bits 10-11) on top of the default word.
ROUNDING_CONTROLS: tuple[int, ...] = (0x037F, 0x077F, 0x0B7F, 0x0F7F)

#: Every precision control (24, 53 and 64 bit mantissa) crossed with every rounding control.
PRECISION_CONTROLS: tuple[int, ...] = tuple(
    precision | rounding | 0x7F
    for precision in (0x000, 0x200, 0x300)
    for rounding in (0x000, 0x400, 0x800, 0xC00)
)
#: Functions whose result depends on the precision control, so their cases vary it too.
#: The others keep 64-bit precision, which is what the game runs in.
PRECISION_SENSITIVE_VAS: frozenset[int] = frozenset({0x003CCE24})

_EXT_BIAS = 16383
_EXT_MAX_EXP = 0x7FFF
_QUIET_BIT = 1 << 62

#: Values chosen to hit the branches of a float-to-int64 conversion: signs, zero, half-way
#: ties, the 32-bit and 64-bit boundaries, overflow, infinities and NaN.
SPECIAL_DOUBLES: tuple[float, ...] = (
    0.0,
    -0.0,
    0.5,
    -0.5,
    1.5,
    -1.5,
    2.5,
    -2.5,
    0.49999999999999994,
    -0.49999999999999994,
    1.0,
    -1.0,
    123.75,
    -123.75,
    2147483647.5,
    -2147483648.5,
    4294967295.5,
    4294967296.0,
    -4294967296.5,
    9007199254740993.0,
    4611686018427387904.0,
    9223372036854775807.0,
    -9223372036854775808.0,
    9.3e18,
    -9.3e18,
    1e30,
    -1e30,
    1e-30,
    -1e-30,
    math.inf,
    -math.inf,
    math.nan,
)


def double_bits(value: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def bits_double(bits: int) -> float:
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def double_to_extended(value: float) -> tuple[int, int]:
    """`(mantissa, sign_exponent)` of the exact 80-bit extended value of a double."""
    bits = double_bits(value)
    sign = bits >> 63
    exponent = (bits >> 52) & 0x7FF
    fraction = bits & ((1 << 52) - 1)
    if exponent == 0x7FF:
        mantissa = (1 << 63) | (fraction << 11)
        if fraction:
            mantissa |= _QUIET_BIT
        return mantissa, (sign << 15) | _EXT_MAX_EXP
    if exponent == 0:
        if fraction == 0:
            return 0, sign << 15
        shift = 64 - fraction.bit_length()
        return fraction << shift, (sign << 15) | (_EXT_BIAS - 1022 - shift + 11)
    return (1 << 63) | (fraction << 11), (sign << 15) | (exponent - 1023 + _EXT_BIAS)


def extended_to_double_bits(mantissa: int, sign_exponent: int) -> int:
    """The nearest double to an extended register, as IEEE bits. NaN payloads are quieted."""
    sign = sign_exponent >> 15
    exponent = sign_exponent & _EXT_MAX_EXP
    if exponent == _EXT_MAX_EXP:
        if mantissa & ((1 << 63) - 1):
            return (sign << 63) | (0x7FF << 52) | (1 << 51) | ((mantissa >> 11) & ((1 << 51) - 1))
        return (sign << 63) | (0x7FF << 52)
    if mantissa == 0:
        return sign << 63
    try:
        magnitude = math.ldexp(float(mantissa), exponent - _EXT_BIAS - 63)
    except OverflowError:
        magnitude = math.inf
    return double_bits(-magnitude if sign else magnitude)


def extended_is_nan(mantissa: int, sign_exponent: int) -> bool:
    return (sign_exponent & _EXT_MAX_EXP) == _EXT_MAX_EXP and bool(mantissa & ((1 << 63) - 1))


def extended_fits_double(mantissa: int, sign_exponent: int) -> bool:
    """Is this 80-bit register exactly a double, so the subject's double stack can hold it?

    NaNs count as holdable (they compare by class). Anything with more than 53 significant
    bits, an exponent outside the double range, or an unnormal encoding does not.
    """
    if extended_is_nan(mantissa, sign_exponent):
        return True
    bits = extended_to_double_bits(mantissa, sign_exponent)
    return double_to_extended(bits_double(bits)) == (mantissa, sign_exponent)


def extended_equal(left: tuple[int, int], right: tuple[int, int]) -> bool:
    """Exact 80-bit equality, with every NaN equal to every other NaN."""
    if left == right:
        return True
    return extended_is_nan(*left) and extended_is_nan(*right)


def _tie_double(rng: Random) -> float:
    """A double that, plus 1.0, lands exactly half-way between two 24 or 53 bit mantissas.

    Round-to-nearest-even ties are otherwise almost unreachable by random doubles, so a
    flipped tie rule would survive. Both signs are used, so the same ties are hit from
    above and below.
    """
    odd = 2 * rng.randrange(1 << 10) + 1
    base, scale = rng.choice(((0.0, 24), (1.0, 22), (0.0, 53), (1.0, 52)))
    value = base + math.ldexp(odd, -scale)
    return -value if rng.random() < 0.5 else value


def attach_fp_stack(case: Case, *, depth: int = 1, vary_precision: bool = False) -> Case:
    """Return `case` with an x87 entry stack derived from its `(seed, index)` alone."""
    from dataclasses import replace

    from .seeding import derive_seed

    rng = Random(derive_seed(case.seed, case.index) ^ 0x87)
    values: list[float] = []
    for _ in range(depth):
        roll = rng.random()
        if roll < 0.5:
            value = SPECIAL_DOUBLES[rng.randrange(len(SPECIAL_DOUBLES))]
        elif roll < 0.75:
            value = rng.randrange(-(1 << 40), 1 << 40) + rng.choice((0.0, 0.25, 0.5, 0.75, 0.999))
        else:
            value = bits_double(rng.randrange(1 << 64))
        if vary_precision and rng.random() < 0.3:
            value = _tie_double(rng)
        values.append(value)
    controls = PRECISION_CONTROLS if vary_precision else ROUNDING_CONTROLS
    control = controls[rng.randrange(len(controls))]
    return replace(case, fp_stack=tuple(values), fp_control=control)
