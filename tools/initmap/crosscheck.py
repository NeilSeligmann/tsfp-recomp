# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the lifted C's call edges against an independent decode of the XBE's own bytes.

The walk trusts the lifter's reading of each function. That reading could be wrong in two
ways a text parser cannot see: the lifter might have decoded data as code, or skipped code it
could not decode. So each expanded function is decoded again here from the bytes in the image,
by capstone, and the two lists of call targets are compared.

capstone's `disasm` STOPS at the first byte it cannot decode and says nothing. A single call
to it over a range therefore reports a short, clean-looking function. The sweep below decodes
one instruction at a time and, on a failure, steps one byte and counts it, then checks that
decoded bytes plus skipped bytes equal the function's extent. A sweep that does not account for
every byte is reported as incomplete and is never counted as agreeing.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass, field

import capstone
from capstone import x86_const

from tools.initmap.image import Image
from tools.initmap.liftparse import (
    KIND_DIRECT,
    KIND_INDIRECT,
    KIND_MANUAL,
    FunctionIndex,
    parse_function,
)

MAX_INSN_BYTES = 15


@dataclass
class Sweep:
    """What one linear decode of a function's bytes found."""

    start: int
    end: int
    instructions: int = 0
    decoded_bytes: int = 0
    skipped_bytes: int = 0
    #: Targets of `call rel32`, with multiplicity.
    calls: Counter[int] = field(default_factory=Counter)
    #: `call r/m32`: how many, since the target is not known.
    indirect_calls: int = 0
    #: `jmp rel` targets outside the function.
    jumps_out: Counter[int] = field(default_factory=Counter)

    @property
    def complete(self) -> bool:
        """True when every byte of the extent was either decoded or counted as skipped."""
        return self.decoded_bytes + self.skipped_bytes == self.end - self.start


def sweep(image: Image, start: int, end: int) -> Sweep:
    """Decode `[start, end)` one instruction at a time, resyncing past undecodable bytes."""
    result = Sweep(start=start, end=end)
    offset = image.xbe.va_to_offset(start)
    if offset is None or end <= start:
        return result
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    data = image.data[offset : offset + (end - start)]
    position = 0
    while position < len(data):
        va = start + position
        instruction = next(decoder.disasm(data[position : position + MAX_INSN_BYTES], va, 1), None)
        if instruction is None:
            result.skipped_bytes += 1
            position += 1
            continue
        result.instructions += 1
        result.decoded_bytes += instruction.size
        position += instruction.size
        operands = instruction.operands
        is_call = instruction.group(capstone.CS_GRP_CALL)
        is_jump = instruction.group(capstone.CS_GRP_JUMP)
        if not operands:
            continue
        immediate = operands[0].type == x86_const.X86_OP_IMM
        if is_call and immediate:
            result.calls[operands[0].imm & 0xFFFFFFFF] += 1
        elif is_call:
            result.indirect_calls += 1
        elif is_jump and immediate:
            target = operands[0].imm & 0xFFFFFFFF
            if not start <= target < end:
                result.jumps_out[target] += 1
    return result


@dataclass
class Verdict:
    """One function's two views, side by side."""

    entry: int
    lifted_calls: Counter[int]
    lifted_indirect: int
    swept: Sweep
    header_instructions: int | None

    @property
    def agrees(self) -> bool:
        return (
            self.swept.complete
            and self.swept.calls == self.lifted_calls
            and self.swept.indirect_calls == self.lifted_indirect
        )

    @property
    def instruction_count_matches(self) -> bool | None:
        if self.header_instructions is None:
            return None
        return self.swept.instructions == self.header_instructions


def check_function(index: FunctionIndex, image: Image, entry: int) -> Verdict:
    """Compare the lifted calls of one function with a fresh decode of its bytes."""
    function = parse_function(entry, index.body(entry))
    lifted: Counter[int] = Counter()
    indirect = 0
    for node in function.nodes:
        for event in node.events:
            if event.kind in (KIND_DIRECT, KIND_MANUAL) and event.target is not None:
                lifted[event.target] += 1
            elif event.kind == KIND_INDIRECT:
                indirect += 1
    end, header_instructions = index.extents[entry]
    return Verdict(
        entry=entry,
        lifted_calls=lifted,
        lifted_indirect=indirect,
        swept=sweep(image, entry, end),
        header_instructions=header_instructions,
    )


@dataclass
class Summary:
    checked: int
    agree: int
    incomplete: int
    count_matches: int
    count_checked: int
    disagreeing: list[Verdict]
    #: Functions where the sweep had to step over at least one undecodable byte.
    with_skipped_bytes: int
    calls_lifted: int
    calls_swept: int
    #: Functions the byte decode found at least one call in, and how many of those agree. A
    #: function with no calls agrees trivially, so the second figure is the fair one.
    with_calls: int = 0
    with_calls_agree: int = 0


def cross_check(index: FunctionIndex, image: Image, entries: list[int]) -> Summary:
    """Check every function in `entries` and summarise."""
    verdicts = [check_function(index, image, entry) for entry in entries]
    comparable = [v for v in verdicts if v.instruction_count_matches is not None]
    return Summary(
        checked=len(verdicts),
        agree=sum(1 for v in verdicts if v.agrees),
        incomplete=sum(1 for v in verdicts if not v.swept.complete),
        count_matches=sum(1 for v in comparable if v.instruction_count_matches),
        count_checked=len(comparable),
        disagreeing=[v for v in verdicts if not v.agrees],
        with_skipped_bytes=sum(1 for v in verdicts if v.swept.skipped_bytes),
        calls_lifted=sum(sum(v.lifted_calls.values()) + v.lifted_indirect for v in verdicts),
        calls_swept=sum(sum(v.swept.calls.values()) + v.swept.indirect_calls for v in verdicts),
        with_calls=sum(1 for v in verdicts if v.swept.calls or v.swept.indirect_calls),
        with_calls_agree=sum(
            1 for v in verdicts if (v.swept.calls or v.swept.indirect_calls) and v.agrees
        ),
    )
