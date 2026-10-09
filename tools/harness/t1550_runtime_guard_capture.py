# SPDX-License-Identifier: GPL-3.0-or-later
"""Retain actual stopped-backend evidence for frozen fatal guard controls.

This diagnostic helper does not execute, classify a guest fault, or grant credit.
It reads the existing backend before the controller disposes/resets that backend.
"""

from __future__ import annotations

from .code_safety import NamedGlobalCodeSafetyError
from .scoped_fixture_code import ScopedFixtureSafetyError


def capture(oracle: object, error: NamedGlobalCodeSafetyError) -> dict[str, object]:
    if type(error) not in (NamedGlobalCodeSafetyError, ScopedFixtureSafetyError):
        raise ValueError("actual campaign-fatal code-safety error required")
    record = dict(
        violation=dict(error.observed),
        architectural_fault_state_claim=False,
        proof_credit=False,
    )
    # Preserve the initial failure even if an optional stopped-backend read fails.
    try:
        record["executed_instructions"] = oracle._insns
        record["touched_pages"] = sorted(oracle._touched)
        actual = bytes(oracle._uc.mem_read(0x10207, 1))
        record["code_write_control"] = dict(
            address=0x10207, initial_hex="40", actual_hex=actual.hex()
        )
    except Exception as read_error:
        record["diagnostic_read_error"] = repr(read_error)
    return record
