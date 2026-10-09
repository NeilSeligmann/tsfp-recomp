# SPDX-License-Identifier: GPL-3.0-or-later
"""Original-validated, bounded intraprocedural jump tables for replacement proof.

This is deliberately narrower than arbitrary indirect control flow. A table
must be reached through an adjacent unsigned bound check, and every target
must be a decoded instruction in the same function. Oracle execution also
guards the original body/table identity; modifying compiler data is outside
this proof contract and produces no verdict rather than invented execution.
"""

from __future__ import annotations

import hashlib
from collections.abc import Callable
from dataclasses import dataclass

import capstone
from capstone import x86


@dataclass(frozen=True)
class StaticJumpTable:
    site: int
    register: str
    address: int
    targets: tuple[int, ...]
    body_va: int
    body_size: int
    body_sha256: str

    @property
    def original_bytes(self) -> bytes:
        return b"".join(target.to_bytes(4, "little") for target in self.targets)

    def document(self) -> dict[str, object]:
        return {
            "site": f"0x{self.site:08x}",
            "index_register": self.register,
            "address": f"0x{self.address:08x}",
            "targets": [f"0x{target:08x}" for target in self.targets],
            "body_va": f"0x{self.body_va:08x}",
            "body_size": self.body_size,
            "body_sha256": self.body_sha256,
        }


def prove_static_tables(
    va: int,
    size: int,
    code: bytes,
    read_code: Callable[[int, int], bytes],
    *,
    allow_calls: bool = False,
) -> tuple[StaticJumpTable, ...] | None:
    """Prove only CMP r32,imm / JA default / JMP [r32*4+absolute].

    Direct or other indirect entries bypassing the CMP are refused. Table
    data must be outside the decoded body, entirely readable without uint32
    wrap, and contain at most 256 in-body instruction-boundary targets.
    Calls are excluded by default. Explicit allow_calls permits calls elsewhere,
    never between the adjacent CMP/JA/JMP; this is static planning only and
    supplies no new closure/runtime admission. Call entry into JA/JMP refuses.
    """
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    instructions = list(decoder.disasm(code, va))
    if len(code) != size or sum(i.size for i in instructions) != size:
        return None
    boundaries = {i.address for i in instructions}
    tables: list[StaticJumpTable] = []
    protected: set[int] = set()
    for ordinal, instruction in enumerate(instructions):
        if instruction.mnemonic.startswith(("call", "lcall")) and not allow_calls:
            return None
        if instruction.mnemonic != "jmp" or instruction.operands[0].type == x86.X86_OP_IMM:
            continue
        if (
            ordinal < 2
            or instruction.operands[0].type != x86.X86_OP_MEM
            or instruction.operands[0].size != 4
        ):
            return None
        memory = instruction.operands[0].mem
        index = decoder.reg_name(memory.index)
        if (
            memory.base != 0
            or memory.segment != 0
            or memory.scale != 4
            or index not in {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"}
        ):
            return None
        comparison, rejection = instructions[ordinal - 2 : ordinal]
        if (
            comparison.mnemonic != "cmp"
            or len(comparison.operands) != 2
            or comparison.operands[0].type != x86.X86_OP_REG
            or comparison.operands[0].size != 4
            or comparison.operands[0].reg != memory.index
            or comparison.operands[1].type != x86.X86_OP_IMM
            or not 0 <= comparison.operands[1].imm <= 255
            or rejection.mnemonic != "ja"
            or len(rejection.operands) != 1
            or rejection.operands[0].type != x86.X86_OP_IMM
            or rejection.operands[0].imm not in boundaries
        ):
            return None
        address = memory.disp & 0xFFFFFFFF
        count = comparison.operands[1].imm + 1
        end = address + count * 4
        if end > 1 << 32 or (address < va + size and va < end):
            return None
        data = read_code(address, count * 4)
        if len(data) != count * 4:
            return None
        targets = tuple(int.from_bytes(data[i : i + 4], "little") for i in range(0, len(data), 4))
        if not all(target in boundaries for target in targets):
            return None
        protected.update((rejection.address, instruction.address))
        tables.append(
            StaticJumpTable(
                instruction.address,
                index,
                address,
                targets,
                va,
                size,
                hashlib.sha256(code).hexdigest(),
            )
        )
    # A separate branch into JA/JMP would evade the bound check. This includes
    # entries from another table, not merely the ordinary relative branches.
    for instruction in instructions:
        if allow_calls and capstone.CS_GRP_CALL in instruction.groups:
            if any(
                operand.type == x86.X86_OP_IMM and operand.imm in protected
                for operand in instruction.operands
            ):
                return None
            # CALL is also CS_GRP_BRANCH_RELATIVE; outside-node callees are
            # validated separately by plan_calls, not mistaken for jump arms.
            continue
        if (
            capstone.CS_GRP_JUMP in instruction.groups
            or capstone.CS_GRP_BRANCH_RELATIVE in instruction.groups
        ):
            for operand in instruction.operands:
                if operand.type == x86.X86_OP_IMM:
                    if operand.imm not in boundaries or operand.imm in protected:
                        return None
    if any(target in protected for table in tables for target in table.targets):
        return None
    return tuple(tables)
