# SPDX-License-Identifier: GPL-3.0-or-later
"""FPREM with the incomplete-reduction path, and the 3C9DE8 retry loop (T1510).

Extends `x87_fprem_model.py` (left byte-identical, so the arbiter fingerprint of the four admitted
roots does not change) with the part that model refuses: an exponent difference D >= 64.

Intel SDM FPREM pseudo-code (the part that is specified):
    D <- exponent(ST(0)) - exponent(ST(1))
    D < 64:  Q <- trunc(ST(0)/ST(1)), ST(0) <- ST(0) - ST(1)*Q, C2 <- 0,
             C0 <- Q2, C3 <- Q1, C1 <- Q0
    D >= 64: C2 <- 1, N <- an implementation-dependent number between 32 and 63,
             QQ <- trunc((ST(0)/ST(1)) / 2^(D-N)), ST(0) <- ST(0) - ST(1)*QQ*2^(D-N)
and "when the reduction is incomplete C0, C1 and C3 are undefined". Everything the SDM leaves
implementation-dependent is therefore a POLICY here, labelled as such and measured, not derived:

  * N = 32 + (D mod 32) (``N_POLICY``). MEASURED on the host x87 in this container (the same
    rule QEMU/xemu's helper_fprem uses, from memory of its source, not read here). The SDM allows
    any N in 32..63, so this is NOT a statement about a Pentium III. For a fixed final remainder
    N does not matter (the loop converges on ST(0) mod ST(1) whatever N is); N DOES change the
    quotient bits C0/C1/C3 of the LAST iteration (they are the low bits of the last partial
    quotient) and the number of iterations, hence the final AX.
  * C0 = C1 = C3 = 0 on an incomplete iteration (MEASURED on the host, never set).
  * No exponent window: an earlier draft refused operands outside the double range after a
    calibration probe appeared to show host QNaN results there. That finding was WRONG (the probe
    ignored the tag word: the rows had an EMPTY ST(1), a stack underflow the base model already
    refuses). With tags respected the host and this model agree over the whole normal exponent
    range, see docs/replace-x87-t1510.md (correction). The earlier receipts are retained in
    docs/evidence/t1510/fprem_full/retained-window-v1/.

Everything the base model refuses (NaN, invalid, empty stack, denormal remainder, unmasked
exceptions) stays refused.
"""

from __future__ import annotations

from fractions import Fraction

from tools.harness.x87_arith_model import decode, encode, tag_of, value_of
from tools.harness.x87_fprem_model import C0, C2, C3, fprem
from tools.harness.x87_state import X87State
from tools.harness.x87_store_model import C1, NotRepresented, _floor_log2

MAX_ITERATIONS = 2048
DEFECTS = frozenset(
    {
        "n-fixed-32",
        "n-fixed-63",
        "n-policy-off-by-one",
        "incomplete-q-bits",
        "incomplete-c2-clear",
        "incomplete-sign-lost",
        "loop-stops-early",
    }
)


def n_policy(difference: int, defects: frozenset[str] = frozenset()) -> int:
    if "n-fixed-32" in defects:
        return 32
    if "n-fixed-63" in defects:
        return 63
    if "n-policy-off-by-one" in defects:
        return 32 + (difference + 1) % 32
    return 32 + difference % 32


def fprem_full(state: X87State, defects: frozenset[str] = frozenset()) -> X87State:
    """One FPREM iteration, complete or incomplete."""
    top = state.top
    other = (top + 1) & 7
    try:
        return fprem(state)
    except NotRepresented as error:
        if "incomplete reduction" not in str(error):
            raise
    dividend, divisor = decode(state.physical[top]), decode(state.physical[other])
    if dividend.kind != "normal" or divisor.kind != "normal":
        raise NotRepresented("incomplete reduction needs two normal operands")
    a, b = abs(value_of(dividend)), abs(value_of(divisor))
    difference = _floor_log2(a) - _floor_log2(b)
    n = n_policy(difference, defects)
    scale = Fraction(2) ** (difference - n)
    ratio = a / b / scale
    partial = ratio.numerator // ratio.denominator
    remainder = a - b * partial * scale
    negative = False if "incomplete-sign-lost" in defects else dividend.negative
    raw = encode(remainder, negative)
    status = state.status & ~(C0 | C1 | C2 | C3)
    if "incomplete-c2-clear" not in defects:
        status |= C2
    if "incomplete-q-bits" in defects:
        status |= (partial >> 2 & 1) * C0 | (partial >> 1 & 1) * C3 | (partial & 1) * C1
    physical = list(state.physical)
    physical[top] = raw
    tags = (state.tags & ~(3 << top * 2)) | (tag_of(raw) << top * 2)
    return X87State(tuple(physical), tags, state.control, status)


def fprem_loop(
    state: X87State, defects: frozenset[str] = frozenset()
) -> tuple[X87State, list[int]]:
    """Iterate like `L: fprem; fnstsw ax; sahf; jp L`: returns the final state and every status."""
    statuses: list[int] = []
    for _ in range(MAX_ITERATIONS):
        state = fprem_full(state, defects)
        statuses.append(state.status)
        if not state.status & C2 or ("loop-stops-early" in defects and len(statuses) == 2):
            return state, statuses
    raise NotRepresented("FPREM loop did not converge within the iteration cap")
