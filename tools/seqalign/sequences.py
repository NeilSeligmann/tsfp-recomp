# SPDX-License-Identifier: GPL-3.0-or-later
"""Ordered `call rel32` targets of an x86-32 function, decoded accurately.

WHY NOT A BYTE SCAN. `tools/codediff/boundaries.py::count_call_sites` sweeps for the
byte 0xE8 without decoding, and documents that its counts are upper bounds because
0xE8 occurs inside other instructions and inside data. That is tolerable when the
output is a ranking over the whole image. It is *not* tolerable here: this module's
output is an ordered sequence, and one phantom call inserted into the middle of a
dispatcher's sequence shifts every later element by one position, which an alignment
then has to pay for with two gaps. Sequence work needs real instruction lengths, so
this decodes with capstone, exactly as `tools/codediff/normalise.py` does.

WHAT COUNTS AS A CALL. Only `call` with a single immediate operand, i.e. `call rel32`
and `call rel16`. Indirect calls (`call dword ptr [...]`, `call eax`) are deliberately
excluded, and their exclusion is recorded in `indirect_calls` rather than silently
dropped: a dispatcher that reaches its subsystems through a function-pointer table
would show up as a function with many indirect calls and no direct ones, and that is a
finding about why the technique cannot work rather than an empty result.

WHAT THE SEQUENCE IS INDEXED BY. Address order within the function body, which is not
necessarily *execution* order -- a compiler may hoist a cold branch, and MSVC's
`.text` ordering of basic blocks within a function is not guaranteed. The donor side
of the alignment is link order, which is also an address order, so address order is
the consistent choice. Where the two disagree the alignment pays for it; there is no
way to recover true execution order from a linear decode, and pretending otherwise
would be the kind of unfalsifiable step this package is built to avoid.

BODIES CAN BE NON-CONTIGUOUS. `tools/ghidra/ExportFunctionBounds.java` emits both
`size_bytes` (an address count) and `body_max_va` precisely so a consumer can tell.
This module decodes the half-open span `[entry_va, body_max_va + 1)` and reports
`fragmented` when that span is larger than `size_bytes`, because in that case some of
what was decoded belongs to another function and the sequence is not trustworthy.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING

import capstone
from capstone import x86 as capstone_x86

from tools.codediff.boundaries import Function

if TYPE_CHECKING:
    from collections.abc import Iterable, Sequence

#: Mnemonic capstone gives a near or far call.
CALL_MNEMONIC = "call"

#: A `jmp` with a single immediate operand, which is how MSVC emits an import thunk
#: and a tail call. Followed through by `resolve_thunks`.
JUMP_MNEMONIC = "jmp"


@dataclass(frozen=True)
class CallSequence:
    """The ordered direct-call targets of one target-image function.

    `targets` is in ascending address order of the *call site*, so index 0 is the
    earliest call in the function body. Duplicates are kept: a dispatcher that calls
    the same helper twice really does call it twice, and removing the repeat would
    silently shorten the sequence the alignment is scored against.
    """

    entry_va: int
    name: str
    size_bytes: int
    targets: tuple[int, ...]
    """Direct-call targets, in call-site address order."""

    indirect_calls: int
    """Calls through a register or memory operand, which have no static target."""

    decoded_bytes: int
    """Bytes capstone successfully decoded. Short of the span means it gave up."""

    span_bytes: int
    """`body_max_va + 1 - entry_va`, the span this sequence was decoded over."""

    fragmented: bool
    """`span_bytes > size_bytes`, i.e. the body is non-contiguous in Ghidra's view."""

    @property
    def distinct_targets(self) -> int:
        return len(set(self.targets))

    @property
    def length(self) -> int:
        return len(self.targets)


def _decoder() -> capstone.Cs:
    """A detail-enabled 32-bit x86 decoder.

    Detail is required because the operand *type* is what distinguishes `call rel32`
    from `call [mem]`, and the text of `op_str` is not a safe thing to pattern-match.
    """
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    return decoder


def _immediate_target(instruction: capstone.CsInsn) -> int | None:
    """The branch target of an instruction with exactly one immediate operand."""
    operands = instruction.operands
    if len(operands) != 1:
        return None
    operand = operands[0]
    if operand.type != capstone_x86.X86_OP_IMM:
        return None
    return int(operand.imm)


def extract_call_sequence(
    text: bytes,
    base_va: int,
    function: Function,
    *,
    decoder: capstone.Cs | None = None,
) -> CallSequence:
    """Decode one function's ordered direct-call targets.

    `base_va` is the virtual address of `text[0]`, matching the convention in
    `tools/codediff/boundaries.py`. A function whose span falls outside `text` is
    returned with an empty sequence rather than raising, because a function table and
    a section come from two different tools and a disagreement between them is a
    normal condition to report, not a crash.
    """
    machine = decoder if decoder is not None else _decoder()
    start = function.entry_va - base_va
    span = function.body_max_va + 1 - function.entry_va
    if span <= 0 or start < 0 or start >= len(text):
        return CallSequence(
            entry_va=function.entry_va,
            name=function.name,
            size_bytes=function.size_bytes,
            targets=(),
            indirect_calls=0,
            decoded_bytes=0,
            span_bytes=max(span, 0),
            fragmented=span > function.size_bytes,
        )
    stop = min(start + span, len(text))
    body = text[start:stop]

    targets: list[int] = []
    indirect = 0
    decoded = 0
    for instruction in machine.disasm(body, function.entry_va):
        decoded += instruction.size
        if instruction.mnemonic != CALL_MNEMONIC:
            continue
        target = _immediate_target(instruction)
        if target is None:
            indirect += 1
            continue
        targets.append(target)

    return CallSequence(
        entry_va=function.entry_va,
        name=function.name,
        size_bytes=function.size_bytes,
        targets=tuple(targets),
        indirect_calls=indirect,
        decoded_bytes=decoded,
        span_bytes=span,
        fragmented=span > function.size_bytes,
    )


def extract_call_sequences(
    text: bytes,
    base_va: int,
    functions: Iterable[Function],
) -> dict[int, CallSequence]:
    """`entry_va -> CallSequence` for every function, sharing one decoder.

    One decoder is reused across ~12,000 functions because constructing a `Cs` is not
    free and the object carries no per-call state that matters here.
    """
    machine = _decoder()
    return {
        function.entry_va: extract_call_sequence(text, base_va, function, decoder=machine)
        for function in functions
    }


@dataclass
class ThunkMap:
    """`thunk entry_va -> ultimate target`, for following `jmp rel32` stubs."""

    targets: dict[int, int] = field(default_factory=dict)

    def resolve(self, va: int) -> int:
        """Follow thunks to a non-thunk, or return `va` unchanged.

        Cycles terminate: the walk is bounded by the number of known thunks, which is
        finite, and a repeated address stops it. A self-jumping stub therefore
        resolves to itself rather than hanging.
        """
        seen: set[int] = set()
        current = va
        while current in self.targets and current not in seen:
            seen.add(current)
            current = self.targets[current]
        return current


def build_thunk_map(
    text: bytes,
    base_va: int,
    functions: Iterable[Function],
) -> ThunkMap:
    """Map every single-`jmp` function to what it jumps to.

    WHY THIS MATTERS TO AN ALIGNMENT. A retail build is full of one-instruction
    `jmp` stubs -- 86 functions in the retail table are already named `thunk_FUN_*`.
    If a dispatcher calls a subsystem through a thunk, the raw sequence records the
    thunk's address, and two different suffix groups routed through two different
    thunks to the same real function would look like disagreement when they agree.
    Resolving first removes that failure mode.

    Only a function whose *entire* decoded body is one immediate `jmp` is treated as a
    thunk. A longer function ending in a tail jump is real code and is left alone.
    """
    machine = _decoder()
    mapping: dict[int, int] = {}
    for function in functions:
        start = function.entry_va - base_va
        span = function.body_max_va + 1 - function.entry_va
        if start < 0 or span <= 0 or start >= len(text):
            continue
        body = text[start : min(start + span, len(text))]
        instructions = list(machine.disasm(body, function.entry_va))
        if len(instructions) != 1:
            continue
        only = instructions[0]
        if only.mnemonic != JUMP_MNEMONIC:
            continue
        target = _immediate_target(only)
        if target is not None and target != function.entry_va:
            mapping[function.entry_va] = target
    return ThunkMap(targets=mapping)


def resolve_sequence(sequence: CallSequence, thunks: ThunkMap) -> CallSequence:
    """`sequence` with every target pushed through `thunks`."""
    return CallSequence(
        entry_va=sequence.entry_va,
        name=sequence.name,
        size_bytes=sequence.size_bytes,
        targets=tuple(thunks.resolve(target) for target in sequence.targets),
        indirect_calls=sequence.indirect_calls,
        decoded_bytes=sequence.decoded_bytes,
        span_bytes=sequence.span_bytes,
        fragmented=sequence.fragmented,
    )


def global_fan_in(sequences: Iterable[CallSequence]) -> dict[int, int]:
    """`target_va -> how many distinct functions call it`.

    Distinct *callers*, not call sites, because a lifecycle function is characterised
    by being reached from one dispatcher rather than by being called once. A helper
    called three times from one dispatcher still has fan-in 1 and is still a plausible
    subsystem entry point; a utility called from forty functions is not.
    """
    callers: dict[int, set[int]] = {}
    for sequence in sequences:
        for target in set(sequence.targets):
            callers.setdefault(target, set()).add(sequence.entry_va)
    return {target: len(owners) for target, owners in callers.items()}


def first_occurrences(targets: Sequence[int]) -> tuple[int, ...]:
    """`targets` with repeats after the first removed, order preserved.

    Order comparisons need each subsystem to appear once: a helper called three times
    contributes three copies of the same address, and every ordered pair among them
    compares equal, which inflates an order-concordance denominator with pairs that can
    neither agree nor disagree. The *first* call is kept because that is the one a
    dispatcher's sequence position refers to.
    """
    seen: set[int] = set()
    ordered: list[int] = []
    for target in targets:
        if target in seen:
            continue
        seen.add(target)
        ordered.append(target)
    return tuple(ordered)


def sizes_by_entry(functions: Sequence[Function]) -> dict[int, int]:
    """`entry_va -> size_bytes`, the size signal the scoring model uses."""
    return {function.entry_va: function.size_bytes for function in functions}
