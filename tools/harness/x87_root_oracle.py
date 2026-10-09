# SPDX-License-Identifier: GPL-3.0-or-later
"""Model-arbitrated original execution for the four authentic x87 roots (T1510).

The original bytes run in Unicorn, but each authenticated x87 instruction (FLD/FMUL/FSTP m32,
sites from `x87_lift.CONTRACTS`) is overwritten with NOPs in the copy Unicorn executes and a code
hook applies the independent SDM model instead (operand read from guest memory, FSTP result
written to guest memory). Integer state, control flow, stack and memory therefore come from the
original code and only the x87 arithmetic is the model's. An unmodified run is available as a
diagnostic (`run_unicorn_x87`) and is never an arbiter.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from tools.harness.model import Case, ExecResult
from tools.harness.oracle import UnicornOracle
from tools.harness.seeding import GUEST_LO
from tools.harness.x87_arith_model import FLD_DEFECTS, FMUL_DEFECTS, fld_m32, fmul_m32
from tools.harness.x87_lift import CONTRACTS
from tools.harness.x87_state import X87State
from tools.harness.x87_store_model import NotRepresented, fstp_m32

REG_ORDER = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


@dataclass
class Arbitrated:
    result: ExecResult
    x87: X87State | None
    unrepresented: str | None = None
    trace: list[dict[str, Any]] = field(default_factory=list)


def authenticated_sites() -> dict[int, tuple[str, str | None, int]]:
    return {
        operation.va: (operation.mnemonic, operation.base, operation.displacement)
        for contract in CONTRACTS.values()
        for operation in contract.operations
    }


def nop_sites(image: bytes, base: int = GUEST_LO) -> bytes:
    """Return a copy with each authenticated FPU instruction replaced by same-length NOPs."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    patched = bytearray(image)
    for va, (mnemonic, _, _) in authenticated_sites().items():
        offset = va - base
        instruction = next(decoder.disasm(bytes(image[offset : offset + 15]), va), None)
        if instruction is None or instruction.mnemonic != mnemonic:
            raise ValueError(f"site {va:08X} is not the authenticated {mnemonic}")
        patched[offset : offset + instruction.size] = b"\x90" * instruction.size
    return bytes(patched)


class ArbitratedOracle:
    def __init__(self, image: bytes, defects: frozenset[str] = frozenset()) -> None:
        from unicorn import UC_HOOK_CODE
        from unicorn import x86_const as x86

        self._x86 = x86
        self._regs = dict(
            zip(
                REG_ORDER,
                (getattr(x86, f"UC_X86_REG_{n.upper()}") for n in REG_ORDER),
                strict=True,
            )
        )
        self._sites = authenticated_sites()
        self._defects = defects
        self._oracle = UnicornOracle(nop_sites(image))
        self._state: X87State | None = None
        self._failure: str | None = None
        self._trace: list[dict[str, Any]] = []
        low, high = min(self._sites), max(self._sites)
        self._oracle._uc.hook_add(UC_HOOK_CODE, self._on_site, begin=low, end=high + 16)

    def _on_site(self, uc: Any, address: int, _size: int, _data: Any) -> None:
        site = self._sites.get(address)
        if site is None or self._failure is not None or self._state is None:
            return
        mnemonic, base, displacement = site
        effective = ((uc.reg_read(self._regs[base]) if base else 0) + displacement) & 0xFFFFFFFF
        try:
            if mnemonic == "fstp":
                result = fstp_m32(self._state, self._defects)
                data = result.single.to_bytes(4, "little")
                uc.mem_write(effective, data)
                self._oracle._mark_dirty(effective, 4)
                self._trace.append(
                    {"va": address, "op": "fstp", "ea": effective, "bits": result.single}
                )
                self._state = result.state
            else:
                bits = int.from_bytes(bytes(uc.mem_read(effective, 4)), "little")
                model = fld_m32 if mnemonic == "fld" else fmul_m32
                self._state = model(self._state, bits, self._defects)
                self._trace.append({"va": address, "op": mnemonic, "ea": effective, "bits": bits})
        except NotRepresented as error:
            self._failure = f"{mnemonic}@{address:08X}: {error}"
            uc.emu_stop()
        except Exception as error:  # unreadable operand or store: retained, never skipped
            self._failure = f"{mnemonic}@{address:08X}: access {type(error).__name__}"
            uc.emu_stop()

    def run(self, case: Case, state: X87State) -> Arbitrated:
        if case.x87_state is not None:
            raise ValueError("arbitrated runs keep x87 state in the model, not in Unicorn")
        self._state, self._failure, self._trace = state, None, []
        result = self._oracle.run(case)
        trace = list(self._trace)
        if self._failure is not None:
            return Arbitrated(result, None, self._failure, trace)
        return Arbitrated(result, self._state, None, trace)


def run_unicorn_x87(oracle: UnicornOracle, case: Case) -> ExecResult | str:
    """Unmodified original run with Unicorn's own x87 (diagnostic only)."""
    try:
        return oracle.run(case)
    except ValueError as error:
        return f"restore-refused: {error}"


__all__ = [
    "FLD_DEFECTS",
    "FMUL_DEFECTS",
    "Arbitrated",
    "ArbitratedOracle",
    "authenticated_sites",
    "nop_sites",
    "run_unicorn_x87",
]
