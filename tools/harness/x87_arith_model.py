# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent exact model of FLD m32real and FMUL m32real (Intel SDM), masked exceptions only.

Derived from the manual, not from Unicorn or the host FPU. Fails closed (NotRepresented)
on unmasked exceptions, pseudo-encodings, ambiguous NaN ties and unmodelled before-state.
Extends tools/harness/x87_store_model.py one instruction at a time.
"""

from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction

from tools.harness.x87_state import X87State
from tools.harness.x87_store_model import C1, IE, OE, PE, SF, UE, NotRepresented, _floor_log2

DE = 0x2
MASK_BIT = {IE: 0x1, DE: 0x2, OE: 0x8, UE: 0x10, PE: 0x20}
INDEFINITE_RAW = (0xFFFF << 64) | (3 << 62)
FLD_DEFECTS = frozenset(
    {"no-quiet", "no-de", "no-push", "tag-valid", "overflow-c1-clear", "c1-before-kept"}
)
FMUL_DEFECTS = frozenset(
    {
        "pc-ignored",
        "rc-ignored",
        "no-de",
        "c1-stuck",
        "pe-missing",
        "tiny-before",
        "inf-zero-ok",
        "c1-before-kept",
    }
)


@dataclass(frozen=True)
class Operand:
    kind: str  # zero denormal normal inf qnan snan
    negative: bool
    exponent: int
    mantissa: int


def decode(raw: int) -> Operand:
    negative = bool(raw >> 79)
    exponent = (raw >> 64) & 0x7FFF
    mantissa = raw & ((1 << 64) - 1)
    integer = mantissa >> 63
    fraction = mantissa & ((1 << 63) - 1)
    if exponent == 0:
        if mantissa == 0:
            kind = "zero"
        elif integer:
            raise NotRepresented("pseudo-denormal")
        else:
            kind = "denormal"
    elif exponent == 0x7FFF:
        if not integer:
            raise NotRepresented("pseudo-NaN/infinity")
        kind = "inf" if fraction == 0 else ("qnan" if fraction >> 62 else "snan")
    else:
        if not integer:
            raise NotRepresented("unnormal")
        kind = "normal"
    return Operand(kind, negative, exponent, mantissa)


def value_of(operand: Operand) -> Fraction:
    return operand.mantissa * Fraction(2) ** ((operand.exponent or 1) - 16383 - 63)


def tag_of(raw: int) -> int:
    kind = decode(raw).kind
    return {"normal": 0, "zero": 1}.get(kind, 2)


def encode(value: Fraction, negative: bool) -> int:
    """Encode a nonnegative value exactly representable in the 64-bit extended format."""
    sign = 1 << 79 if negative else 0
    if value == 0:
        return sign
    exponent = _floor_log2(value)
    if exponent < -16382:
        mantissa = value / Fraction(2) ** -16445
        assert mantissa.denominator == 1
        return sign | int(mantissa)
    mantissa = value / Fraction(2) ** (exponent - 63)
    assert mantissa.denominator == 1
    return sign | ((exponent + 16383) << 64) | int(mantissa)


def single_to_extended(bits: int) -> tuple[int, bool, bool]:
    """Return (raw80, signalling, denormal) for a binary32 operand, exactly."""
    negative = bool(bits >> 31)
    sign = 1 << 79 if negative else 0
    exponent = (bits >> 23) & 0xFF
    fraction = bits & 0x7FFFFF
    if exponent == 0xFF:
        if fraction == 0:
            return sign | (0x7FFF << 64) | (1 << 63), False, False
        raw = sign | (0x7FFF << 64) | (1 << 63) | (fraction << 40)
        return raw, not fraction >> 22, False
    if exponent == 0:
        if fraction == 0:
            return sign, False, False
        return encode(fraction * Fraction(2) ** -149, negative), False, True
    return sign | ((exponent - 127 + 16383) << 64) | (1 << 63) | (fraction << 40), False, False


def _round(
    magnitude: Fraction, negative: bool, rc: int, bits: int, emin: int, clamp: bool
) -> Fraction:
    """Round to `bits` significant bits. clamp=True denormalizes below 2^emin."""
    exponent = _floor_log2(magnitude)
    if clamp:
        exponent = max(exponent, emin)
    quantum = Fraction(2) ** (exponent - (bits - 1))
    scaled = magnitude / quantum
    low = scaled.numerator // scaled.denominator
    frac = scaled - low
    count = low
    if frac:
        if rc == 0:
            half = Fraction(1, 2)
            count = low + (1 if frac > half or (frac == half and low & 1) else 0)
        else:
            count = low + (1 if (rc == 2 and not negative) or (rc == 1 and negative) else 0)
    return count * quantum


def _check_before(state: X87State) -> None:
    # C1 is defined afresh by FLD/FMUL/FSTP (SDM: stack-fault/round-up indicator, else cleared),
    # so a set before-state C1 is in the domain. ES and B arise only from unmasked exceptions.
    if state.status & (0x80 | 0x8000):
        raise NotRepresented("before-state ES/B outside modelled domain")
    for index in range(8):
        if state.tag(index) != 3 and state.tag(index) != tag_of(state.physical[index]):
            raise NotRepresented("before-state tag inconsistent with value")


def _finish(
    state: X87State,
    flags: int,
    c1: int,
    tags: int,
    physical: tuple[int, ...],
    top: int,
    defects: frozenset[str] = frozenset(),
) -> X87State:
    for flag, bit in MASK_BIT.items():
        if flags & flag and not state.control & bit:
            raise NotRepresented(f"unmasked exception 0x{flag:X}")
    if "c1-before-kept" in defects:
        c1 |= int(bool(state.status & C1))
    status = (state.status & ~(C1 | 0x3800)) | flags | (c1 * C1) | (top << 11)
    return X87State(physical, tags, state.control, status)


def _with_slot(state: X87State, index: int, raw: int, tag: int) -> tuple[tuple[int, ...], int]:
    physical = list(state.physical)
    physical[index] = raw
    tags = (state.tags & ~(3 << (index * 2))) | (tag << (index * 2))
    return tuple(physical), tags


def fld_m32(state: X87State, bits: int, defects: frozenset[str] = frozenset()) -> X87State:
    _check_before(state)
    top = state.top if "no-push" in defects else (state.top - 1) & 7
    flags = c1 = 0
    if state.tag(top) != 3 and "no-push" not in defects:
        flags |= IE | SF  # stack overflow, masked: real indefinite loaded
        c1 = 0 if "overflow-c1-clear" in defects else 1
        raw = INDEFINITE_RAW
    else:
        raw, signalling, denormal = single_to_extended(bits)
        if signalling:
            flags |= IE
            if "no-quiet" not in defects:
                raw |= 1 << 62
        if denormal and "no-de" not in defects:
            flags |= DE
    tag = 0 if "tag-valid" in defects else tag_of(raw)
    physical, tags = _with_slot(state, top, raw, tag)
    return _finish(state, flags, c1, tags, physical, top, defects)


def _nan_result(a: Operand, a_raw: int, b: Operand, b_raw: int) -> tuple[int, bool]:
    nans = [(op, raw) for op, raw in ((a, a_raw), (b, b_raw)) if op.kind in ("qnan", "snan")]
    invalid = any(op.kind == "snan" for op, _ in nans)
    quiets = [item for item in nans if item[0].kind == "qnan"]
    pool = quiets if len(quiets) == 1 else nans  # x87: a lone QNaN beats an SNaN
    if len(pool) == 1 or pool[0][1] == pool[1][1]:
        chosen = pool[0]
    elif pool[0][0].mantissa == pool[1][0].mantissa:
        raise NotRepresented("NaN tie with differing sign")
    else:
        chosen = max(pool, key=lambda item: item[0].mantissa)
    return chosen[1] | (1 << 62), invalid


def fmul_m32(state: X87State, bits: int, defects: frozenset[str] = frozenset()) -> X87State:
    _check_before(state)
    top = state.top
    control = state.control
    flags = c1 = 0
    if state.tag(top) == 3:
        physical, tags = _with_slot(state, top, INDEFINITE_RAW, 2)
        return _finish(state, IE | SF, 0, tags, physical, top, defects)
    a_raw = state.physical[top]
    b_raw, _, b_denormal = single_to_extended(bits)
    a, b = decode(a_raw), decode(b_raw)
    negative = a.negative != b.negative
    pc = {0: 24, 2: 53, 3: 64}.get(control >> 8 & 3)
    if pc is None:
        raise NotRepresented("reserved precision control")
    if "pc-ignored" in defects:
        pc = 64
    rc = 0 if "rc-ignored" in defects else control >> 10 & 3
    sign = 1 << 79 if negative else 0
    infinity = sign | (0x7FFF << 64) | (1 << 63)
    if a.kind in ("qnan", "snan") or b.kind in ("qnan", "snan"):
        raw, invalid = _nan_result(a, a_raw, b, b_raw)
        flags |= IE if invalid else 0
    elif {a.kind, b.kind} == {"zero", "inf"}:
        raw, flags = (sign, 0) if "inf-zero-ok" in defects else (INDEFINITE_RAW, IE)
    else:
        if (a.kind == "denormal" or b_denormal) and "no-de" not in defects:
            flags |= DE
        if "inf" in (a.kind, b.kind):
            raw = infinity
        elif "zero" in (a.kind, b.kind):
            raw = sign
        else:
            exact = value_of(a) * value_of(b)
            unbounded = _round(exact, negative, rc, pc, -16382, False)
            rounded = _round(exact, negative, rc, pc, -16382, True)
            tiny = (exact if "tiny-before" in defects else unbounded) < Fraction(2) ** -16382
            if unbounded >= Fraction(2) ** 16384:
                flags |= OE | PE
                to_inf = rc == 0 or (rc == 2 and not negative) or (rc == 1 and negative)
                largest = (Fraction(2) ** pc - 1) * Fraction(2) ** (16384 - pc)
                raw = infinity if to_inf else encode(largest, negative)
                c1 = int(to_inf)
            else:
                raw = encode(rounded, negative)
                if rounded != exact:
                    flags |= PE | (UE if tiny else 0)
                    c1 = int(rounded > exact)
            if "pe-missing" in defects:
                flags &= ~PE
            if "c1-stuck" in defects:
                c1 = 1
    physical, tags = _with_slot(state, top, raw, tag_of(raw))
    return _finish(state, flags, c1, tags, physical, top, defects)
