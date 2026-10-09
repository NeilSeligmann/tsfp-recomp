# SPDX-License-Identifier: GPL-3.0-or-later
"""Authenticate and adapt only the predeclared isolated three-operation bodies.

This does not register replacements, grant raw driver capability or prove the
integer lifting, fault adapter or complete closure. Canonical inputs are read-only.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass


@dataclass(frozen=True)
class Operation:
    va: int
    mnemonic: str
    base: str | None
    displacement: int

    @property
    def expression(self) -> str:
        address = f"0x{self.displacement:X}"
        return f"{self.base} + {address}" if self.base else address


@dataclass(frozen=True)
class Contract:
    size: int
    original_sha256: str
    generated_sha256: str
    operations: tuple[Operation, ...]


CONTRACTS = {
    0xCB3B0: Contract(
        37,
        "8ee94afef69275ebd019e55701e746406ef3616a01e12f2dce6dec8bca38938a",
        "f04603b0b2b4bb30315e35634ccf86b3ecf51c6c436433c24390e91cb7c24afb",
        (Operation(0xCB3C7, "fld", None, 0x478900), Operation(0xCB3CE, "fld", None, 0x4785CC)),
    ),
    0x1B7D00: Contract(
        97,
        "e1696ccf4425969ae6b5bef50bda622ddc921716b14ba24f12ff60e5ceaa1b1b",
        "036aefc92ef768620771c0a87578f2400d8b6f88dfa10c98dd903f0f982e3134",
        (
            Operation(0x1B7D28, "fmul", "esi", 0x5C),
            Operation(0x1B7D2B, "fstp", "eax", 0x38C),
            Operation(0x1B7D3A, "fmul", "esi", 0x60),
            Operation(0x1B7D40, "fstp", "edx", 0x390),
            Operation(0x1B7D4F, "fmul", "esi", 0x64),
            Operation(0x1B7D58, "fstp", "ecx", 0x394),
        ),
    ),
    0x1B7D70: Contract(
        119,
        "522c740b4a02a4674309ab6c9531074fde4b3e4e34b1d1d82fa71b18f668b29d",
        "f0acadaa6be97773729e4bb6344061377b9815be7f6be3e797790c5fc5fc0787",
        (Operation(0x1B7DAE, "fstp", "eax", 0x390),),
    ),
    0x259DF0: Contract(
        83,
        "901271c56f0b3b93126117ee1c2a88b2ccde9328ab7f8b550156fdc91a127251",
        "06dfd91901325deee3423d95f3fcfabc5b01e2b94ed705b9bce6ed8c1a01ac38",
        (Operation(0x259E27, "fstp", "ecx", 0x388),),
    ),
}

LEGACY_MACROS = r"""    #define fp_push(v) do { double _fp_value = (v); \
        g_fp_top = (g_fp_top + 7u) & 7u; \
        g_fp_stack[g_fp_top] = _fp_value; } while (0)
    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)
    #define fp_top() g_fp_stack[g_fp_top]
    #define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]
    #define fp_st1() fp_st(1)
"""
PATTERNS = {
    "fld": re.compile(r"    fp_push\(MEMF\((.+)\)\); /\* fld float \*/"),
    "fmul": re.compile(r"    fp_top\(\) = RECOMP_FP_PC\(fp_top\(\) \* MEMF\((.+)\)\); /\* .* \*/"),
    "fstp": re.compile(r"    MEMF\((.+)\) = \(float\)fp_top\(\); fp_pop\(\); /\* fstp \*/"),
}


def lower_function(va: int, original: bytes, generated_chunk: str) -> str:
    """Return authenticated isolated C; every mismatch is an explicit refusal."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86_const import X86_GRP_FPU, X86_OP_MEM

    contract = CONTRACTS.get(va)
    if contract is None:
        raise ValueError("no predeclared x87 lift contract")
    if (
        len(original) != contract.size
        or hashlib.sha256(original).hexdigest() != contract.original_sha256
    ):
        raise ValueError("original x87 span authentication failed")
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    instructions = tuple(decoder.disasm(original, va))
    if sum(item.size for item in instructions) != len(original):
        raise ValueError("original x87 span did not fully decode")
    actual = []
    for instruction in instructions:
        if X86_GRP_FPU not in instruction.groups:
            continue
        if instruction.mnemonic not in PATTERNS or len(instruction.operands) != 1:
            raise ValueError("unsupported original x87 operation")
        operand = instruction.operands[0]
        if (
            operand.type != X86_OP_MEM
            or operand.size != 4
            or operand.mem.index
            or operand.mem.segment
        ):
            raise ValueError("unsupported original x87 operand")
        actual.append(
            Operation(
                instruction.address,
                instruction.mnemonic,
                instruction.reg_name(operand.mem.base) if operand.mem.base else None,
                operand.mem.disp,
            )
        )
    if tuple(actual) != contract.operations:
        raise ValueError("original x87 operation identity mismatch")
    bodies = re.findall(
        r"^void sub_" + f"{va:08X}" + r"\(void\)\n\{\n.*?^\}\n", generated_chunk, re.M | re.S
    )
    if (
        len(bodies) != 1
        or hashlib.sha256(bodies[0].encode()).hexdigest() != contract.generated_sha256
    ):
        raise ValueError("generated x87 body authentication failed")
    body = bodies[0]
    if body.count(LEGACY_MACROS) != 1:
        raise ValueError("unrecognized legacy x87 macro block")
    body = body.replace(LEGACY_MACROS, "", 1)
    for name in ("fp_push", "fp_pop", "fp_top", "fp_st", "fp_st1"):
        line = f"    #undef {name}\n"
        if body.count(line) != 1:
            raise ValueError("unrecognized legacy x87 macro cleanup")
        body = body.replace(line, "", 1)
    lowered = []
    position = 0
    for line in body.splitlines(keepends=True):
        found = [(name, pattern.fullmatch(line.rstrip("\n"))) for name, pattern in PATTERNS.items()]
        matches = [(name, match) for name, match in found if match is not None]
        if matches:
            name, match = matches[0]
            if position >= len(contract.operations):
                raise ValueError("extra generated x87 operation")
            operation = contract.operations[position]
            if name != operation.mnemonic or match.group(1) != operation.expression:
                raise ValueError("generated x87 operand/order mismatch")
            line = (
                f"    harness_x87_{name}32((uint32_t)({operation.expression})); "
                f"/* original {operation.va:08X} {name} m32 */\n"
            )
            position += 1
        lowered.append(line)
    result = "".join(lowered)
    if position != len(contract.operations) or re.search(
        r"\b(?:fp_\w+|g_fp_\w+|RECOMP_FP_\w+|MEM[FD]|float|double)\b", result
    ):
        raise ValueError("unrepresented legacy x87 expression remains")
    return result
