# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic initial state, so a reported divergence is reproducible from its row.

Every case is derived from `(seed, index)` and NOTHING else -- not from its position in
a shuffle, not from a shared generator that has been advanced by earlier cases. That
independence is the whole point: given a CSV row saying `seed=20261001 index=4071`, the
exact failing input can be rebuilt without rerunning the thousands of cases before it.

The seed is stretched with SHA-256 rather than by seeding `random.Random` with a tuple.
Python randomises `hash()` for `str` and `bytes` per process, and anything that reaches
for `hash()` on a composite is one refactor away from becoming irreproducible across
runs. A hash chosen for being stable is worth the two extra lines.

Inputs are deliberately POINTER-SHAPED most of the time. Uniformly random 32-bit values
would make almost every `mov edx, [ecx+0x34]` fault on both sides, and a case where both
sides fault yields no verdict -- the harness would report thousands of cases and verify
almost nothing. Aiming registers at a mapped scratch arena is what makes dereferencing
code actually get exercised.

WHERE THE NO-VERDICT RATE ACTUALLY COMES FROM
---------------------------------------------
41% of cases in the standing run reached no verdict, and the obvious culprit turned out
not to be the main one. Measured over the full case set, per lever:

* **Stack arguments, 0.5 -> 1.0 pointer-shaped: 51.7% -> 63.0% clean.** The dominant
  lever by an order of magnitude. Half of the sixteen argument dwords were uniform random,
  so any function that read an argument and dereferenced it faulted on about half its
  cases. This is now `frame_arg_pointer_share`.
* **Register pointer share, 0.65 -> 0.85: +2.5 points.** Now the default.
* **Pre-filling the scratch arena with self-referential pointers: +0.2 points.** The
  hypothesis that pointer chasing through a zero-filled arena dominated was WRONG as a
  matter of magnitude -- it is real (40% of faulting addresses are below 0x1000) but small,
  because most of those chains fault on their second dereference either way. Kept because
  it is free, not because it mattered.
* **Removing the small-integer bucket: no effect, slightly negative.** A small integer
  dereferenced as a pointer faults exactly as reliably as a uniform random one.

Raising the clean rate also raises COUNT-LIMIT (388 -> 1,108), because more of each
function actually runs: mean executed instructions per clean case went from 203 to 385.
Those cases still reach no verdict, so the gain is reported as VERDICTS, never as cases.
"""

from __future__ import annotations

import hashlib
from collections.abc import Iterator
from dataclasses import dataclass, replace
from itertools import product
from random import Random

from .model import REG_NAMES, Case

#: Guest window the harness maps, identity. Matches `tools/harness/driver.c`.
GUEST_LO = 0x00010000
GUEST_HI = 0x01000000
GUEST_SPAN = GUEST_HI - GUEST_LO

#: Where generated stacks live, well clear of the loaded image.
STACK_BASE = 0x00E80000
#: The fake return address pushed below the frame. Execution reaching it is how the
#: oracle recognises a normal return; it is deliberately outside any code section.
SENTINEL = 0x00FF0000
#: A mapped arena registers are aimed at, so pointer dereferences land somewhere real.
SCRATCH_BASE = 0x00D00000
SCRATCH_WORDS = 0x8000
SCRATCH_FILL_BYTES = 0x400

#: Kernel import thunk slots the harness resolves, as `{slot VA: value dword}`. The XBE holds a
#: raw ordinal in each slot (`0x475868` = `0x8000009c`, KeTickCount) and the loader patches it
#: to an export address at run time, so a body that dereferences the slot would fault on every
#: case. Each listed slot is pointed at its own harness-owned dword just past the scratch
#: arena, holding the given known value. Baked into the shared image, so oracle and subject see
#: identical bytes.
THUNK_TARGET_BASE = SCRATCH_BASE + SCRATCH_WORDS * 4
SEEDED_THUNK_SLOTS: dict[int, int] = {0x00475868: 0x0001E240}

#: The fs segment (T469). Both sides run with fs based at `KPCR_BASE`: the oracle writes the
#: FS_BASE MSR and the subject driver sets `g_fs_base`, so `fs:[x]` reads the same dword on
#: each. The page sits past the thunk targets, inside the guest window and outside the
#: image, so it is part of the one shared image. Address 4 (an unseeded fs base of 0) is below
#: the window and faults on both sides, which is how a case with a nulled `fs:[4]` is judged.
#:   fs:[4]        -> TLS pointer array (`TLS_TABLE_BASE`), one dword per slot index
#:   array[i]      -> a TLS block (`TLS_BLOCK_BASE + 0x40 * i`), dword +4 of it holds the value
#:   [0x771368]    -> the TLS slot index the CRT allocated, seeded to `TLS_SLOT_INDEX`
KPCR_BASE = 0x00D40000
TLS_TABLE_BASE = 0x00D41000
TLS_BLOCK_BASE = 0x00D42000
TLS_BLOCK_STRIDE = 0x40
TLS_SLOT_COUNT = 8
TLS_SLOT_INDEX = 2
TLS_INDEX_ADDRESS = 0x00771368
KPCR_BYTE_24 = 0x24
KPCR_DWORD_28 = 0x28


def tls_value(index: int) -> int:
    """The dword stored at offset 4 of TLS block `index`."""
    return 0x4C000000 + index * 0x111


def seeded_guest_dwords() -> dict[int, int]:
    """`{address: dword}` written into the shared image for the fs segment and TLS slots."""
    dwords: dict[int, int] = {
        KPCR_BASE + 4: TLS_TABLE_BASE,
        KPCR_BASE + KPCR_DWORD_28: 0x00C0FFEE,
        TLS_INDEX_ADDRESS: TLS_SLOT_INDEX,
    }
    for index in range(TLS_SLOT_COUNT):
        block = TLS_BLOCK_BASE + TLS_BLOCK_STRIDE * index
        dwords[TLS_TABLE_BASE + 4 * index] = block
        dwords[block] = 0xB10C0000 + index
        dwords[block + 4] = tls_value(index)
    return dwords


#: Stack-argument dwords written above the return address.
FRAME_ARGS = 16

_POINTER_SHARE = 0.65
#: Share of registers given a small integer 0..255. The remainder after the pointer and
#: small-int shares is uniform 32-bit. Kept as its own knob rather than derived, so that
#: the historical distribution (65% pointer, 20% small, 15% uniform) is reproducible
#: exactly -- otherwise a before/after verdict comparison would be measuring a silently
#: different input distribution as well as the change under test.
_SMALL_INT_SHARE = 0.20

#: Every Nth dword of the pre-filled arena is left zero, so that a pointer chase
#: TERMINATES. An arena of nothing but valid pointers removes the unmapped faults and
#: replaces them with runaway loops, which reach no verdict either -- measured as
#: UNMAPPED down and COUNT-LIMIT up. A terminator every 16 dwords keeps chains finite.
SCRATCH_TERMINATOR_EVERY = 16

#: Spread of the back-step a chain takes. Each dword points at a STRICTLY LOWER-indexed
#: dword, which is what makes termination structural rather than incidental: a chase cannot
#: do anything but descend, so it reaches index 0 in at most as many steps as its starting
#: index even if it never lands on a terminator.
#:
#: An earlier version pointed each dword at `(i * 7 + 1) mod SCRATCH_WORDS` and relied on
#: the orbit eventually hitting a multiple of 16. It does not: that map has cycles mod 16
#: that never reach 0 -- a chase starting at dword 5 runs 5, 4, 13, 12, 5 forever. The test
#: `test_a_pointer_chase_through_the_arena_terminates` catches exactly that.
SCRATCH_STRIDE = 7


@dataclass(frozen=True)
class SeedPolicy:
    """Knobs for the input distribution, so a run can record how it generated inputs."""

    stack_base: int = STACK_BASE
    scratch_base: int = SCRATCH_BASE
    sentinel: int = SENTINEL
    frame_args: int = FRAME_ARGS
    scratch_fill_bytes: int = SCRATCH_FILL_BYTES
    #: Share of registers aimed into the scratch arena. The rest are small integers and
    #: uniform 32-bit values, which almost always fault when dereferenced -- measured as
    #: 51% of all oracle faults landing above the guest window.
    pointer_share: float = 0.85
    small_int_share: float = 0.10
    #: Share of stack arguments made pointer-shaped.
    #:
    #: This is the single biggest lever on the no-verdict rate, and it was not the obvious
    #: one. Half of these used to be uniform random 32-bit values, so any function that
    #: read an argument and dereferenced it faulted on about half its cases. Measured over
    #: the full 50,752-case sweep: raising it from 0.5 to 1.0 moved the clean rate from
    #: 51.7% to 63.0%, against 0.2 points for pre-filling the scratch arena. Kept below 1.0
    #: so that integer-valued arguments still occur -- a function branching on a small
    #: count is not exercised by a pointer.
    frame_arg_pointer_share: float = 0.85
    #: Choose each argument's shape by POSITION rather than by a random draw, as the
    #: original did. Restores the historical case stream exactly: a random choice consumes
    #: an extra random number per argument, which shifts every later value in the case and
    #: makes the old figures unreproducible. It also pins argument 0 to pointer-shaped,
    #: which is why the old fixed pattern beat a random half-share.
    alternate_frame_args: bool = False
    #: Fill the whole arena with aligned pointers back into itself, so that a chain of
    #: dereferences stays mapped instead of loading zero and faulting near address 0.
    #: Lives in the shared IMAGE rather than in a per-case patch: patching 128 KB per
    #: case would put a quarter of a megabyte of hex on the wire every case.
    self_referential_arena: bool = True
    #: Share of register slots (and stack-argument dwords) OVERRIDDEN with an
    #: add/sub overflow or partial-register boundary value after the base draw.
    #: 0.0 by default, and 0.0 consumes NO extra randomness, so the default case
    #: stream is bit-for-bit what it was before these knobs existed. Nonzero only
    #: in the flags-adversarial policy `--compare-eflags` selects: random
    #: pointer-shaped inputs essentially never produce 0x7FFFFFFF or 0xFFFFFFFF,
    #: which is exactly where the known flag-model gaps live.
    boundary_share: float = 0.0
    #: Share of cases whose ECX is overridden with a shift count on the masked
    #: 0/1/31/32/33 boundary. ECX specifically because every variable-count
    #: shift and rotate takes its count from CL.
    shift_count_ecx_share: float = 0.0

    def arena_dword(self, index: int) -> int:
        """The value pre-filled at arena dword `index`.

        Zero every `SCRATCH_TERMINATOR_EVERY` dwords so a pointer chase ends. Both sides
        read this out of the same image, so it is identical by construction rather than by
        two implementations agreeing.
        """
        if not self.self_referential_arena:
            return 0
        if index % SCRATCH_TERMINATOR_EVERY == 0:
            return 0
        # Strictly lower-indexed, so a chase can only descend and must terminate.
        return self.scratch_base + 4 * max(0, index - 1 - (index % SCRATCH_STRIDE))


#: The input distribution as it stood before this widening, so the previous figures can be
#: RE-MEASURED on the current tree instead of only quoted from an older one.
#:
#: `alternate_frame_args` is what makes this an EXACT replay rather than an approximation:
#: choosing each argument's shape by a random draw consumes one extra random number per
#: argument, which shifts every later value in the case, so the same share values alone
#: would not reproduce the old stream.
#:
#: That alternation also pinned argument 0 to pointer-shaped, and argument 0 is the one most
#: functions dereference, which made the old fixed pattern considerably stronger than a
#: random half-share. Measuring the change against a random-0.5 share instead of against
#: this would have flattered it by about seven points of clean rate.
LEGACY_SEEDING = SeedPolicy(
    pointer_share=_POINTER_SHARE,
    small_int_share=_SMALL_INT_SHARE,
    frame_arg_pointer_share=0.5,
    self_referential_arena=False,
    alternate_frame_args=True,
)

#: Values on the add/sub overflow and 8/16-bit partial-register boundaries. A
#: value here forced into an operand makes CF/OF/SF flip exactly where the
#: flag model's reconstruction must be width- and wrap-exact.
BOUNDARY_VALUES: tuple[int, ...] = (
    0,
    1,
    0x7F,
    0x80,
    0xFF,
    0x7FFF,
    0x8000,
    0xFFFF,
    0x7FFFFFFF,
    0x80000000,
    0x80000001,
    0xFFFFFFFE,
    0xFFFFFFFF,
)

#: Shift and rotate counts around the hardware's count-masking boundary: x86
#: masks a 32-bit shift count to 5 bits, so 32 means "no shift, flags
#: preserved" and 33 means "shift by 1" -- the exact semantics a lifter is
#: likeliest to get wrong.
SHIFT_COUNTS: tuple[int, ...] = (0, 1, 31, 32, 33)

#: The input distribution the EFLAGS comparison runs under (`--compare-eflags`).
#: Identical to the default except that register and stack-argument slots are
#: overridden with boundary values at these shares. Deliberately NOT the
#: default: boundary integers dereference as faults, so they cost clean-rate,
#: and they earn that cost only when flags are actually being compared.
FLAGS_ADVERSARIAL_SEEDING = SeedPolicy(
    boundary_share=0.35,
    shift_count_ecx_share=0.5,
)


def scratch_arena_bytes(policy: SeedPolicy | None = None) -> bytes:
    """The arena's initial contents, for `build_guest_image` to bake into the image."""
    policy = policy or SeedPolicy()
    out = bytearray(SCRATCH_WORDS * 4)
    for index in range(SCRATCH_WORDS):
        out[index * 4 : index * 4 + 4] = policy.arena_dword(index).to_bytes(4, "little")
    return bytes(out)


def derive_seed(seed: int, index: int) -> int:
    """A stable 64-bit stretch of `(seed, index)`, identical across processes and runs."""
    digest = hashlib.sha256(f"{seed}:{index}".encode()).digest()
    return int.from_bytes(digest[:8], "big")


def make_case(
    seed: int,
    index: int,
    va: int,
    size: int,
    *,
    policy: SeedPolicy | None = None,
) -> Case:
    """Build case `index` of stream `seed` for one function. Pure, and order-independent.

    Calling this with the same arguments always yields an identical `Case`, whether it
    is the first case built in a process or the ten-thousandth.
    """
    policy = policy or SeedPolicy()
    rng = Random(derive_seed(seed, index))

    esp = policy.stack_base + rng.randrange(0, 0x800) * 16
    regs: list[int] = []
    for _ in range(8):
        roll = rng.random()
        if roll < policy.pointer_share:
            regs.append(policy.scratch_base + rng.randrange(0, SCRATCH_WORDS) * 4)
        elif roll < policy.pointer_share + policy.small_int_share:
            regs.append(rng.randrange(0, 1 << 32) & 0xFF)
        else:
            regs.append(rng.randrange(0, 1 << 32))
    # Boundary overrides, applied AFTER the base draw so that a zero share
    # consumes no randomness at all -- the default stream must stay bit-for-bit
    # reproducible, or every recorded (seed, index) stops naming its case.
    if policy.boundary_share:
        for slot in range(8):
            if slot in (4, 5):  # esp/ebp are overwritten below regardless
                continue
            if rng.random() < policy.boundary_share:
                regs[slot] = BOUNDARY_VALUES[rng.randrange(len(BOUNDARY_VALUES))]
    if policy.shift_count_ecx_share and rng.random() < policy.shift_count_ecx_share:
        regs[1] = SHIFT_COUNTS[rng.randrange(len(SHIFT_COUNTS))]
    regs[4] = esp
    regs[5] = policy.stack_base + 0x8000

    frame = bytearray(policy.sentinel.to_bytes(4, "little"))
    for arg in range(policy.frame_args):
        # A function that reads an argument and dereferences it faults unless that
        # argument is pointer-shaped, and half of these used to be uniform random.
        if policy.alternate_frame_args:
            pointer_shaped = arg % 2 == 0
        else:
            pointer_shaped = rng.random() < policy.frame_arg_pointer_share
        if pointer_shaped:
            value = policy.scratch_base + rng.randrange(0, SCRATCH_WORDS) * 4
        else:
            value = rng.randrange(0, 1 << 32)
        frame += value.to_bytes(4, "little")

    if policy.boundary_share:
        for arg in range(policy.frame_args):
            if rng.random() < policy.boundary_share:
                forced = BOUNDARY_VALUES[rng.randrange(len(BOUNDARY_VALUES))]
                frame[4 + 4 * arg : 8 + 4 * arg] = forced.to_bytes(4, "little")

    scratch = bytes(rng.randrange(0, 256) for _ in range(policy.scratch_fill_bytes))
    patches = ((esp, bytes(frame)), (policy.scratch_base, scratch))
    if va in (0x22020, 0x22030):
        # Independently seed counter and adjacent bytes; no timing/callback delivery.
        patches += ((0x563914, bytes(rng.randrange(256) for _ in range(12))),)
    return Case(
        seed=seed,
        index=index,
        va=va,
        size=size,
        regs=tuple(regs),
        df=0,
        patches=patches,
    )


def generate_cases(
    seed: int,
    functions: list[tuple[int, int]],
    cases_per_function: int,
    *,
    policy: SeedPolicy | None = None,
    start_index: int = 0,
) -> Iterator[Case]:
    """Yield `cases_per_function` cases for each `(va, size)`, with contiguous indices.

    Indices are assigned in a single ascending sweep so that `(seed, index)` names
    exactly one case for a given function list, and resuming a checkpointed run is a
    matter of skipping to an index rather than replaying a generator.
    """
    index = start_index
    for va, size in functions:
        for _ in range(cases_per_function):
            yield make_case(seed, index, va, size, policy=policy)
            index += 1


# ---------------------------------------------------------------------------
# Edge cases for hand-written replacements.
#
# Random inputs are pointer-shaped on purpose (see the module docstring), which is exactly
# wrong for a function that does integer arithmetic on an argument: it never sees 0, the
# signed-overflow boundary or all-ones. A replacement is judged on the SAME random cases
# plus these, which sweep each argument slot over the boundary values of a 32-bit word.
# They are generated only for registered replacements, whose argument count the registry
# states, because a lifted function's argument count is not known.
# ---------------------------------------------------------------------------

#: 0 and its neighbours, the signed boundary, and all-ones.
EDGE_VALUES: tuple[int, ...] = (
    0,
    1,
    2,
    0x7FFFFFFF,
    0x80000000,
    0x80000001,
    0xFFFFFFFE,
    0xFFFFFFFF,
)
#: Largest number of edge cases per function. Full cross products are used while they fit
#: (up to three argument slots, 9**3 = 729), then one slot at a time.
MAX_EDGE_CASES = 1024
#: Edge case ordinals live far above any random case index so the two streams cannot share
#: an `(seed, index)` pair: `index = EDGE_INDEX_BASE + (va << 11) + ordinal`.
EDGE_INDEX_BASE = 1 << 40
EDGE_ORDINAL_BITS = 11
#: Register slot positions in `Case.regs` (eax, ecx, edx, ...).
ECX_SLOT = 1
EDX_SLOT = 2

#: Selector 0 means "keep the value the random case already put in this slot".
_KEEP = 0


def edge_values(va: int | None = None) -> tuple[int, ...]:
    """Measured selector literals supplement integer boundaries for this mapper."""
    return EDGE_VALUES + (54, 55, 230) if va == 0x000B3D60 else EDGE_VALUES


def edge_selectors(slot_count: int, va: int | None = None) -> list[tuple[int, ...]]:
    """Which candidate each argument slot takes, one tuple per edge case.

    Selector 0 keeps the random value (a pointer into the scratch arena, so functions that
    dereference an argument still run), positive selectors index `edge_values(va)`. The all-keep
    tuple is omitted: it is an ordinary random case. The two counter replacements
    additionally select the counter global through a final virtual state slot.
    """
    if va in (0x22020, 0x22030):
        slot_count += 1  # final selector is the observed counter global
    if slot_count <= 0:
        return []
    values = edge_values(va)
    choices = range(len(values) + 1)
    if (len(values) + 1) ** slot_count <= MAX_EDGE_CASES:
        selectors = [t for t in product(choices, repeat=slot_count) if any(t)]
    else:
        selectors = []
        for slot in range(slot_count):
            for choice in range(1, len(values) + 1):
                selector = [_KEEP] * slot_count
                selector[slot] = choice
                selectors.append(tuple(selector))
        for choice in range(1, len(values) + 1):
            selectors.append((choice,) * slot_count)
    return selectors


def argument_register_slots(count: int, names: tuple[str, ...] | None = None) -> tuple[int, ...]:
    if type(count) is not int or not 0 <= count <= 2:
        raise ValueError("at most two register arguments exist")
    names = ("ecx", "edx")[:count] if names is None else names
    if names not in ((), ("ecx",), ("esi",), ("ecx", "edx"), ("ecx", "esi")) or len(names) != count:
        raise ValueError("invalid register argument names/count")
    return tuple(REG_NAMES.index(name) for name in names)


def make_edge_case(
    seed: int,
    ordinal: int,
    va: int,
    size: int,
    *,
    register_args: int,
    stack_args: int,
    register_names: tuple[str, ...] | None = None,
    selector: tuple[int, ...],
    policy: SeedPolicy | None = None,
) -> Case:
    """One edge case: a random case with chosen argument slots forced to edge values.

    `register_args` is 0 for cdecl and stdcall, 1 for thiscall, 2 for fastcall, and those
    slots are ecx then edx. `stack_args` follow, at `[esp+4]` upward. A function's
    non-argument state (the other registers, the scratch arena) comes from the random
    case, so an edge case differs from a random one only where the selector says so.
    """
    counter_selector = va in (0x22020, 0x22030)
    if len(selector) != register_args + stack_args + int(counter_selector):
        raise ValueError("selector length must equal argument slots plus any counter state slot")
    register_slots = argument_register_slots(register_args, register_names)
    if ordinal >= 1 << EDGE_ORDINAL_BITS:
        raise ValueError("edge ordinal out of range")
    policy = policy or SeedPolicy()
    index = EDGE_INDEX_BASE + (va << EDGE_ORDINAL_BITS) + ordinal
    case = make_case(seed, index, va, size, policy=policy)
    regs = list(case.regs)
    esp_value, frame = case.patches[0]
    frame_bytes = bytearray(frame)
    for position, choice in enumerate(selector[:-1] if counter_selector else selector):
        if choice == _KEEP:
            continue
        value = edge_values(va)[choice - 1]
        if position < register_args:
            regs[register_slots[position]] = value
        else:
            offset = 4 + 4 * (position - register_args)
            frame_bytes[offset : offset + 4] = value.to_bytes(4, "little")
    patches = ((esp_value, bytes(frame_bytes)), *case.patches[1:])
    if counter_selector and selector[-1] != _KEEP:
        address, state = patches[-1]
        state = bytearray(state)
        state[4:8] = edge_values(va)[selector[-1] - 1].to_bytes(4, "little")
        patches = (*patches[:-1], (address, bytes(state)))
    return replace(case, regs=tuple(regs), patches=patches)
