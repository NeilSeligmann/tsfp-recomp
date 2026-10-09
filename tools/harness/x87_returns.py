# SPDX-License-Identifier: GPL-3.0-or-later
"""Static isolated return/call adaptation; no runtime capability or handoff."""

from __future__ import annotations

from tools.harness.x87_lift import lower_function

RETURNS = {
    0xCB3B0: (0xCB3CD, 0xCB3D4),
    0x1B7D00: (0x1B7D60,),
    0x1B7D70: (0x1B7DD8, 0x1B7DE6),
    0x259DF0: (0x259E42,),
}
# (instruction VA, callee VA, original continuation VA)
CALLS = {
    0xCB3B0: (),
    0x1B7D00: (
        (0x1B7D20, 0xCB3B0, 0x1B7D25),
        (0x1B7D35, 0xCB3B0, 0x1B7D3A),
        (0x1B7D4A, 0xCB3B0, 0x1B7D4F),
    ),
    0x1B7D70: ((0x1B7DA6, 0xCB3B0, 0x1B7DAB), (0x1B7DDB, 0x1B7D00, 0x1B7DE0)),
    0x259DF0: ((0x259E1C, 0xCB3B0, 0x259E21),),
}


def lower_with_returns(va: int, original: bytes, generated_chunk: str) -> str:
    """Authenticate v1 inputs first, then replace only exact declared RET/calls."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86_const import X86_GRP_CALL, X86_GRP_RET, X86_OP_IMM

    result = lower_function(va, original, generated_chunk)
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    decoder.detail = True
    returns, calls = [], []
    for item in decoder.disasm(original, va):
        if X86_GRP_RET in item.groups:
            if item.mnemonic != "ret" or bytes(item.bytes) != b"\xc3":
                raise ValueError("unsupported isolated return")
            returns.append(item.address)
        if X86_GRP_CALL in item.groups:
            if (
                item.mnemonic != "call"
                or item.size != 5
                or item.bytes[0] != 0xE8
                or len(item.operands) != 1
                or item.operands[0].type != X86_OP_IMM
            ):
                raise ValueError("unsupported isolated call")
            calls.append((item.address, item.operands[0].imm, item.address + item.size))
    if tuple(returns) != RETURNS.get(va) or tuple(calls) != CALLS.get(va):
        raise ValueError("original return/call contract mismatch")
    old_return = "    esp += 4; return; /* ret */"
    if result.count(old_return) != len(returns):
        raise ValueError("generated return count mismatch")
    for site in returns:
        result = result.replace(
            old_return,
            f"    harness_x87_return32(&esp, 0u, 0x{site:08X}u); return; /* checked ret */",
            1,
        )
    for site, target, continuation in calls:
        old = (
            f"    PUSH32(esp, 0x{continuation:08X}u); "
            f"RECOMP_ABI_CALL(0x{target:08X}u, sub_{target:08X}); /* call 0x{target:08X} */"
        )
        if result.count(old) != 1:
            raise ValueError("generated call contract mismatch")
        new = (
            f"    PUSH32(esp, 0x{continuation:08X}u);\n"
            f"    harness_x87_return_begin(0x{continuation:08X}u, 0x{site:08X}u);\n"
            f"    RECOMP_ABI_CALL(0x{target:08X}u, sub_{target:08X});\n"
            f"    harness_x87_return_end(0x{continuation:08X}u, 0x{site:08X}u);"
        )
        result = result.replace(old, new, 1)
    if "/* ret */" in result or result.count("RECOMP_ABI_CALL(") != len(calls):
        raise ValueError("unrepresented generated return/call")
    return result
