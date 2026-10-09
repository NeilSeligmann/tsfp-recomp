# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact original MOV/LEA/PUSH/CALL first-path flags carry; scratch only."""

from __future__ import annotations

from dataclasses import dataclass

ROOT_PREFIX = bytes.fromhex("568bf1c706d4624b008d4e0cc7460800000000e8f8bbffff")
CALLEE_PREFIX = bytes.fromhex("8bc1c70000000000")
EXECUTION_CAPABILITY = 0


@dataclass(frozen=True)
class PreservedFlags:
    word: int
    valid_mask: int = 0xFFFFFFFF
    boundary_eip: int = 0x3BB658


def export_first_call_flags(
    *, root_prefix: bytes, callee_prefix: bytes, case_df: int, observed_input_flags: int
) -> PreservedFlags:
    """No arithmetic occurred on this exact prefix; retain the actual complete input."""
    if root_prefix != ROOT_PREFIX or callee_prefix != CALLEE_PREFIX:
        raise ValueError("unknown opcode effects or call path")
    if type(case_df) is not int or case_df not in (0, 1):
        raise ValueError("invalid actual case DF")
    if type(observed_input_flags) is not int or observed_input_flags != 0x202 | (case_df << 10):
        raise ValueError("input flags differ from authenticated current case contract")
    return PreservedFlags(observed_input_flags)


def scratch_flags_source() -> str:
    """Wrapper must call begin with actual case DF; invalid/uninitialized carry refuses."""
    return """/* T1508 isolated scratch flags, exact first-call path only; capability0. */
static __thread uint32_t t1508_full_flags;
static __thread int t1508_flags_initialized;
static int t1508_flags_begin(unsigned actual_case_df) {
    t1508_flags_initialized = 0;
    if (actual_case_df > 1u) return 0;
    t1508_full_flags = 0x202u | (actual_case_df << 10);
    t1508_flags_initialized = 1;
    return 1;
}
static int t1508_flags_read(uint32_t *out) {
    if (!t1508_flags_initialized || out == NULL) return 0;
    *out = t1508_full_flags;
    return 1;
}
"""
