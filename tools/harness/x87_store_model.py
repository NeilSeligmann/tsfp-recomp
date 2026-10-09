# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent exact model of FSTP m32real (Intel SDM), used only to arbitrate backends.

Derived from the manual, not from Unicorn or the host FPU. Fails closed
(NotRepresented) on unmasked exceptions and states outside the modelled domain.
"""

from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction

from tools.harness.x87_state import X87State

IE, SF, OE, UE, PE, C1 = 0x1, 0x40, 0x8, 0x10, 0x20, 0x200
MASK_BIT = {IE: 0x1, OE: 0x8, UE: 0x10, PE: 0x20}
INDEFINITE = 0xFFC00000
DEFECTS = frozenset(
    {"c1-stuck", "no-pop", "no-quiet", "rc-ignored", "pe-missing", "c1-before-kept"}
)


class NotRepresented(ValueError):
    """The input or outcome is outside the modelled, masked-exception domain."""


@dataclass(frozen=True)
class StoreResult:
    state: X87State
    single: int


def _floor_log2(value: Fraction) -> int:
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if Fraction(2) ** exponent > value:
        exponent -= 1
    return exponent


def _round(magnitude: Fraction, negative: bool, rc: int) -> tuple[Fraction, bool, bool, bool]:
    """Return (rounded magnitude, tiny, inexact, rounded_up) with the single's 24-bit precision."""
    exponent = _floor_log2(magnitude)
    quantum = Fraction(2) ** (exponent - 23) if exponent >= -126 else Fraction(2) ** -149
    scaled = magnitude / quantum
    low = scaled.numerator // scaled.denominator
    frac = scaled - low
    count = low
    if frac:
        if rc == 0:
            half = Fraction(1, 2)
            count = low + (1 if frac > half or (frac == half and low & 1) else 0)
        else:
            toward_up = (rc == 2 and not negative) or (rc == 1 and negative)
            count = low + (1 if toward_up else 0)
    result = count * quantum
    return result, result < Fraction(2) ** -126, result != magnitude, result > magnitude


def _single_bits(value: Fraction, negative: bool) -> int:
    sign = 0x80000000 if negative else 0
    if value == 0:
        return sign
    if value < Fraction(2) ** -126:
        return sign | int(value / Fraction(2) ** -149)
    exponent = _floor_log2(value)
    mantissa = int(value / Fraction(2) ** (exponent - 23)) - (1 << 23)
    return sign | ((exponent + 127) << 23) | mantissa


def fstp_m32(state: X87State, defects: frozenset[str] = frozenset()) -> StoreResult:
    """Model FSTP m32real on a raw x87 state; the memory store itself is assumed to complete."""
    # C1 is defined afresh by FSTP (round-up indicator, else cleared); only ES/B are out of domain.
    if state.status & (0x80 | 0x8000):
        raise NotRepresented("before-state ES/B outside modelled domain")
    top = state.top
    flags = 0
    c1 = 0
    if state.tag(top) == 3:
        flags |= IE | SF
        single = INDEFINITE
    else:
        raw = state.physical[top]
        negative = bool(raw >> 79)
        exponent = (raw >> 64) & 0x7FFF
        mantissa = raw & ((1 << 64) - 1)
        integer = mantissa >> 63
        fraction = mantissa & ((1 << 63) - 1)
        sign = 0x80000000 if negative else 0
        rc = state.control >> 10 & 3
        if "rc-ignored" in defects:
            rc = 0
        if exponent == 0x7FFF:
            if integer == 0:
                flags, single = IE, INDEFINITE  # pseudo-NaN / pseudo-infinity
            elif fraction == 0:
                single = sign | 0x7F800000
            else:
                quiet = fraction >> 62
                if not quiet:
                    flags |= IE
                payload = (fraction >> 40) & 0x3FFFFF
                bit = 0 if "no-quiet" in defects and not quiet else 0x400000
                single = sign | 0x7F800000 | bit | payload
        elif exponent != 0 and integer == 0:
            flags, single = IE, INDEFINITE  # unnormal
        elif mantissa == 0:
            single = sign
        else:
            unit = Fraction(2) ** ((exponent or 1) - 16383 - 63)
            result, tiny, inexact, up = _round(mantissa * unit, negative, rc)
            if result >= Fraction(2) ** 128:
                flags |= OE | PE
                to_infinity = rc == 0 or (rc == 2 and not negative) or (rc == 1 and negative)
                single = sign | (0x7F800000 if to_infinity else 0x7F7FFFFF)
                up = to_infinity
                inexact = True
            else:
                single = _single_bits(result, negative)
                if inexact:
                    flags |= PE | (UE if tiny else 0)
            c1 = int(up and inexact)
            if "c1-stuck" in defects:
                c1 = 1
            if "pe-missing" in defects:
                flags &= ~PE
    for flag, bit in MASK_BIT.items():
        if flags & flag and not state.control & bit:
            raise NotRepresented(f"unmasked exception 0x{flag:X}")
    if "c1-before-kept" in defects:
        c1 |= int(bool(state.status & C1))
    status = (state.status & ~C1) | flags | (c1 * C1)
    tags = state.tags
    if "no-pop" not in defects:
        tags |= 3 << (top * 2)
        status = (status & ~0x3800) | (((top + 1) & 7) << 11)
    return StoreResult(X87State(state.physical, tags, state.control, status), single)
