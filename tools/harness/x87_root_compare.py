# SPDX-License-Identifier: GPL-3.0-or-later
"""Predeclared comparator contract for lifted x87 root native-versus-original runs (T1510).

Declared before any outcome. Pure; no Unicorn, no compiler.

Reference side ("arbitrated original"): the ORIGINAL guest bytes executed by Unicorn with every
authenticated x87 instruction (FLD/FMUL/FSTP m32) replaced by the independent SDM model
(`x87_arith_model`, `x87_store_model`). Integer registers, control flow, stack and every memory
byte therefore come from the original code, and the only x87 inputs are the model's. Unicorn's own
x87 arithmetic is never an arbiter for these instructions (its disagreements with the model are
recorded as a diagnostic only, `unicorn_x87`).

AGREE requires ALL of, with no mask and no tolerance:
  * all eight integer registers equal;
  * the byte-granular write set (address -> final byte, only changed bytes) identical, including
    the bytes written by FSTP (the model supplies them on the reference side);
  * the complete raw x87 state equal: eight physical raw80 slots, tags, control, status.

Fail-closed outcomes, never AGREE and never skipped: ORACLE-FAULT (the original faulted or hit the
count limit), NATIVE-REFUSED (the native runner refused, died or returned no record),
UNREPRESENTED (the model refused a state, operand or outcome outside its masked-exception domain),
MISSING (a required observation is absent from either side).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from tools.harness.x87_state import X87State, differences

REG_NAMES = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


@dataclass(frozen=True)
class Side:
    regs: tuple[int, ...] | None
    writes: dict[int, int] | None
    x87: X87State | None
    fault: str | None = None


@dataclass(frozen=True)
class Verdict:
    outcome: str
    components: tuple[str, ...] = field(default=())

    @property
    def agree(self) -> bool:
        return self.outcome == "AGREE"


def compare(
    reference: Side,
    native: Side,
    *,
    unrepresented: str | None = None,
    ignored_registers: frozenset[str] = frozenset(),
    ignored_write_ranges: tuple[tuple[int, int], ...] = (),
) -> Verdict:
    """`ignored_registers` and `ignored_write_ranges` (half-open) are the declared scratch
    registers and dead stack of a hand replacement, exactly as the production comparator takes
    them (`tools.harness.compare.compare_case`). They default to empty, which is the strict
    lifted-root contract above; the x87 state, fail-closed outcomes and every other register or
    write are never narrowed."""
    if unrepresented is not None:
        return Verdict("UNREPRESENTED", (unrepresented,))
    if reference.fault is not None:
        return Verdict("ORACLE-FAULT", (reference.fault,))
    if native.fault is not None:
        return Verdict("NATIVE-REFUSED", (native.fault,))
    missing = tuple(
        f"{name}:{side_name}"
        for side_name, side in (("reference", reference), ("native", native))
        for name, value in (("regs", side.regs), ("writes", side.writes), ("x87", side.x87))
        if value is None
    )
    if missing:
        return Verdict("MISSING", missing)
    assert reference.regs is not None and native.regs is not None
    assert reference.writes is not None and native.writes is not None
    components: list[str] = []
    if len(reference.regs) != 8 or len(native.regs) != 8:
        return Verdict("MISSING", ("regs:width",))
    components.extend(
        f"reg:{name}"
        for name, left, right in zip(REG_NAMES, reference.regs, native.regs, strict=True)
        if left != right and name not in ignored_registers
    )
    for address in sorted(set(reference.writes) | set(native.writes)):
        if any(low <= address < high for low, high in ignored_write_ranges):
            continue
        if reference.writes.get(address) != native.writes.get(address):
            components.append(f"write:{address:08X}")
    components.extend(f"x87:{item}" for item in differences(reference.x87, native.x87))
    return Verdict("DISAGREE" if components else "AGREE", tuple(components))
