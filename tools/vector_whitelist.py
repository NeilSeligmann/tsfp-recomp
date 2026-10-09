# SPDX-License-Identifier: GPL-3.0-or-later
"""T1594: static whole-body vector admission shared by the pilot screen and the draft screen.

A decoded capstone instruction that touches vector state is admitted only when
tools.harness.vector.supported_vector_instruction accepts it (10 exact move/bit mnemonics,
xmm0-7, no segment override). Everything else (x87, MMX, emms, MXCSR and FPU control,
arithmetic, compare, convert, movaps, movd, movq, integer SSE2) keeps the function refused.
"""

from __future__ import annotations

import re
from typing import Any

from tools.harness.vector import (
    supported_scalar_fp_instruction,
    supported_vector_instruction,
    vector_state_instruction,
)

VECTOR_GROUPS = frozenset({"mmx", "sse1", "sse2", "sse3", "ssse3", "sse41", "sse42", "avx"})
VECTOR_OPERAND = re.compile(r"\b(?:x|y|z)?mm\d+\b")
CONTROL_MNEMONICS = frozenset(
    {"emms", "femms", "ldmxcsr", "stmxcsr", "fxsave", "fxrstor", "fldcw", "fnstcw", "fstcw"}
)
SEGMENT = re.compile(r"\b[cdefgs]s:")


def touches_vector(insn: Any) -> bool:
    """True for any SSE/MMX opcode, vector-state control opcode or xmm/mm operand."""
    names = {insn.group_name(group) for group in insn.groups}
    return bool(
        names & VECTOR_GROUPS
        or insn.mnemonic.lower() in CONTROL_MNEMONICS
        or VECTOR_OPERAND.search(insn.op_str)
        or vector_state_instruction(insn)
    )


def has_segment(insn: Any) -> bool:
    """Segment-override or segment-addressed forms are never admitted."""
    from capstone.x86_const import X86_OP_MEM

    if SEGMENT.search(insn.op_str):
        return True
    return any(op.type == X86_OP_MEM and op.mem.segment != 0 for op in insn.operands)


def admissible_vector_instruction(insn: Any) -> bool:
    """One vector-touching instruction passes the whitelist and has no segment form."""
    return supported_vector_instruction(insn) and not has_segment(insn)


def vector_body_admissible(insns: list[Any]) -> bool:
    """True when every vector-touching instruction (live or dead path) is whitelisted."""
    if any(is_x87(i) for i in insns):
        return False
    return all(admissible_vector_instruction(i) for i in insns if touches_vector(i))


def has_admitted_vector(insns: list[Any]) -> bool:
    """T1611: the body holds at least one whitelisted vector instruction and nothing refused."""
    return vector_body_admissible(insns) and any(touches_vector(i) for i in insns)


def is_x87(insn: Any) -> bool:
    """Any x87 opcode (including fldcw, fnstcw) refuses the whole function."""
    return insn.mnemonic.lower().startswith("f") or "fpu" in {
        insn.group_name(group) for group in insn.groups
    }


# ---------------------------------------------------------------------------------------
# T1620 fp-scalar-v1: additive scalar-float admission. The legacy functions above are unchanged.


def _extra_form(insn: Any) -> bool:
    """Operand shape for the hypothetical step 2b rows (unlock measurement only): xmm0-7
    destination, xmm or dword memory source, r32 destination for cvttss2si, register movaps."""
    from capstone.x86_const import X86_OP_MEM, X86_OP_REG

    if len(insn.operands) != 2:
        return False
    left, right = insn.operands
    names = {f"xmm{i}" for i in range(8)}

    def xmm(op: Any) -> bool:
        return op.type == X86_OP_REG and insn.reg_name(op.reg) in names

    def dword(op: Any) -> bool:
        return op.type == X86_OP_MEM and op.size == 4

    mnemonic = insn.mnemonic.lower()
    if mnemonic == "movaps":
        return xmm(left) and xmm(right)
    if mnemonic == "cvttss2si":
        return left.type == X86_OP_REG and (xmm(right) or dword(right))
    return xmm(left) and (xmm(right) or dword(right))


def admissible_scalar_fp_instruction(insn: Any, extra: frozenset[str] = frozenset()) -> bool:
    """One vector-touching instruction passes the legacy whitelist or an exact fp-scalar form.

    `extra` (default empty) names hypothetical mnemonics for the unlock measurement only, the
    production check never passes it."""
    if has_segment(insn):
        return False
    if supported_vector_instruction(insn) or supported_scalar_fp_instruction(insn):
        return True
    return insn.mnemonic.lower() in extra and _extra_form(insn)


def vector_scalar_fp_body_admissible(insns: list[Any], extra: frozenset[str] = frozenset()) -> bool:
    """Every vector-touching instruction (live or dead path) is legacy-whitelisted or an exact
    fp-scalar-v1 form, and no x87 opcode is present."""
    if any(is_x87(i) for i in insns):
        return False
    return all(admissible_scalar_fp_instruction(i, extra) for i in insns if touches_vector(i))


def has_admitted_scalar_fp(insns: list[Any]) -> bool:
    """The body holds at least one whitelisted vector instruction under fp-scalar-v1."""
    return vector_scalar_fp_body_admissible(insns) and any(touches_vector(i) for i in insns)
