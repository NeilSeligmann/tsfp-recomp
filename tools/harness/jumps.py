# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared near-direct JMP classification; no indirect/tail admission authority."""

import re

import capstone

_DIRECT_TARGET = re.compile(r"^0x[0-9a-f]+$")


def direct_jump_target(insn: capstone.CsInsn) -> int | None:
    """Preserve legacy text-only decoder support and reject register/memory/far JMP."""
    text = insn.op_str.strip().lower()
    if insn.mnemonic.lower() != "jmp" or not _DIRECT_TARGET.fullmatch(text):
        return None
    return int(text, 16)


def inbody_direct_jump(insn: capstone.CsInsn, va: int, size: int) -> bool:
    target = direct_jump_target(insn)
    return target is not None and va <= target < va + size


def direct_tail_jump(insn: capstone.CsInsn, va: int, size: int) -> bool:
    target = direct_jump_target(insn)
    return target is not None and not va <= target < va + size
