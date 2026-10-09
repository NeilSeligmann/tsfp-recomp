# SPDX-License-Identifier: GPL-3.0-or-later
"""Who holds the address of a static object? Counts of references, by kind.

Used to ask of each precompiled vertex program: is its address ever taken, and how. A
program whose address appears nowhere is either dead or reached some way this scan
cannot see, and which of the two it is changes how many programs the title can use.

Two kinds of reference are counted separately because they mean different things:

    code    an immediate or absolute displacement in a decoded instruction, i.e. a
            `push 0x4b9190` or `mov eax, [0x4b9190]`
    data    a 4-byte-aligned dword equal to the address, anywhere in the initialised
            bytes: a pointer table, a struct field, a jump-table entry

A register-relative access into a table (`[reg*4 + base]`) names the table's base and
not each element, so a program reached only through such a table shows `data` references
and no `code` reference to its own address. That is expected and is why both are reported.
"""

from __future__ import annotations

import struct
from collections import Counter
from collections.abc import Iterable, Sequence

import capstone
from capstone import x86 as cs_x86

from tools.shaderscan.image import Image


def code_references(insns: Sequence[capstone.CsInsn], addresses: Iterable[int]) -> Counter[int]:
    """Instructions whose immediate or absolute displacement equals one of `addresses`."""
    wanted = set(addresses)
    counts: Counter[int] = Counter()
    for insn in insns:
        if capstone.CS_GRP_BRANCH_RELATIVE in insn.groups:
            continue
        for operand in insn.operands:
            if operand.type == cs_x86.X86_OP_IMM:
                value = operand.imm & 0xFFFFFFFF
            elif operand.type == cs_x86.X86_OP_MEM and operand.mem.base == 0:
                value = operand.mem.disp & 0xFFFFFFFF
            else:
                continue
            if value in wanted:
                counts[value] += 1
    return counts


def data_references(image: Image, addresses: Iterable[int]) -> Counter[int]:
    """4-byte-aligned dwords in any section's initialised bytes equal to an address."""
    wanted = set(addresses)
    counts: Counter[int] = Counter()
    for section in image.xbe.sections:
        body = image.raw[section.raw_addr : section.raw_addr + section.raw_size]
        usable = len(body) - len(body) % 4
        for (value,) in struct.iter_unpack("<I", body[:usable]):
            if value in wanted:
                counts[value] += 1
    return counts


def _is_global_operand(operand: cs_x86.X86Op, address: int) -> bool:
    return (
        operand.type == cs_x86.X86_OP_MEM
        and operand.mem.base == 0
        and operand.mem.index == 0
        and operand.mem.disp & 0xFFFFFFFF == address
    )


#: Instructions to look ahead from a load of the global for the `or`/`and` and store.
RMW_WINDOW = 14


def global_modifications(
    insns: Sequence[capstone.CsInsn], address: int
) -> Counter[tuple[str, int]]:
    """Every `(operation, immediate)` the code applies to the dword at `address`.

    Two shapes: a direct read-modify-write (`or dword ptr [G], imm`) and a load into a
    register, an `or`/`and` of that register against an immediate, and a store back
    within `RMW_WINDOW` instructions. The second is looked for across branches because
    a compiler places the two arms of an if/else between the load and the store.
    A register that is stored somewhere else, or never stored back, contributes nothing.
    """
    found: Counter[tuple[str, int]] = Counter()
    for index, insn in enumerate(insns):
        operands = insn.operands
        if (
            insn.mnemonic in ("or", "and", "xor")
            and len(operands) == 2
            and _is_global_operand(operands[0], address)
            and operands[1].type == cs_x86.X86_OP_IMM
        ):
            found[(insn.mnemonic, operands[1].imm & 0xFFFFFFFF)] += 1
            continue
        if not (
            insn.mnemonic == "mov"
            and len(operands) == 2
            and operands[0].type == cs_x86.X86_OP_REG
            and _is_global_operand(operands[1], address)
        ):
            continue
        register = operands[0].reg
        window = insns[index + 1 : index + 1 + RMW_WINDOW]
        stores_back = any(
            later.mnemonic == "mov"
            and len(later.operands) == 2
            and _is_global_operand(later.operands[0], address)
            and later.operands[1].type == cs_x86.X86_OP_REG
            and later.operands[1].reg == register
            for later in window
        )
        if not stores_back:
            continue
        for later in window:
            if (
                later.mnemonic in ("or", "and", "xor")
                and len(later.operands) == 2
                and later.operands[0].type == cs_x86.X86_OP_REG
                and later.operands[0].reg == register
                and later.operands[1].type == cs_x86.X86_OP_IMM
            ):
                found[(later.mnemonic, later.operands[1].imm & 0xFFFFFFFF)] += 1
    return found
