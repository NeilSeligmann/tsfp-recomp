# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent exact model of FXCH st(1), FPREM (one iteration) and FSTP st(1) for 3C9DE8.

Original 3C9DE8 (T1510): `fxch st1; L: fprem; fnstsw ax; sahf; jp L; fstp st1; ret`. Derived from
the Intel SDM, not from Unicorn or the host FPU. Masked exceptions only. Fails closed
(NotRepresented) wherever the SDM leaves the result or a status bit undefined or
implementation-chosen, instead of guessing:
  * FPREM with an incomplete reduction (exponent difference >= 64): the partial step N (32..63)
    and the C0/C1/C3 bits are implementation-chosen;
  * NaN, infinite dividend, zero divisor (invalid) and stack underflow: C0/C2/C3 undefined;
  * a nonzero denormal remainder: SDM does not say whether UE is raised for an exact denormal;
  * FXCH/FSTP st(1) with an empty source register (SDM pseudo-code leaves it to #IS handling).
"""

from __future__ import annotations

from fractions import Fraction

from tools.harness.x87_arith_model import (
    DE,
    MASK_BIT,
    _check_before,
    decode,
    encode,
    tag_of,
    value_of,
)
from tools.harness.x87_state import X87State
from tools.harness.x87_store_model import C1, NotRepresented, _floor_log2

C0, C2, C3 = 0x100, 0x400, 0x4000
DEFECTS = frozenset(
    {
        "ieee-nearest-quotient",
        "q-bits-swapped",
        "sign-lost",
        "no-de",
        "fxch-tags-kept",
        "fstp1-no-pop",
        "c1-kept",
        "incomplete-ok",
    }
)


def _swap(values: tuple[int, ...], a: int, b: int) -> tuple[int, ...]:
    result = list(values)
    result[a], result[b] = result[b], result[a]
    return tuple(result)


def fxch_st1(state: X87State, defects: frozenset[str] = frozenset()) -> X87State:
    _check_before(state)
    top = state.top
    low, high = top, (top + 1) & 7
    if state.tag(low) == 3 or state.tag(high) == 3:
        raise NotRepresented("FXCH with an empty register")
    tags = state.tags
    if "fxch-tags-kept" not in defects:
        mask = (3 << low * 2) | (3 << high * 2)
        tags = (tags & ~mask) | (state.tag(high) << low * 2) | (state.tag(low) << high * 2)
    status = state.status if "c1-kept" in defects else state.status & ~C1
    return X87State(_swap(state.physical, low, high), tags, state.control, status)


def fstp_st1(state: X87State, defects: frozenset[str] = frozenset()) -> X87State:
    _check_before(state)
    top = state.top
    other = (top + 1) & 7
    if state.tag(top) == 3:
        raise NotRepresented("FSTP st(1) with an empty source")
    physical = list(state.physical)
    physical[other] = physical[top]
    tags = (state.tags & ~(3 << other * 2)) | (state.tag(top) << other * 2)
    status = state.status if "c1-kept" in defects else state.status & ~C1
    if "fstp1-no-pop" not in defects:
        tags |= 3 << top * 2
        status = (status & ~0x3800) | (((top + 1) & 7) << 11)
    return X87State(tuple(physical), tags, state.control, status)


def fprem(state: X87State, defects: frozenset[str] = frozenset()) -> X87State:
    _check_before(state)
    top = state.top
    other = (top + 1) & 7
    if state.tag(top) == 3 or state.tag(other) == 3:
        raise NotRepresented("FPREM stack underflow leaves C0/C2/C3 undefined")
    dividend, divisor = decode(state.physical[top]), decode(state.physical[other])
    if dividend.kind in ("qnan", "snan") or divisor.kind in ("qnan", "snan"):
        raise NotRepresented("FPREM with a NaN operand leaves C0/C1/C3 undefined")
    if dividend.kind == "inf" or divisor.kind == "zero":
        raise NotRepresented("FPREM invalid operation leaves C0/C1/C3 undefined")
    negative = dividend.negative
    flags = 0
    if "no-de" not in defects and "denormal" in (dividend.kind, divisor.kind):
        flags |= DE
    if flags & DE and not state.control & MASK_BIT[DE]:
        raise NotRepresented("unmasked denormal operand")
    quotient = 0
    if dividend.kind == "zero" or divisor.kind == "inf":
        remainder_raw = state.physical[top]  # remainder is the dividend, quotient 0
    else:
        a, b = abs(value_of(dividend)), abs(value_of(divisor))
        if _floor_log2(a) - _floor_log2(b) >= 64 and "incomplete-ok" not in defects:
            raise NotRepresented("incomplete reduction (exponent difference >= 64)")
        ratio = a / b
        quotient = ratio.numerator // ratio.denominator
        if "ieee-nearest-quotient" in defects:
            low = quotient
            fraction = ratio - low
            quotient = low + (
                1 if fraction > Fraction(1, 2) or (fraction == Fraction(1, 2) and low & 1) else 0
            )
        remainder = a - quotient * b
        negative_result = negative if "sign-lost" not in defects else False
        if remainder < 0:  # only reachable through the nearest-quotient defect
            negative_result, remainder = not negative_result, -remainder
        if remainder != 0 and remainder < Fraction(2) ** -16382:
            raise NotRepresented("denormal remainder: UE behaviour unspecified")
        remainder_raw = encode(remainder, negative_result)
    if dividend.kind == "denormal" and divisor.kind == "inf":
        raise NotRepresented("denormal remainder: UE behaviour unspecified")
    if "q-bits-swapped" in defects:
        c0, c3 = (quotient >> 1) & 1, (quotient >> 2) & 1
    else:
        c0, c3 = (quotient >> 2) & 1, (quotient >> 1) & 1
    c1 = quotient & 1
    for flag, bit in MASK_BIT.items():
        if flags & flag and not state.control & bit:
            raise NotRepresented(f"unmasked exception 0x{flag:X}")
    status = state.status & ~(C0 | C1 | C2 | C3)
    status |= flags | c0 * C0 | c1 * C1 | c3 * C3
    if (
        "incomplete-ok" in defects
        and _floor_log2(abs(value_of(dividend))) - _floor_log2(abs(value_of(divisor))) >= 64
    ):
        status |= C2
    physical = list(state.physical)
    physical[top] = remainder_raw
    tags = (state.tags & ~(3 << top * 2)) | (tag_of(remainder_raw) << top * 2)
    return X87State(tuple(physical), tags, state.control, status)
