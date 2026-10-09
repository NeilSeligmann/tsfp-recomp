# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure helpers for counting how many register-combiner configurations the title can set.

THE QUESTION. The NV2A pixel pipeline is register combiners, not shaders. The title can
reach the combiner registers by three routes, and whether the SET of configurations is
closed (enumerable from tables and literals) or open (computed from run-time inputs)
depends on what feeds each route:

  1. `SetPixelShader` is handed a PIXELSHADERDEF-shaped block. Either a pointer to a
     static block in initialised data (a precompiled definition) or the output of the
     run-time assembler. `first_argument` classifies the argument at each call site and
     `definition_gaps` and `field_histogram` characterise the static blocks.
  2. The game's own table-driven `SetRenderState` loops walk a table of
     (state index, value) records. `find_array_walks` finds such a walk,
     `read_walk_indices` reads the state indices out of the table, and
     `index_histogram` says how many fall in the combiner class (indices 0 to 56).
  3. A few direct writes of one combiner word through D3D8's own one-header primitive.
     `register_argument` classifies the header and value registers at those sites.

`field_extent` and `copy_blocks` answer a different question about route 1: how big the
definition block is and how it is spread over hardware methods, read from the one
function that consumes it.

EVERYTHING HERE IS A FUNCTION OF DECODED INSTRUCTIONS AND AN `Image`. No address from
the title is written in this module. The addresses are inputs, so a different build gives
different numbers instead of silently reproducing these.

WHAT NONE OF THIS CAN SEE. A configuration that exists only because of a run-time value
(a colour constant, a flag read from a global) is reported as `global_load`, `computed` or
`live_in`, never resolved. The count of such sites is the measure of how open a route is.
"""

from __future__ import annotations

import struct
from collections import Counter
from collections.abc import Iterable, Sequence
from dataclasses import dataclass

import capstone
from capstone import x86 as cs_x86

from tools.d3dscan.methods import decode_header
from tools.shaderscan import callargs
from tools.shaderscan.callargs import KIND_IMM, KIND_MISSING, ArgValue
from tools.shaderscan.image import Image

#: Render-state indices 0 to 56 are the combiner methods. MEASURED: in the recovered
#: table, indices 0 to 56 are the alpha and colour input and output words, the factors,
#: the final-combiner words and the combiner control, in that order, and 57 onward are
#: depth, alpha, blend, stencil and polygon-offset state. See `docs/d3d8-usage.md` s10.
COMBINER_LAST_INDEX = 56

#: Class bounds of the dispatch as recovered by `tools.d3dscan.rstable`. MEASURED
#: defaults, overridable because a different build moves them.
IMMEDIATE_BOUND = 0x5C
DEFERRED_BOUND = 0x88
HANDLER_BOUND = 0xA6

CLASS_COMBINER = "combiner"
CLASS_IMMEDIATE_OTHER = "immediate_other"
CLASS_DEFERRED = "deferred"
CLASS_HANDLER = "handler"
CLASS_OUT_OF_RANGE = "out_of_range"

#: A loop is only a table walk when it advances by a plausible record size.
MIN_STRIDE = 4
MAX_STRIDE = 0x100

#: How far before a loop's top `find_array_walks` looks for the register's initial value.
INIT_LOOKBACK = 48


def classify_state_index(
    index: int,
    *,
    immediate_bound: int = IMMEDIATE_BOUND,
    deferred_bound: int = DEFERRED_BOUND,
    handler_bound: int = HANDLER_BOUND,
) -> str:
    """Which dispatch class a render-state index falls in, splitting off the combiners.

    The immediate class `[0, immediate_bound)` is split at `COMBINER_LAST_INDEX` because
    the first 57 of its entries are combiner methods and the rest are not.
    """
    if index < 0:
        return CLASS_OUT_OF_RANGE
    if index <= COMBINER_LAST_INDEX:
        return CLASS_COMBINER
    if index < immediate_bound:
        return CLASS_IMMEDIATE_OTHER
    if index < deferred_bound:
        return CLASS_DEFERRED
    if index < handler_bound:
        return CLASS_HANDLER
    return CLASS_OUT_OF_RANGE


def index_histogram(indices: Iterable[int], **bounds: int) -> Counter[str]:
    """Count `indices` per dispatch class. Pass the same keyword bounds as the classifier."""
    return Counter(classify_state_index(index, **bounds) for index in indices)


# --------------------------------------------------------------------------------------
# Route 2: a table walk


@dataclass(frozen=True)
class ArrayWalk:
    """A loop that visits `count` fixed-size records through one pointer register."""

    #: VA of the loop's first instruction (the backward branch's target).
    loop_va: int
    register: str
    #: The register's value on entry, as a `mov reg, imm` before the loop.
    start: int
    stride: int
    #: The register's value at which the loop stops.
    end: int
    #: Displacements the loop body uses off `register`.
    displacements: tuple[int, ...]
    #: The displacement of the field that is compared against a literal, or None.
    key_displacement: int | None

    @property
    def count(self) -> int:
        return (self.end - self.start + self.stride - 1) // self.stride


def find_array_walks(insns: Sequence[capstone.CsInsn]) -> list[ArrayWalk]:
    """Every `add reg, stride` / `cmp reg, end` / `jl top` loop with a literal start.

    The shape is what a compiler emits for a pointer-stepped array loop. The start comes
    from the nearest `mov reg, imm` above the loop top. The key field is the displacement
    that was loaded into a register and then compared with an immediate, which for a
    state-record table is the state index. A loop with no such start is skipped and not
    guessed, so an absent walk means "not recognised", not "absent".
    """
    walks: list[ArrayWalk] = []
    by_address = {insn.address: position for position, insn in enumerate(insns)}
    for position in range(len(insns)):
        stepped = _step_and_bound(insns, position)
        if stepped is None:
            continue
        register, stride, end, branch = stepped
        top = by_address.get(_branch_target(branch) or -1)
        if top is None or top >= position:
            continue
        start = _initial_value(insns, top, register)
        if start is None:
            continue
        body = insns[top : position + 1]
        displacements = sorted({d for d in _displacements(body, register)})
        walks.append(
            ArrayWalk(
                loop_va=insns[top].address,
                register=register,
                start=start,
                stride=stride,
                end=end,
                displacements=tuple(displacements),
                key_displacement=_compared_field(body, register),
            )
        )
    return walks


def _step_and_bound(
    insns: Sequence[capstone.CsInsn], position: int
) -> tuple[str, int, int, capstone.CsInsn] | None:
    """`(register, stride, end, branch)` if `insns[position]` begins `add reg, k; cmp; jl`."""
    step = insns[position]
    if step.mnemonic != "add" or len(step.operands) != 2 or position + 2 >= len(insns):
        return None
    dest, amount = step.operands
    if dest.type != cs_x86.X86_OP_REG or amount.type != cs_x86.X86_OP_IMM:
        return None
    if not MIN_STRIDE <= amount.imm <= MAX_STRIDE:
        return None
    compare, branch = insns[position + 1], insns[position + 2]
    if compare.mnemonic != "cmp" or branch.mnemonic not in ("jl", "jb", "jne", "jle", "jbe"):
        return None
    left, right = compare.operands
    if left.type != cs_x86.X86_OP_REG or left.reg != dest.reg:
        return None
    if right.type != cs_x86.X86_OP_IMM:
        return None
    return step.reg_name(dest.reg), int(amount.imm), right.imm & 0xFFFFFFFF, branch


def _branch_target(branch: capstone.CsInsn) -> int | None:
    ops = branch.operands
    if len(ops) == 1 and ops[0].type == cs_x86.X86_OP_IMM:
        return int(ops[0].imm) & 0xFFFFFFFF
    return None


def _initial_value(insns: Sequence[capstone.CsInsn], top: int, register: str) -> int | None:
    for position in range(top - 1, max(-1, top - 1 - INIT_LOOKBACK), -1):
        insn = insns[position]
        if insn.mnemonic != "mov" or len(insn.operands) != 2:
            continue
        dest, source = insn.operands
        if dest.type != cs_x86.X86_OP_REG or insn.reg_name(dest.reg) != register:
            continue
        if source.type == cs_x86.X86_OP_IMM:
            return int(source.imm) & 0xFFFFFFFF
        return None
    return None


def _displacements(body: Sequence[capstone.CsInsn], register: str) -> list[int]:
    found: list[int] = []
    for insn in body:
        for operand in insn.operands:
            if operand.type != cs_x86.X86_OP_MEM or not operand.mem.base:
                continue
            if insn.reg_name(operand.mem.base) == register and not operand.mem.index:
                found.append(int(operand.mem.disp))
    return found


def _compared_field(body: Sequence[capstone.CsInsn], register: str) -> int | None:
    """Displacement loaded into a register that the next few instructions compare to a literal."""
    for position, insn in enumerate(body):
        if insn.mnemonic != "mov" or len(insn.operands) != 2:
            continue
        dest, source = insn.operands
        if dest.type != cs_x86.X86_OP_REG or source.type != cs_x86.X86_OP_MEM:
            continue
        mem = source.mem
        if not mem.base or insn.reg_name(mem.base) != register or mem.index:
            continue
        for later in body[position + 1 : position + 3]:
            if later.mnemonic != "cmp" or len(later.operands) != 2:
                continue
            left, right = later.operands
            if (
                left.type == cs_x86.X86_OP_REG
                and left.reg == dest.reg
                and right.type == cs_x86.X86_OP_IMM
            ):
                return int(mem.disp)
    return None


def read_walk_indices(image: Image, walk: ArrayWalk) -> tuple[int, ...]:
    """The key field of every record the walk visits, read from initialised data.

    Raises ValueError rather than returning a short list, because a short list would
    shrink the denominator of every fraction computed from it.
    """
    if walk.key_displacement is None:
        raise ValueError("the walk has no recognised key field")
    values: list[int] = []
    for record in range(walk.count):
        address = walk.start + walk.key_displacement + record * walk.stride
        value = image.u32(address)
        if value is None:
            raise ValueError(f"record {record} key at {address:#x} is not initialised data")
        values.append(value)
    return tuple(values)


# --------------------------------------------------------------------------------------
# Call sites: stack arguments, tail calls and register arguments


def tail_call_argument(insns: Sequence[capstone.CsInsn], index: int, slot: int = 4) -> ArgValue:
    """The first stack argument of a tail `jmp`, written as `mov dword ptr [esp+slot], v`.

    A tail jump pushes nothing. Its argument is already in the caller's own frame, and
    the compiler overwrites that slot just before the jump. `trace_arguments` looks for
    pushes and reports `missing` here, so this reads the store instead. An argument that
    is passed through untouched also looks like `missing`, which is the honest answer.
    """
    for step in range(1, callargs.SCAN_LIMIT + 1):
        position = index - step
        if position < 0:
            break
        insn = insns[position]
        if insn.mnemonic in ("call", "jmp", "ret", "retn") or insn.mnemonic.startswith("j"):
            break
        if insn.mnemonic != "mov" or len(insn.operands) != 2:
            continue
        dest, source = insn.operands
        if dest.type != cs_x86.X86_OP_MEM or not dest.mem.base or dest.mem.index:
            continue
        if insn.reg_name(dest.mem.base) != "esp" or dest.mem.disp != slot:
            continue
        if source.type == cs_x86.X86_OP_IMM:
            return ArgValue(KIND_IMM, int(source.imm) & 0xFFFFFFFF)
        if source.type == cs_x86.X86_OP_REG:
            leaders = callargs.branch_leaders(insns)
            family = callargs.FAMILY_OF.get(insn.reg_name(source.reg), "")
            return callargs._resolve_register(insns, position, family, leaders)
        return ArgValue(callargs.KIND_UNRESOLVED, note=insn.op_str)
    return ArgValue(KIND_MISSING)


def first_argument(
    insns: Sequence[capstone.CsInsn], index: int, leaders: frozenset[int]
) -> ArgValue:
    """First stack argument of the transfer at `insns[index]`, for a call or a tail jump."""
    if insns[index].mnemonic == "jmp":
        return tail_call_argument(insns, index)
    return callargs.trace_arguments(insns, index, 1, leaders)[0]


def register_argument(
    insns: Sequence[capstone.CsInsn],
    index: int,
    register: str,
    leaders: frozenset[int],
) -> ArgValue:
    """What `register` holds at the transfer at `insns[index]`, for register conventions.

    The one-header write primitive takes the header in `ecx` and the value in `edx`, which
    `trace_arguments` cannot see because it only follows pushes. Uses the resolver
    `callargs` keeps for its own pushed registers.
    """
    family = callargs.FAMILY_OF.get(register, register)
    return callargs._resolve_register(insns, index, family, leaders)


@dataclass(frozen=True)
class HeaderSite:
    """One call to a header-in-register writer, with what it writes."""

    va: int
    kind: str
    header: ArgValue
    value: ArgValue
    #: The NV2A method number when the header is a literal that decodes as one.
    method: int | None


def header_sites(
    insns: Sequence[capstone.CsInsn],
    targets: set[int],
    leaders: frozenset[int],
    header_register: str = "ecx",
    value_register: str = "edx",
) -> list[HeaderSite]:
    """Every call or tail jump to `targets`, with the header and value registers classified."""
    found: list[HeaderSite] = []
    for position, insn in enumerate(insns):
        if insn.mnemonic not in ("call", "jmp"):
            continue
        target = _branch_target(insn)
        if target is None or target not in targets:
            continue
        header = register_argument(insns, position, header_register, leaders)
        value = register_argument(insns, position, value_register, leaders)
        method = None
        if header.kind == KIND_IMM and header.value is not None:
            decoded = decode_header(header.value)
            method = decoded.method if decoded is not None else None
        found.append(HeaderSite(insn.address, insn.mnemonic, header, value, method))
    return found


# --------------------------------------------------------------------------------------
# Route 1 and 3: the definition block and its consumer


@dataclass(frozen=True)
class FieldExtent:
    """The byte ranges of a block the consumer reads, as `(offset, length)` spans."""

    spans: tuple[tuple[int, int], ...]

    @property
    def size(self) -> int:
        """Smallest block that covers every span read."""
        return max((offset + length for offset, length in self.spans), default=0)


def field_extent(insns: Sequence[capstone.CsInsn], register: str) -> FieldExtent:
    """Which bytes of the block pointed to by `register` a function reads.

    Counts a plain load off the register as 4 bytes, and a `lea` of an address inside
    the block followed by `mov ecx, n` and `rep movsd` as `4*n` bytes. The size of the
    block is then the end of the last span. This is a LOWER bound on the true size,
    because a field the function never reads does not show up, and the report says so.
    """
    spans: list[tuple[int, int]] = []
    for position, insn in enumerate(insns):
        ops = insn.operands
        if insn.mnemonic == "lea" and len(ops) == 2 and _is_block_address(insn, ops[1], register):
            count = _movsd_count(insns, position)
            if count is not None:
                spans.append((int(ops[1].mem.disp), 4 * count))
            continue
        if insn.mnemonic == "lea":
            continue
        for operand in ops:
            if operand.type == cs_x86.X86_OP_MEM and _is_block_address(insn, operand, register):
                spans.append((int(operand.mem.disp), 4))
    return FieldExtent(tuple(sorted(set(spans))))


def _is_block_address(insn: capstone.CsInsn, operand: cs_x86.X86Op, register: str) -> bool:
    mem = operand.mem
    return (
        operand.type == cs_x86.X86_OP_MEM
        and bool(mem.base)
        and insn.reg_name(mem.base) == register
        and not mem.index
    )


def _movsd_count(insns: Sequence[capstone.CsInsn], position: int) -> int | None:
    """The literal `ecx` of the `rep movsd` that follows a `lea`, within a short window."""
    count: int | None = None
    for later in insns[position + 1 : position + 6]:
        if later.mnemonic == "mov" and later.op_str.startswith("ecx, ") and later.operands:
            source = later.operands[1]
            if source.type == cs_x86.X86_OP_IMM:
                count = int(source.imm)
        if later.mnemonic.startswith("rep movs"):
            return count
    return None


@dataclass(frozen=True)
class CopyBlock:
    """A `rep movsd` with a literal dword count."""

    va: int
    dwords: int
    #: The literal destination when `mov edi, imm` sets it, else None.
    destination: int | None


def copy_blocks(insns: Sequence[capstone.CsInsn]) -> list[CopyBlock]:
    """Every `rep movsd` whose count is a literal, with its literal destination if any."""
    blocks: list[CopyBlock] = []
    for position, insn in enumerate(insns):
        if not insn.mnemonic.startswith("rep movs"):
            continue
        count = destination = None
        for earlier in insns[max(0, position - 6) : position]:
            if earlier.mnemonic != "mov" or len(earlier.operands) != 2:
                continue
            dest, source = earlier.operands
            if dest.type != cs_x86.X86_OP_REG or source.type != cs_x86.X86_OP_IMM:
                continue
            name = earlier.reg_name(dest.reg)
            if name == "ecx":
                count = int(source.imm)
            elif name == "edi":
                destination = int(source.imm) & 0xFFFFFFFF
        if count is not None:
            blocks.append(CopyBlock(insn.address, count, destination))
    return blocks


# --------------------------------------------------------------------------------------
# Static definitions: counts only


def definition_gaps(addresses: Iterable[int]) -> Counter[int]:
    """Histogram of the byte gaps between consecutive distinct addresses.

    A table of fixed-size blocks shows as one dominant gap. That is how the block size is
    corroborated from the pointers alone, independently of `field_extent`.
    """
    ordered = sorted(set(addresses))
    return Counter(later - earlier for earlier, later in zip(ordered, ordered[1:], strict=False))


def distinct_contents(image: Image, addresses: Iterable[int], size: int) -> tuple[int, int]:
    """`(distinct blocks by content, blocks readable)` for `size`-byte blocks at `addresses`.

    Content is compared, never reported. Two addresses with equal bytes are one
    configuration, which is the right unit when counting how many different ones exist.
    """
    seen: set[bytes] = set()
    readable = 0
    for address in set(addresses):
        data = image.read(address, size)
        if data is None:
            continue
        readable += 1
        seen.add(data)
    return len(seen), readable


def field_histogram(
    image: Image, addresses: Iterable[int], offset: int, mask: int = 0xFFFFFFFF
) -> Counter[int]:
    """Histogram of one masked dword field across blocks. Counts only, no block content."""
    counts: Counter[int] = Counter()
    for address in set(addresses):
        data = image.read(address + offset, 4)
        if data is not None:
            counts[int(struct.unpack("<I", data)[0]) & mask] += 1
    return counts


# --------------------------------------------------------------------------------------
# The estimate


@dataclass(frozen=True)
class ConfigurationEstimate:
    """Bounds on the number of distinct installed combiner programs.

    `lower` and `upper` count PROGRAMS, i.e. distinct definition blocks that can reach
    `SetPixelShader`. `variants` is the further multiplier from direct word writes made
    after installation, and `open_routes` counts routes whose inputs are not literals.
    """

    static_definitions: int
    generator_proven: int
    generator_reachable: int
    cache_slots: int
    lower: int
    upper: int
    variants: int
    open_routes: int

    @property
    def upper_with_variants(self) -> int:
        return self.upper * self.variants


def estimate_configurations(
    static_definitions: int,
    generator_reachable: int,
    cache_slots: int,
    generator_proven: int = 0,
    variants: int = 1,
    open_routes: int = 0,
) -> ConfigurationEstimate:
    """Combine the measured counts into bounds, without adding anything not measured.

    `generator_reachable` is how many distinct outputs the builder can produce over its
    whole key domain, which is an over-approximation of what the game ever asks for.
    `generator_proven` is how many of those are known to be asked for, which is zero
    unless every key at every call site was resolved to a literal.

    LOWER: static definitions, each read from the image and named by a literal call site,
    plus the generator outputs proven reachable. UPPER: the static definitions plus the
    smaller of the generator's reachable outputs and the run-time cache's slots. The
    cache's over-capacity check calls a function that is a bare `ret` in the retail
    build, so exceeding the bound would silently overwrite neighbouring memory and not
    abort. A title that ships and runs is INFERRED not to exceed it, which is why this
    is the weaker of the two ceilings and is reported separately. Inputs that contradict
    each other are rejected and not clamped.
    """
    ceiling = min(generator_reachable, cache_slots)
    if generator_proven > ceiling:
        raise ValueError(
            f"{generator_proven} proven generator outputs exceed the ceiling of {ceiling}, "
            "so either the proof or one of the bounds is wrong"
        )
    return ConfigurationEstimate(
        static_definitions=static_definitions,
        generator_proven=generator_proven,
        generator_reachable=generator_reachable,
        cache_slots=cache_slots,
        lower=static_definitions + generator_proven,
        upper=static_definitions + ceiling,
        variants=variants,
        open_routes=open_routes,
    )
