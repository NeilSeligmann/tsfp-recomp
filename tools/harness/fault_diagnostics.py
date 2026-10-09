# SPDX-License-Identifier: GPL-3.0-or-later
"""Incomplete stopped-backend observations; never architectural fault equivalence."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass

MISSING_SEMANTICS = (
    "precise-architectural-exception",
    "fault-ip-access-width",
    "committed-access-order",
    "consumed-call-ret-control",
    "complete-applicable-machine-state",
)


@dataclass(frozen=True)
class FaultObservation:
    origin: str
    label: str
    regs: tuple[int, ...] | None = None
    flags: int | None = None
    backend_ip: int | None = None
    changed_bytes: tuple[tuple[int, int], ...] | None = None
    capture_errors: tuple[str, ...] = ()
    schema: str = "fault-diagnostic-v1"

    def __post_init__(self) -> None:
        if (
            self.schema != "fault-diagnostic-v1"
            or self.origin not in ("oracle-stopped-backend", "subject-legacy-unavailable")
            or type(self.label) is not str
            or not self.label
        ):
            raise ValueError("invalid fault diagnostic identity")
        for value in (self.flags, self.backend_ip):
            if value is not None and (type(value) is not int or not 0 <= value <= 0xFFFFFFFF):
                raise ValueError("fault diagnostic requires unsigned32 observation")
        if self.regs is not None and (
            type(self.regs) is not tuple
            or len(self.regs) != 8
            or any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in self.regs)
        ):
            raise ValueError("fault diagnostic requires eight unsigned32 registers")
        if self.changed_bytes is not None:
            if (
                type(self.changed_bytes) is not tuple
                or any(
                    type(pair) is not tuple
                    or len(pair) != 2
                    or type(pair[0]) is not int
                    or not 0 <= pair[0] <= 0xFFFFFFFF
                    or type(pair[1]) is not int
                    or not 0 <= pair[1] <= 255
                    for pair in self.changed_bytes
                )
                or tuple(sorted(dict(self.changed_bytes).items())) != self.changed_bytes
            ):
                raise ValueError("fault diagnostic requires unique sorted byte observations")
        if type(self.capture_errors) is not tuple or any(
            type(e) is not str for e in self.capture_errors
        ):
            raise ValueError("invalid fault capture errors")
        if self.origin == "subject-legacy-unavailable" and any(
            value is not None
            for value in (self.regs, self.flags, self.backend_ip, self.changed_bytes)
        ):
            raise ValueError("legacy subject fault cannot claim captured state")


def diagnose(left: FaultObservation | None, right: FaultObservation | None) -> str:
    """Versioned JSON diagnostic only; missing memory never means captured empty."""
    for value in (left, right):
        if value is not None and type(value) is not FaultObservation:
            raise ValueError("exact fault observation type required")
    fields = ("regs", "flags", "backend_ip", "changed_bytes")
    compared = []
    differing = []
    unavailable = []
    for name in fields:
        a, b = (None if value is None else getattr(value, name) for value in (left, right))
        if a is None or b is None:
            unavailable.append(name)
        else:
            compared.append(name)
            if a != b:
                differing.append(name)
    return json.dumps(
        dict(
            schema="fault-diagnostic-comparison-v1",
            equivalence="UNESTABLISHED",
            proof_credit=False,
            oracle=None if left is None else asdict(left),
            subject=None if right is None else asdict(right),
            compared=compared,
            differing=differing,
            unavailable=unavailable,
            missing_semantics=MISSING_SEMANTICS,
        ),
        sort_keys=True,
        separators=(",", ":"),
    )


#: T1576 fault parity guard. Oracle labels are Unicorn memory errors, subject labels are host
#: signals. Both map to the one class they can both report; the subject cannot say read versus
#: write versus fetch, so only the class and the guest address are compared (documented limit).
_ORACLE_MEMORY_FAULTS = frozenset(
    f"UC_ERR_{kind}_{why}" for kind in ("READ", "WRITE", "FETCH") for why in ("UNMAPPED", "PROT")
)
_SUBJECT_MEMORY_FAULTS = frozenset(("SEGV", "BUS"))


def fault_parity(
    oracle_fault: str | None,
    oracle_addr: int | None,
    subject_fault: str | None,
    subject_addr: int | None,
) -> tuple[bool, str]:
    """(ok, reason) for two faulted sides. Fail closed: anything unknown is not parity.

    A matched fault stays a non-verdict; this only decides whether a guarded-jump proof may
    keep running past it. Missing addresses, non-memory faults and host-space addresses refuse.
    """
    if oracle_fault not in _ORACLE_MEMORY_FAULTS or subject_fault not in _SUBJECT_MEMORY_FAULTS:
        return False, f"fault class unsupported ({oracle_fault} / {subject_fault})"
    if oracle_addr is None or subject_addr is None:
        return False, "fault address unavailable on at least one side"
    if not 0 <= subject_addr <= 0xFFFFFFFF or not 0 <= oracle_addr <= 0xFFFFFFFF:
        return False, f"fault address not a guest address ({oracle_addr:#x} / {subject_addr:#x})"
    if oracle_addr != subject_addr:
        return False, f"fault address differs ({oracle_addr:#x} vs {subject_addr:#x})"
    return True, "same memory-fault class and guest address"
