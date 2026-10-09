# SPDX-License-Identifier: GPL-3.0-or-later
"""Seed quality: is a matched address plausibly a real function start (T343)?

The NAME of a match is never touched here. Quality only decides which `--seeds-out` rows are
handed to `CreateFunctionsAt.java`, because that script makes a function (and Ghidra then
re-derives from it) at every row, and some matches are entry labels or tails inside a larger
function, not starts. Rules, in priority order, with the measured counts in docs/libsig.md:

* `inside_known_function`: strictly inside a body listed in the function table.
* `inside_other_seed`: strictly inside the matched body of another seed.
* `tiny`: matched body of at most `TINY_SEED_BYTES`: a bare `jmp rel32`, which says nothing
  about being a start.
* `fragment`: the code begins by restoring a callee-saved register or `leave`s a frame nobody
  built (an epilogue tail).
* `no_frame_entry`: the first instructions address memory through ebp before anything wrote
  ebp (the body is the middle of a function that had a frame). A `call` counts as writing ebp:
  MSVC's `push imm; push handler; call __SEH_prolog` builds the frame that way.

Addresses in `trusted` (the xdk_surface.c table, independent evidence of an API entry) are
exempt from the three entry-shape rules `tiny`, `fragment` and `no_frame_entry`, which are
heuristics. They are NOT exempt from the two `inside_*` rules, which are geometry.
"""

from __future__ import annotations

from bisect import bisect_left
from collections.abc import Collection, Sequence
from itertools import accumulate

import capstone
from capstone import x86

from tools.libsig.match import Image, Match

CLEAN = "clean"
INSIDE_KNOWN = "inside_known_function"
INSIDE_SEED = "inside_other_seed"
TINY = "tiny"
FRAGMENT = "fragment"
NO_FRAME = "no_frame_entry"
QUALITIES = (CLEAN, INSIDE_KNOWN, INSIDE_SEED, TINY, FRAGMENT, NO_FRAME)

TINY_SEED_BYTES = 5  # a bare `jmp rel32`
ENTRY_WINDOW_BYTES = 24
ENTRY_WINDOW_INSTRUCTIONS = 6
_CALLEE_SAVED = (x86.X86_REG_EBX, x86.X86_REG_ESI, x86.X86_REG_EDI, x86.X86_REG_EBP)

_DISASSEMBLER = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
_DISASSEMBLER.detail = True


def entry_defect(image: Image, address: int, body_len: int) -> str:
    """`fragment`, `no_frame_entry` or `` from the first instructions at `address`."""
    code = image.read(address, min(max(body_len, 1), ENTRY_WINDOW_BYTES))
    if code is None:
        return ""
    pushed: set[int] = set()
    ebp_written = False
    for index, insn in enumerate(_DISASSEMBLER.disasm(code, address)):
        if index >= ENTRY_WINDOW_INSTRUCTIONS:
            break
        first_operand = insn.operands[0] if insn.operands else None
        if insn.id == x86.X86_INS_PUSH and first_operand and first_operand.type == x86.X86_OP_REG:
            pushed.add(first_operand.reg)
        elif insn.id == x86.X86_INS_LEAVE and not ebp_written:
            return FRAGMENT
        elif (
            insn.id == x86.X86_INS_POP
            and first_operand
            and first_operand.type == x86.X86_OP_REG
            and first_operand.reg in _CALLEE_SAVED
            and first_operand.reg not in pushed
        ):
            return FRAGMENT
        for operand in insn.operands:
            if operand.type == x86.X86_OP_MEM and x86.X86_REG_EBP in (
                operand.mem.base,
                operand.mem.index,
            ):
                if not ebp_written:
                    return NO_FRAME
        _, written = insn.regs_access()
        if x86.X86_REG_EBP in written or x86.X86_REG_BP in written:
            ebp_written = True
        if insn.id == x86.X86_INS_CALL:
            ebp_written = True  # `call __SEH_prolog` builds the frame: not a frameless entry
    return ""


def classify_seeds(
    seeds: Sequence[Match],
    bodies: Sequence[tuple[int, int]],
    image: Image,
    trusted: Collection[int] = (),
) -> dict[int, str]:
    """address -> quality for every seed. `bodies` are the table's (entry, last body byte)."""
    ordered = sorted(bodies)
    entries = [entry for entry, _ in ordered]
    reach = list(accumulate((last for _, last in ordered), max))
    spans = [(m.address, m.address + m.body_len) for m in seeds]
    result = {}
    for match in seeds:
        address = match.address
        at = bisect_left(entries, address)  # entries before `address` are entries[:at]
        if at and reach[at - 1] >= address:
            result[address] = INSIDE_KNOWN
        elif any(start < address < end for start, end in spans):
            result[address] = INSIDE_SEED
        elif address in trusted:
            result[address] = CLEAN
        elif match.body_len <= TINY_SEED_BYTES:
            result[address] = TINY
        else:
            result[address] = entry_defect(image, address, match.body_len) or CLEAN
    return result
