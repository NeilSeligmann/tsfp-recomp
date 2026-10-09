# SPDX-License-Identifier: GPL-3.0-or-later
"""Model-arbitrated original execution of 003C9DE8 (T1510).

The original (authenticated byte for byte below) is
    fxch st1; L: fprem; wait; fnstsw ax; wait; sahf; jp L; fstp st1; ret
Unicorn runs the original bytes with the FPU instructions and the fnstsw replaced by NOPs. A code
hook applies the independent models (`x87_fprem_model.fxch_st1/fstp_st1` and
`x87_fprem_full_model.fprem_full`) at each site and writes the model's status word into AX at the
`fprem` site (what `fnstsw ax` would have stored). SAHF and JP stay real instructions, so the
retry loop is the original control flow: JP is taken while C2 (AH bit 2) is set. Same contract as
`x87_root_oracle.ArbitratedOracle` (that class and the four-root arbiter files are untouched, so
the fingerprint of the four admitted roots does not change): a model refusal, an unreadable
state or more than ``MAX_ITERATIONS`` fprem executions stops the run and is returned as
`unrepresented`, never skipped.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from tools.harness.model import Case, ExecResult
from tools.harness.oracle import UnicornOracle
from tools.harness.seeding import GUEST_LO
from tools.harness.x87_fprem_full_model import MAX_ITERATIONS, fprem_full
from tools.harness.x87_fprem_model import fstp_st1, fxch_st1
from tools.harness.x87_state import X87State
from tools.harness.x87_store_model import NotRepresented

ROOT = 0x3C9DE8
SIZE = 14
ORIGINAL_HEX = "d9c9d9f89bdfe09b9e7af7ddd9c3"
FXCH, FPREM, FSTP = 0x3C9DE8, 0x3C9DEA, 0x3C9DF3
#: (start, end) of the bytes replaced by NOPs: fxch; fprem wait fnstsw wait; fstp. SAHF/JP stay.
NOPPED = ((0x3C9DE8, 0x3C9DEA), (0x3C9DEA, 0x3C9DF0), (0x3C9DF3, 0x3C9DF5))
REG_ORDER = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


@dataclass
class Arbitrated:
    result: ExecResult
    x87: X87State | None
    unrepresented: str | None = None
    trace: list[dict[str, Any]] = field(default_factory=list)


def authenticate(image: bytes, base: int = GUEST_LO) -> None:
    offset = ROOT - base
    if image[offset : offset + SIZE].hex() != ORIGINAL_HEX:
        raise ValueError("3C9DE8 original bytes failed authentication")


def nop_sites(image: bytes, base: int = GUEST_LO) -> bytes:
    authenticate(image, base)
    patched = bytearray(image)
    for start, end in NOPPED:
        patched[start - base : end - base] = b"\x90" * (end - start)
    return bytes(patched)


class FpremRootOracle:
    def __init__(self, image: bytes, defects: frozenset[str] = frozenset()) -> None:
        from unicorn import UC_HOOK_CODE
        from unicorn import x86_const as x86

        self._eax = x86.UC_X86_REG_EAX
        self._defects = defects
        self._oracle = UnicornOracle(nop_sites(image))
        self._state: X87State | None = None
        self._failure: str | None = None
        self._trace: list[dict[str, Any]] = []
        self._iterations = 0
        self._oracle._uc.hook_add(UC_HOOK_CODE, self._on_site, begin=FXCH, end=FSTP + 2)

    def _on_site(self, uc: Any, address: int, _size: int, _data: Any) -> None:
        if self._failure is not None or self._state is None:
            return
        try:
            if address == FXCH:
                self._state = fxch_st1(self._state)
                self._trace.append({"va": address, "op": "fxch"})
            elif address == FPREM:
                self._iterations += 1
                if self._iterations > MAX_ITERATIONS:
                    raise NotRepresented("FPREM loop did not converge within the iteration cap")
                self._state = fprem_full(self._state, self._defects)
                status = self._state.status
                eax = uc.reg_read(self._eax)
                uc.reg_write(self._eax, (eax & 0xFFFF0000) | status)
                self._trace.append({"va": address, "op": "fprem", "status": status})
            elif address == FSTP:
                self._state = fstp_st1(self._state)
                self._trace.append({"va": address, "op": "fstp"})
        except NotRepresented as error:
            self._failure = f"{address:08X}: {error}"
            uc.emu_stop()

    def run(self, case: Case, state: X87State) -> Arbitrated:
        if case.x87_state is not None:
            raise ValueError("arbitrated runs keep x87 state in the model, not in Unicorn")
        self._state, self._failure, self._trace, self._iterations = state, None, [], 0
        result = self._oracle.run(case)
        trace = list(self._trace)
        if self._failure is not None:
            return Arbitrated(result, None, self._failure, trace)
        return Arbitrated(result, self._state, None, trace)
