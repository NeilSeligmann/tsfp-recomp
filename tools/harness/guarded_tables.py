# SPDX-License-Identifier: GPL-3.0-or-later
"""T1576 guarded jump-table prover. Opt-in (guarded-jump-schema3) only.

`static_tables.prove_static_tables` is unchanged and keeps serving every existing receipt.
This module adds exactly two shapes that prover refuses, and nothing else:

* up to two `movaps xmmA, xmmB` fillers between the CMP and the JA (an instruction scheduler
  moves them there, they write no GPR, no flag and touch no memory), and
* the two-level MSVC switch  CMP r,N / JA d / MOVZX r2,byte [r+T1] / JMP [r2*4+T2]. The byte
  table T1 holds N+1 entries, the jump table T2 holds max(byte)+1 slots, both are hashed and
  guarded exactly like a one-level table.

Everything else (adjacent unsigned bound, in-body decoded targets, bypass refusals, no overlap,
no 32-bit wrap) is identical. Static facts only: a decoded plan is not runtime evidence.
"""

from __future__ import annotations

import hashlib
from collections.abc import Callable
from dataclasses import dataclass

import capstone
from capstone import x86

from .static_tables import StaticJumpTable

_INDEX_REGISTERS = frozenset({"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"})
MAX_FILLERS = 2


@dataclass(frozen=True)
class GuardedJumpTable(StaticJumpTable):
    """A StaticJumpTable plus the dispatch guard facts the arm witness needs."""

    index_table_address: int = 0
    index_table: bytes = b""
    guard_site: int = 0  # the JA
    default_target: int = 0  # the JA destination
    bound: int = -1  # CMP immediate

    @property
    def regions(self) -> tuple[tuple[int, bytes], ...]:
        """Every guarded data region (address, original bytes), jump table first."""
        out = [(self.address, self.original_bytes)]
        if self.index_table:
            out.append((self.index_table_address, self.index_table))
        return tuple(out)

    def document(self) -> dict[str, object]:
        document = super().document()
        document["guard_site"] = f"0x{self.guard_site:08x}"
        document["default_target"] = f"0x{self.default_target:08x}"
        document["bound"] = self.bound
        if self.index_table:
            document["index_table_address"] = f"0x{self.index_table_address:08x}"
            document["index_table_sha256"] = hashlib.sha256(self.index_table).hexdigest()
            document["index_table"] = self.index_table.hex()
        return document


def table_regions(table: StaticJumpTable) -> tuple[tuple[int, bytes], ...]:
    """Guarded data regions of any table (legacy tables have exactly the jump table)."""
    if isinstance(table, GuardedJumpTable):
        return table.regions
    return ((table.address, table.original_bytes),)


def _xmm_register_move(instruction: capstone.CsInsn) -> bool:
    operands = instruction.operands
    return (
        instruction.mnemonic == "movaps"
        and len(operands) == 2
        and all(op.type == x86.X86_OP_REG for op in operands)
        and all(x86.X86_REG_XMM0 <= op.reg <= x86.X86_REG_XMM7 for op in operands)
    )


def prove_guarded_tables(
    va: int,
    size: int,
    code: bytes,
    read_code: Callable[[int, int], bytes],
    *,
    allow_calls: bool = True,
    allow_tails: bool = False,
) -> tuple[GuardedJumpTable, ...] | None:
    """Prove the guarded shapes above, or return None (the caller must then refuse).

    `allow_tails` lets a direct out-of-body JMP pass untouched. The exact-entry check of such a
    tail is the closure's and the root plan's job, never this function's. An out-of-body
    conditional branch is always refused."""
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    instructions = list(decoder.disasm(code, va))
    if len(code) != size or sum(i.size for i in instructions) != size:
        return None
    boundaries = {i.address for i in instructions}
    tables: list[GuardedJumpTable] = []
    protected: set[int] = set()
    claimed: list[tuple[int, int]] = []
    for ordinal, instruction in enumerate(instructions):
        if instruction.mnemonic.startswith(("call", "lcall")) and not allow_calls:
            return None
        if instruction.mnemonic != "jmp" or instruction.operands[0].type == x86.X86_OP_IMM:
            continue
        if instruction.operands[0].type != x86.X86_OP_MEM or instruction.operands[0].size != 4:
            return None
        memory = instruction.operands[0].mem
        index = decoder.reg_name(memory.index)
        if (
            memory.base != 0
            or memory.segment != 0
            or memory.scale != 4
            or index not in _INDEX_REGISTERS
        ):
            return None
        cursor = ordinal - 1
        index_table_address = 0
        index_table = b""
        source_register = memory.index
        if cursor >= 0 and instructions[cursor].mnemonic == "movzx":
            widen = instructions[cursor]
            if (
                len(widen.operands) != 2
                or widen.operands[0].type != x86.X86_OP_REG
                or widen.operands[0].reg != memory.index
                or widen.operands[1].type != x86.X86_OP_MEM
                or widen.operands[1].size != 1
            ):
                return None
            byte_memory = widen.operands[1].mem
            if (
                byte_memory.base == 0
                or byte_memory.index != 0
                or byte_memory.segment != 0
                or decoder.reg_name(byte_memory.base) not in _INDEX_REGISTERS
            ):
                return None
            source_register = byte_memory.base
            index_table_address = byte_memory.disp & 0xFFFFFFFF
            cursor -= 1
            protected.add(widen.address)
        if cursor < 1:
            return None
        rejection = instructions[cursor]
        if (
            rejection.mnemonic != "ja"
            or len(rejection.operands) != 1
            or rejection.operands[0].type != x86.X86_OP_IMM
            or rejection.operands[0].imm not in boundaries
            or rejection.operands[0].imm == rejection.address + rejection.size
        ):
            return None
        cursor -= 1
        fillers = 0
        while cursor >= 0 and _xmm_register_move(instructions[cursor]):
            protected.add(instructions[cursor].address)
            fillers += 1
            cursor -= 1
        if fillers > MAX_FILLERS or cursor < 0:
            return None
        comparison = instructions[cursor]
        if (
            comparison.mnemonic != "cmp"
            or len(comparison.operands) != 2
            or comparison.operands[0].type != x86.X86_OP_REG
            or comparison.operands[0].size != 4
            or comparison.operands[0].reg != source_register
            or comparison.operands[1].type != x86.X86_OP_IMM
            or not 0 <= comparison.operands[1].imm <= 255
        ):
            return None
        bound = comparison.operands[1].imm
        if index_table_address:
            span = bound + 1
            end = index_table_address + span
            if end > 1 << 32 or (index_table_address < va + size and va < end):
                return None
            index_table = read_code(index_table_address, span)
            if len(index_table) != span:
                return None
            claimed.append((index_table_address, end))
            count = max(index_table) + 1
        else:
            count = bound + 1
        address = memory.disp & 0xFFFFFFFF
        end = address + count * 4
        if end > 1 << 32 or (address < va + size and va < end):
            return None
        data = read_code(address, count * 4)
        if len(data) != count * 4:
            return None
        claimed.append((address, end))
        targets = tuple(int.from_bytes(data[i : i + 4], "little") for i in range(0, len(data), 4))
        if not all(target in boundaries for target in targets):
            return None
        protected.update((rejection.address, instruction.address))
        tables.append(
            GuardedJumpTable(
                instruction.address,
                index,
                address,
                targets,
                va,
                size,
                hashlib.sha256(code).hexdigest(),
                index_table_address,
                index_table,
                rejection.address,
                rejection.operands[0].imm,
                bound,
            )
        )
    for n, (low, high) in enumerate(claimed):
        if any(low < other_high and other_low < high for other_low, other_high in claimed[:n]):
            return None
    for instruction in instructions:
        if capstone.CS_GRP_CALL in instruction.groups:
            if any(
                operand.type == x86.X86_OP_IMM and operand.imm in protected
                for operand in instruction.operands
            ):
                return None
            continue
        if (
            capstone.CS_GRP_JUMP in instruction.groups
            or capstone.CS_GRP_BRANCH_RELATIVE in instruction.groups
        ):
            for operand in instruction.operands:
                if operand.type != x86.X86_OP_IMM:
                    continue
                if operand.imm in protected:
                    return None
                if operand.imm not in boundaries:
                    if allow_tails and instruction.mnemonic == "jmp":
                        continue
                    return None
    if any(target in protected for table in tables for target in table.targets):
        return None
    return tuple(tables)
