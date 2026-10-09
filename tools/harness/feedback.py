# SPDX-License-Identifier: GPL-3.0-or-later
"""Feedback cases: reaching the branches random inputs do not.

THE PROBLEM THIS SOLVES
-----------------------
Random registers and a pointer-shaped stack never satisfy a test like `cmp [owner+0xD4],
ecx; je`, because that needs an argument to equal a word read from guest memory, and never
satisfy `cmp state, 0x10` unless memory happens to hold 0x10. The first proof run of a
replacement for exactly such a function covered 82% of its body with 100% of cases agreeing
with a function that does nothing: AGREE, on a path that never ran.

WHAT IT DOES
------------
For a few seed cases per function the oracle records every guest memory load. Each seed
then spawns variants of two kinds, both reproducible from the seed case and nothing else:

* ARGUMENT SUBSTITUTION. One argument slot is replaced by a value the function itself
  loaded, so a comparison of an argument against memory can come out equal.
* MEMORY MUTATION. One loaded location is overwritten, in the initial state both sides
  share, with a value from a small pool: 0, 1, all-ones, and every immediate the function
  body compares or masks against (plus and minus one). A guard on `state == 0x10` or a
  flag bit is then crossed from the memory side.

This is the idea behind comparison-feedback fuzzing, deliberately small. It does not prove
every path is reached: coverage is MEASURED on the original's instructions and reported.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import replace

import capstone

from .model import Case
from .seeding import argument_register_slots

#: How many seed cases per function are replayed with load recording, and how many variants
#: the whole function may spawn. Both keep a function's cost bounded and predictable.
SEED_CASES = 16
MAX_VARIANTS = 384
#: Seeds kept in all, and the number of discriminating cases a function is topped up to by
#: replaying the later seeds, which is how a rarely true predicate gets enough true cases.
MAX_SEEDS = 64
TARGET_DISCRIMINATING = 60
TOP_UP_VARIANTS_PER_SEED = 8
#: Feedback case indices live above the edge-case range, in `(seed, va, ordinal)` space.
FEEDBACK_INDEX_BASE = 1 << 42
#: Distinct loaded values tried per argument slot.
VALUES_PER_SLOT = 6
#: The text section. A load from code is skipped: patching bytes the ORACLE executes would
#: change the original's behaviour, which the hand-written C cannot follow.
TEXT_LO = 0x00012000
TEXT_HI = 0x0046F76C
#: Stack bytes above the entry `esp` that hold the return address and arguments.
FRAME_BYTES = 4 * 17
BASE_POOL = (0, 1, 0xFFFFFFFF)
#: Loaded locations considered for pair mutations, and the value pairs tried on each pair.
PAIR_LOCATIONS = 6
PAIR_VALUES = ((0xFFFFFFFF, 0xFFFFFFFF), (0xFFFFFFFF, 1), (1, 0xFFFFFFFF))
MAX_POOL = 16
ECX_SLOT = 1


def body_immediates(code: bytes, va: int) -> list[int]:
    """Every immediate operand in the function body, in first-seen order, as 32-bit values."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    found: list[int] = []
    for insn in md.disasm(code, va):
        for operand in insn.operands:
            if operand.type == capstone.x86.X86_OP_IMM:
                value = operand.imm & 0xFFFFFFFF
                if value not in found:
                    found.append(value)
    return found


def value_pool(immediates: list[int]) -> tuple[int, ...]:
    """0, 1, -1, then each immediate and its neighbours, de-duplicated and capped."""
    pool: list[int] = list(BASE_POOL)
    for value in immediates:
        for candidate in (value, (value - 1) & 0xFFFFFFFF, (value + 1) & 0xFFFFFFFF):
            if candidate not in pool:
                pool.append(candidate)
    return tuple(pool[:MAX_POOL])


def _slot_setter(
    case: Case,
    register_args: int,
    slot: int,
    value: int,
    register_names: tuple[str, ...] | None = None,
) -> Case:
    regs = list(case.regs)
    esp, frame = case.patches[0]
    if slot < register_args:
        regs[argument_register_slots(register_args, register_names)[slot]] = value
        return replace(case, regs=tuple(regs))
    offset = 4 + 4 * (slot - register_args)
    frame_bytes = bytearray(frame)
    frame_bytes[offset : offset + 4] = value.to_bytes(4, "little")
    return replace(case, patches=((esp, bytes(frame_bytes)), *case.patches[1:]))


def feedback_variants(
    case: Case,
    loads: tuple[tuple[int, int, int], ...],
    pool: tuple[int, ...],
    *,
    register_args: int,
    stack_args: int,
    register_names: tuple[str, ...] | None = None,
    limit: int,
    reject: Callable[[int], bool] | None = None,
) -> list[Case]:
    """Variants of `case` driven by what the oracle loaded while running it.

    Three families, taken round-robin so that none uses the whole budget:

    * SUBSTITUTION: an argument slot takes a value the function loaded.
    * SINGLE MUTATION: one loaded location takes a pool value.
    * PAIR MUTATION: two loaded locations are set together to all-ones or 1. A function
      like `mask &= ~table[index]` changes nothing unless the mask AND the table entry are
      both non-zero, which no single mutation can arrange.

    The order and the cap are fixed, so the same seed case always yields the same variants.
    """
    argument_register_slots(register_args, register_names)
    slots = register_args + stack_args
    if reject is not None:
        pool = tuple(value for value in pool if not reject(value))

    # Latest load first: the words a function loads last are the ones it has just compared
    # or is about to, which is what an argument has to be made equal to.
    loaded_values: list[int] = []
    for _address, size, value in reversed(loads):
        if reject is not None and reject(value):
            continue
        if size == 4 and value not in loaded_values:
            loaded_values.append(value)
    substitutions = [
        _slot_setter(case, register_args, slot, value, register_names)
        for value in loaded_values[:VALUES_PER_SLOT]
        for slot in range(slots)
    ]

    esp = case.esp
    locations: list[tuple[int, int]] = []
    for address, size, _value in loads:
        in_frame = esp <= address < esp + FRAME_BYTES
        in_text = TEXT_LO <= address < TEXT_HI
        if not in_frame and not in_text and (address, size) not in locations:
            locations.append((address, size))

    def patch_for(location: tuple[int, int], value: int) -> tuple[int, bytes]:
        address, size = location
        return (address, (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little"))

    singles = [
        replace(case, patches=(*case.patches, patch_for(location, pool[rank])))
        for rank in range(len(pool))
        for location in locations
    ]
    pairs = []
    for first_index, first in enumerate(locations[:PAIR_LOCATIONS]):
        for second in locations[first_index + 1 : PAIR_LOCATIONS]:
            for first_value, second_value in PAIR_VALUES:
                patches = (patch_for(first, first_value), patch_for(second, second_value))
                pairs.append(replace(case, patches=(*case.patches, *patches)))

    variants: list[Case] = []
    families = [substitutions, singles, pairs]
    position = 0
    while len(variants) < limit and any(position < len(family) for family in families):
        for family in families:
            if position < len(family) and len(variants) < limit:
                variants.append(family[position])
        position += 1
    return variants
