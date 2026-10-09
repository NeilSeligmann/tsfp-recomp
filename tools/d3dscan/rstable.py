# SPDX-License-Identifier: GPL-3.0-or-later
"""Recover the deferred render-state dispatch tables that `SetRenderState` is compiled into.

WHY THIS EXISTS. `tools/d3dscan/pushscan.py` recovers method headers that a function
writes as an *immediate*, which is what D3D8's own writers mostly do. `SetRenderState`
does not: it reads its header out of a table indexed by the render-state number, so a
scan for immediates reports it as "not recovered". The table is the whole
render-state-to-hardware mapping, and it can be read out of the image directly, which
turns `SetRenderState` from ~92 hand-written cases into one data-driven loop.

THE SHAPE, AS MEASURED AND NOT AS ASSUMED. A dispatch site is

    cmp   <index>, <bound>
    jge   <next class>
    mov   <reg>, [<index>*4 + <base>]

repeated once per class, and the classes partition the render-state index space. This
module finds every such site by that shape -- a scale-4 constant-base load preceded,
within a bounded instruction window, by a compare of the same register feeding a
conditional branch -- and never by searching for a known base or a known bound. The
bases and bounds are outputs, so a different build or a different title produces
different numbers rather than silently reproducing these.

`jge` IS SIGNED, AND THAT IS A FINDING, NOT AN ASIDE. The first class is entered when
`index < bound` *as a signed comparison*, so a negative index also reaches it and indexes
the table backwards. Nothing in the dispatch rejects it. Any reimplementation that uses
an unsigned index silently diverges on that path; `d3d8_render_state.c` reports it
instead.

THE TWO DERIVATION ROUTES, AND WHY TWO ARE NEEDED
=================================================

`docs/d3d8-usage.md` §2.1 records that a method table written from recall was wrong in
46 of 102 checkable entries, off by one slot, so every name was a real name on the wrong
number. Nothing about such a table looks wrong. So every row here is required to come
back the same from two routes that cannot share that error:

ROUTE D (data).  VA -> file offset through the XBE section table, then `struct.unpack` of
    little-endian dwords, decoded by `methods.is_plausible_header`'s field constraints
    only. No disassembler is involved.

ROUTE C (code).  `capstone` instruction decoding, through
    `gen_d3d8_surface.decode_section`'s resynchronising sweep. It supplies the base, the
    element scale and the bound; and for the handler class it supplies the same
    index->target mapping a *second* time, because the game's `.text` carries an inlined
    `cmp`/`jne`/`call rel32` chain while D3D8's own `.text` carries a function-pointer
    table. A `rel32` displacement and an absolute dword in a data table are different
    encodings of the same address reached by different decoders, so a slot shift in one
    cannot be mirrored in the other.

Route C also pins the SLOT ALIGNMENT, which a run length cannot. `header_geometry`
reports the +4 runs the header table decomposes into; at the true base those runs land on
reference-documented array bases with their documented widths, and at base +/- one slot
they do not. `alignment_margin` computes that for a window of candidate bases so the
margin is a number rather than an assurance.

NAMES ARE SEPARATE AND CARRY THEIR OWN COUNT. `name_methods` takes any number of
reference register headers and records, per method, how many of them named it. A row
named by two independent references is doubly derived; a row named by one is marked as
such in the generated C instead of being quietly included; a row named by none keeps its
bare number. See `docs/provenance.md` on why the numbers and names are facts about NV2A
hardware and the header files themselves are never copied.

WHAT THIS CANNOT DO. It cannot tell you the `D3DRS_*` enumerant for an index. The index
space is the guest's, the methods are the hardware's, and the D3D-level name sits between
them in a header this project may not read.
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

import capstone

from tools.d3dscan.methods import (
    MethodHeader,
    decode_header,
    is_plausible_header,
    parse_method_names,
)
from tools.d3dscan.pushscan import load_image
from tools.gen_d3d8_surface import Image, decode_section
from tools.xbe import XbeSection

#: How far back from a scaled-index load to look for the compare that bounds the index.
#: MEASURED: in the immediate class the compare is two instructions back, but the handler
#: class is reached by a branch over the whole deferred class, which is 10 instructions.
#: 16 covers both. Widening it costs false pairings, so the report prints the site count.
COMPARE_LOOKBACK = 16

#: `name_methods` will not reach more than this many dwords past a documented base.
#: MEASURED: the widest array this table indexes is 8 dwords (the eight combiner stages),
#: so 7 is the largest reach any row legitimately needs. `methods.MAX_ARRAY_SPAN` is 0x80
#: because it must also cover `SET_TRANSFORM_CONSTANT`'s 32-dword window, and that reach
#: is what produced `SET_SHADER_STAGE_PROGRAM+1` for `0x1E74` in an earlier analysis when
#: the real name, which both references carry, is `SET_DOT_RGBMAPPING`.
MAX_NAME_ELEMENT = 7

#: Scale of a dword-array index. A dispatch table of 32-bit entries has no other.
INDEX_SCALE = 4

#: Dispatch sites belonging to one dispatch sit within this many bytes of each other.
#: MEASURED at 0x51 and 0x31 bytes in the two compiled copies; 0x100 is loose enough to
#: survive a different schedule and tight enough not to merge two dispatches.
SITE_GROUP_BYTES = 0x100

#: A header table entry is one parameter on subchannel 0. Anything else in the run means
#: the run is not what it looks like, so it is checked rather than assumed.
EXPECTED_COUNT = 1
EXPECTED_SUBCHANNEL = 0

#: Spelling disagreements between the two local references that are KNOWN to be the same
#: method, verified by hand against both headers. `cross_check` fails on any dispute NOT
#: listed here. The allowlist is BY NAME, never by count: a count of "1 known dispute"
#: would keep passing when the known one disappeared and a new, unexamined one took its
#: place, which is exactly the regression a cross-check exists to catch.
KNOWN_SPELLING_VARIANTS: frozenset[frozenset[str]] = frozenset(
    {
        # nxdk's nv_regs.h says SET_SHADE_MODE, the other reference SET_SHADE_MODEL,
        # both for 0x037C. Same method, one trailing letter. docs/d3d8-usage.md section 10.
        frozenset({"SET_SHADE_MODEL", "SET_SHADE_MODE"}),
    }
)


@dataclass(frozen=True)
class DispatchSite:
    """One bounded scale-4 constant-base table access. Route C.

    `bound` is None for the handler class, because MEASURED there is no compare bounding
    it: the dispatch falls through to the indirect call and an index past the table calls
    through whatever follows it. Demanding a compare there would be demanding something
    the guest does not have.
    """

    section: str
    load_va: int
    base: int
    bound: int | None
    index_reg: str
    #: True when the table is the DESTINATION, i.e. this is a store into a shadow array
    #: rather than a read of a parameter table. Separating them matters: the deferred
    #: class reads a mask table and writes a shadow array in the same two instructions,
    #: and taking the store's base for the mask table's is an easy, silent swap.
    writes: bool = False
    #: True for `call dword ptr [index*4 + base]`, i.e. a function-pointer dispatch.
    indirect_call: bool = False


@dataclass(frozen=True)
class NamedMethod:
    """A method number with however much naming evidence there is, and no more.

    `sources` counts the references that name `base_name` EXACTLY. `element` says how many
    dwords past that base the method sits, so a row whose name was reached rather than read
    says so. Both go into the generated C: the brief this work answers requires that
    anything resting on a single route be marked in the table rather than silently included.
    """

    method: int
    base_name: str | None
    sources: int
    element: int = 0
    #: True when the references that named the base disagreed on the spelling.
    disputed: bool = False
    #: Every spelling the references gave for the base, so a dispute can be checked
    #: against `KNOWN_SPELLING_VARIANTS` by name rather than merely flagged.
    spellings: tuple[str, ...] = ()

    def label(self) -> str:
        if self.base_name is None:
            return f"{self.method:#06x}"
        name = self.base_name + (f"+{self.element}" if self.element else "")
        marks = ""
        if self.sources < 2:
            marks += f" [{self.sources} source]"
        if self.disputed:
            marks += " [spelling disputed]"
        return name + marks


@dataclass(frozen=True)
class HeaderRun:
    """A maximal run of header entries whose methods step by exactly +4."""

    first_index: int
    length: int
    first_method: int

    @property
    def last_method(self) -> int:
        return self.first_method + 4 * (self.length - 1)


@dataclass
class RenderStateTables:
    """Everything the dispatch is parameterised by, with its provenance attached."""

    #: Base of the one-parameter command headers, indexed by state directly.
    immediate_base: int
    #: First class bound, from route C. `len(headers)` is the same number from route D.
    immediate_bound: int
    headers: list[MethodHeader]

    #: Base of the deferred dirty-bit masks. BIASED: only [immediate_bound, deferred_bound)
    #: is ever formed, so the array's real storage starts at base + immediate_bound*4.
    deferred_base: int
    deferred_bound: int
    deferred_masks: list[int]

    #: Base of the per-state shadow array the deferred class stores into. Indexed by the
    #: raw state, so it is NOT biased. Guest-visible state, not a parameter table.
    shadow_base: int | None

    #: Base of the per-state handler pointers, biased the same way. None when no indirect
    #: dispatch was found.
    handler_base: int | None
    handler_bound: int
    handlers: list[int]
    #: index -> target from the inlined compare chain. Route C's second, independent pass.
    chain_handlers: dict[int, int] = field(default_factory=dict)

    sites: list[DispatchSite] = field(default_factory=list)

    #: Full length of route D's header run BEFORE it is clipped to route C's bound.
    #: Kept so `cross_check` can compare the two derivations instead of one silently
    #: winning. At the true base the run is exactly the bound: see `alignment_margin`.
    route_d_run: int = 0

    @property
    def deferred_storage_base(self) -> int:
        """Where the deferred array's first live entry actually is."""
        return self.deferred_base + INDEX_SCALE * self.immediate_bound

    def handler_agreement(self) -> tuple[int, int, list[int]]:
        """(agreed, compared, disagreeing indices) between the pointer table and the chain."""
        compared = sorted(set(self.chain_handlers) & self._pointer_indices())
        bad = [i for i in compared if self.chain_handlers[i] != self.handler_for(i)]
        return len(compared) - len(bad), len(compared), bad

    def _pointer_indices(self) -> set[int]:
        return set(range(self.deferred_bound, self.deferred_bound + len(self.handlers)))

    def handler_for(self, index: int) -> int | None:
        offset = index - self.deferred_bound
        if not 0 <= offset < len(self.handlers):
            return None
        return self.handlers[offset]


def _mem_table_operand(insn: capstone.CsInsn) -> tuple[int, int, bool] | None:
    """`(base, index_reg, is_destination)` for a scale-4 constant-base operand, or None."""
    for position, operand in enumerate(insn.operands):
        if operand.type != capstone.x86.X86_OP_MEM:
            continue
        mem = operand.mem
        if mem.base == 0 and mem.index != 0 and mem.scale == INDEX_SCALE and mem.disp:
            # Operand 0 is the destination for every two-operand form this dispatch uses.
            written = position == 0 and len(insn.operands) == 2
            return mem.disp & 0xFFFFFFFF, mem.index, written
    return None


def _compare_bound(window: list[capstone.CsInsn], index_reg: int) -> int | None:
    """The immediate of the last `cmp index_reg, imm` in `window` that feeds a branch."""
    for position in range(len(window) - 1, -1, -1):
        insn = window[position]
        if insn.mnemonic != "cmp" or len(insn.operands) != 2:
            continue
        left, right = insn.operands
        if left.type != capstone.x86.X86_OP_REG or left.reg != index_reg:
            continue
        if right.type != capstone.x86.X86_OP_IMM:
            continue
        # The compare must actually guard something, or it is not a bound.
        following = window[position + 1 : position + 3]
        if not any(item.mnemonic.startswith("j") for item in following):
            continue
        return right.imm & 0xFFFFFFFF
    return None


def find_dispatch_sites(image: Image, section: XbeSection) -> list[DispatchSite]:
    """Every bounded scale-4 table load in `section`. Route C, and the only locator used.

    No base and no bound is searched for by value, so the result is a measurement of this
    image rather than a confirmation of a number written here.
    """
    instructions, _ = decode_section(image.section_body(section), section.virtual_addr)
    sites: list[DispatchSite] = []
    for position, insn in enumerate(instructions):
        found = _mem_table_operand(insn)
        if found is None:
            continue
        base, index_reg, written = found
        window = instructions[max(0, position - COMPARE_LOOKBACK) : position]
        bound = _compare_bound(window, index_reg)
        indirect = insn.mnemonic == "call"
        if bound is None and not indirect:
            continue
        sites.append(
            DispatchSite(
                section=section.name,
                load_va=insn.address,
                base=base,
                bound=bound,
                index_reg=insn.reg_name(index_reg) or f"r{index_reg}",
                writes=written,
                indirect_call=indirect,
            )
        )
    return sites


def header_run(image: Image, base: int, limit: int = 4096) -> list[MethodHeader]:
    """Consecutive dwords at `base` that decode as command headers. Route D."""
    out: list[MethodHeader] = []
    for step in range(limit):
        value = image.read_u32(base + INDEX_SCALE * step)
        if value is None:
            break
        header = decode_header(value)
        if header is None:
            break
        out.append(header)
    return out


def header_geometry(headers: list[MethodHeader]) -> list[HeaderRun]:
    """Decompose the table into maximal +4 runs. This is what pins the slot alignment."""
    runs: list[HeaderRun] = []
    start = 0
    for position in range(1, len(headers) + 1):
        ends = position == len(headers)
        if not ends and headers[position].method == headers[position - 1].method + 4:
            continue
        runs.append(
            HeaderRun(
                first_index=start,
                length=position - start,
                first_method=headers[start].method,
            )
        )
        start = position
    return runs


def alignment_score(headers: list[MethodHeader], names: dict[int, str]) -> int:
    """How many +4 runs begin exactly on a reference-documented method. Route C x names."""
    return sum(1 for run in header_geometry(headers) if run.first_method in names)


@dataclass(frozen=True)
class Alignment:
    """What a candidate base yields, so the slot the table really starts on is a number."""

    shift_bytes: int
    run_length: int
    runs: int
    on_documented_base: int

    def viable(self, bound: int) -> bool:
        """Route D's run must be exactly as long as route C's bound, or the base is wrong."""
        return self.run_length == bound


def alignment_margin(
    image: Image, base: int, bound: int, names: dict[int, str], slots: int = 2
) -> list[Alignment]:
    """What each candidate base one or two slots either side of `base` would give.

    THIS IS THE OFF-BY-ONE-SLOT CHECK, and it is the reason the base is reported rather
    than asserted. MEASURED on retail: one slot DOWN yields a run of length 0, because the
    dword below the table is the last zero of the preceding array and zero is not a header;
    one slot UP yields a run one short of the bound route C read out of the compare. So the
    base is pinned from below by the header field constraints and from above by the
    code/data length agreement, and those two constraints do not share a premise.
    """
    out: list[Alignment] = []
    for shift in range(-slots, slots + 1):
        headers = header_run(image, base + INDEX_SCALE * shift)
        clipped = headers[:bound]
        out.append(
            Alignment(
                shift_bytes=INDEX_SCALE * shift,
                run_length=len(headers),
                runs=len(header_geometry(clipped)),
                on_documented_base=alignment_score(clipped, names),
            )
        )
    return out


def run_length_null(data: bytes) -> Counter[int]:
    """`{run length: how many 4-aligned dwords start a maximal run that long}`.

    Exhaustive over `data`, so there is no sample and no seed. Run it on a section with
    no header table in it and the result is the distribution a coincidence is drawn from.
    """
    count = len(data) // INDEX_SCALE
    if count == 0:
        return Counter()
    values = struct.unpack_from(f"<{count}I", data, 0)
    runs: Counter[int] = Counter()
    tail = 0
    for value in reversed(values):
        tail = tail + 1 if is_plausible_header(value) else 0
        runs[tail] += 1
    return runs


def compare_chain(image: Image, entry: int, length: int) -> dict[int, int]:
    """`{index: target}` from a `cmp index, imm` / `jne` / `call rel32` chain. Route C.

    The second, independent pass over the handler class: these are `rel32` displacements
    in the game's own code, where `handler_pointers` reads absolute dwords out of D3D8's
    data. A slot shift cannot appear in both.
    """
    offset = image.offset(entry)
    if offset is None:
        return {}
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    out: dict[int, int] = {}
    pending: int | None = None
    for insn in decoder.disasm(image.data[offset : offset + length], entry):
        if insn.mnemonic == "cmp" and len(insn.operands) == 2:
            right = insn.operands[1]
            if right.type == capstone.x86.X86_OP_IMM:
                pending = right.imm & 0xFFFFFFFF
        elif insn.mnemonic == "call" and insn.operands:
            target = insn.operands[0]
            if target.type == capstone.x86.X86_OP_IMM and pending is not None:
                out.setdefault(pending, target.imm & 0xFFFFFFFF)
    return out


def handler_pointers(image: Image, base: int, first_index: int) -> list[int]:
    """Dwords at `base + index*4` from `first_index` on that are function entries. Route D.

    The stop condition is `Image.is_entry_point`, which requires the bytes to decode AND
    the preceding bytes to end a function. A float or a stray integer fails both, which is
    what bounds the table without a length having to be assumed.
    """
    out: list[int] = []
    index = first_index
    while True:
        value = image.read_u32(base + INDEX_SCALE * index)
        if value is None or not image.is_entry_point(value):
            break
        out.append(value)
        index += 1
    return out


def name_methods(
    method: int, tables: list[dict[int, str]], max_element: int = MAX_NAME_ELEMENT
) -> NamedMethod:
    """Name `method` by nearest documented base, counting how many references agree.

    The nearest base is taken over the UNION of the tables, so a base only one reference
    documents still sharpens the array boundary for the other. That is not a nicety: one
    reference stops `SET_SPECULAR_FOG_FACTOR` after 2 dwords and the other leaves the next
    base 32 dwords away, and only the union gets the array width right.

    `sources` then counts the references naming that one base, which is deliberately NOT
    the same as "how confident are we in this row". A row at `base+6` with one source is
    reported as exactly that and nothing is rounded up.
    """
    union: dict[int, list[str]] = {}
    for table in tables:
        for base, name in table.items():
            union.setdefault(base, []).append(name)
    reach = INDEX_SCALE * max_element
    candidates = [base for base in union if base <= method and method - base <= reach]
    if not candidates:
        return NamedMethod(method=method, base_name=None, sources=0)
    base = max(candidates)
    spellings = union[base]
    return NamedMethod(
        method=method,
        base_name=spellings[0],
        sources=len(spellings),
        element=(method - base) // INDEX_SCALE,
        disputed=len(set(spellings)) > 1,
        spellings=tuple(spellings),
    )


def cross_check(tables: RenderStateTables, names: list[dict[int, str]]) -> list[str]:
    """Every place two derivation routes disagree, as printable findings. Empty is agreement.

    THE POINT: where this tool derives the same fact twice, neither derivation is allowed
    to win silently. `main` fails the run on any finding. The brief is T24 in
    `docs/tasks.md`: a lowercase-only hex pattern dropped `SET_DOT_RGBMAPPING` from BOTH
    references at once, and nothing reported it, so a single-route derivation was
    confidently wrong and self-consistent. Three cross-checks, one per doubly derived fact:

    1. Route D's header run length against route C's compare bound.
    2. Route D's handler pointer table against route C's inlined compare chain.
    3. The two references' spellings for each named method, against
       `KNOWN_SPELLING_VARIANTS` by name.
    """
    findings: list[str] = []
    if tables.route_d_run != tables.immediate_bound:
        findings.append(
            f"ROUTE DISAGREEMENT: route D header run is {tables.route_d_run} entries at "
            f"base {tables.immediate_base:#010x}, route C compare bound is "
            f"{tables.immediate_bound}; the base or the bound is wrong"
        )
    _, _, bad = tables.handler_agreement()
    for index in bad:
        findings.append(
            f"ROUTE DISAGREEMENT: handler index {index:#x} is "
            f"{tables.handler_for(index):#010x} in the pointer table (route D) but "
            f"{tables.chain_handlers[index]:#010x} in the compare chain (route C)"
        )
    for header in tables.headers:
        named = name_methods(header.method, names)
        if named.disputed and frozenset(named.spellings) not in KNOWN_SPELLING_VARIANTS:
            findings.append(
                f"NAME DISAGREEMENT: method {header.method:#06x} is spelled "
                f"{sorted(set(named.spellings))} by the references and that pair is not in "
                f"KNOWN_SPELLING_VARIANTS; verify by hand before allowlisting it by name"
            )
    return findings


def recover(image: Image, code_sections: list[str]) -> RenderStateTables:
    """Find the render-state dispatch and read every table it is parameterised by."""
    sections = {section.name: section for section in image.xbe.sections}
    sites: list[DispatchSite] = []
    for name in code_sections:
        if name in sections:
            sites.extend(find_dispatch_sites(image, sections[name]))

    # The immediate class is the one whose base begins the longest run of headers. Chosen
    # by measurement over every site found, not by looking for a known base.
    best: tuple[int, DispatchSite] | None = None
    for site in sites:
        if site.bound is None or site.writes:
            continue
        length = len(header_run(image, site.base))
        if best is None or length > best[0]:
            best = (length, site)
    if best is None or best[0] == 0:
        raise SystemExit("no bounded scale-4 table load reaches a run of command headers")
    immediate_site = best[1]
    immediate_bound = immediate_site.bound
    assert immediate_bound is not None

    headers = header_run(image, immediate_site.base)

    # Every compiled copy of this dispatch, not just the one the immediate table was found
    # through. MEASURED: the game's inlined copy reaches the handler class with a compare
    # chain and D3D8's own copy with a function-pointer table, so the pointer table is only
    # reachable from the OTHER copy and restricting to one group loses it.
    anchors = [site for site in sites if site.base == immediate_site.base]
    group = [
        site
        for site in sites
        if any(
            site.section == anchor.section
            and abs(site.load_va - anchor.load_va) <= SITE_GROUP_BYTES
            for anchor in anchors
        )
    ]

    # The deferred class is the next bound up that READS a table. The store to the shadow
    # array shares its bound and is kept apart rather than merged with it.
    reads = sorted(
        {
            (site.bound, site.base)
            for site in group
            if site.bound is not None and not site.writes and not site.indirect_call
        }
    )
    deferred = next(((b, base) for b, base in reads if b > immediate_bound), None)
    if deferred is None:
        raise SystemExit(f"only one read class found at {immediate_site.load_va:#010x}")
    deferred_bound, deferred_base = deferred
    deferred_masks = [
        image.read_u32(deferred_base + INDEX_SCALE * index) or 0
        for index in range(immediate_bound, deferred_bound)
    ]
    shadow_base = next((site.base for site in group if site.writes), None)

    # Among the indirect calls in the dispatch, the handler table is the one that actually
    # yields function entry points at this index range. Scoring it rather than taking the
    # first matters: an unrelated indirect call can sit within the grouping window.
    scored = sorted(
        (
            (len(handler_pointers(image, site.base, deferred_bound)), site.base)
            for site in group
            if site.indirect_call
        ),
        reverse=True,
    )
    handler_base = scored[0][1] if scored and scored[0][0] else None
    handlers = handler_pointers(image, handler_base, deferred_bound) if handler_base else []

    chain: dict[int, int] = {}
    for site in sites:
        if site.base == immediate_site.base and not site.indirect_call:
            chain.update(compare_chain(image, site.load_va - 0x40, 0x400))

    return RenderStateTables(
        immediate_base=immediate_site.base,
        immediate_bound=immediate_bound,
        headers=headers[:immediate_bound],
        deferred_base=deferred_base,
        deferred_bound=deferred_bound,
        deferred_masks=deferred_masks,
        shadow_base=shadow_base,
        handler_base=handler_base,
        handler_bound=deferred_bound + len(handlers),
        handlers=handlers,
        chain_handlers={k: v for k, v in chain.items() if k >= deferred_bound},
        sites=sites,
        route_d_run=len(headers),
    )


# --------------------------------------------------------------------------------------
# C emission


_C_PREAMBLE = """\
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GENERATED, AND COMMITTED ON PURPOSE. Regenerate with:
 *
 *     python -m tools.d3dscan.rstable <your own default.xbe> \\
 *         --method-names <an NV2A register header> \\
 *         --method-names <a second, independent one> \\
 *         --emit-c src/gpu/d3d8_render_state_table.c
 *
 * ONLY THE TABLE IS GENERATED. The dispatch that walks it is hand-written in
 * `d3d8_render_state.c`, so regenerating against a different build cannot silently
 * rewrite behaviour, only data.
 *
 * WHY COMMITTED RATHER THAN GITIGNORED. `src/xbox/xdk_surface.c` is generated and
 * gitignored because every row of it is a guest address recovered from the user's
 * executable, and `src/audio/dsound_hle.c` compiles its rows in so a fresh clone builds.
 * This file follows `dsound_hle.c`: it holds NV2A method numbers and D3D8 dirty-bit
 * masks, which `docs/provenance.md` classes as facts about the hardware and our own
 * analysis output, and `src/gpu/` cannot do anything at all without them. The per-state
 * handler ADDRESSES, which are the part that would be an address dump, are deliberately
 * NOT emitted here -- the HLE replaces those handlers rather than calling them, so only
 * the index range they occupy is needed. `tools/ci/check-no-disc-data.sh` rejects bulk
 * per-address output by content, and this file carries none.
 *
 * NAMES CARRY THEIR SOURCE COUNT. `name_sources` is how many independent NV2A register
 * references named the method: 2 means doubly derived, 1 means a single route and is
 * marked rather than quietly included, 0 means no local reference names it and `name` is
 * NULL. `docs/d3d8-usage.md` §2.1 is why: a table of this shape written from recall was
 * wrong in 46 of 102 entries, off by one slot, and every name looked plausible.
 */

#include "d3d8_render_state.h"

"""


def emit_c(tables: RenderStateTables, names: list[dict[int, str]]) -> str:
    """The committed table, as C.

    Methods are emitted BARE, not as assembled headers, so the header encoding lives in
    exactly one place (`D3D8_NV2A_HEADER`) and a mutation of it is detectable. It also
    keeps every literal in this file to four hex digits, which is why
    `tools/ci/check-no-disc-data.sh`'s bulk-per-address content check sees nothing here.
    """
    lines = [_C_PREAMBLE]
    lines.append(f"const uint32_t d3d8_rs_immediate_bound = {tables.immediate_bound:#04x};")
    lines.append(f"const uint32_t d3d8_rs_deferred_bound  = {tables.deferred_bound:#04x};")
    lines.append(f"const uint32_t d3d8_rs_handler_bound   = {tables.handler_bound:#04x};")
    lines.append("")
    lines.append("const d3d8_render_state_row d3d8_render_state_rows[D3D8_RS_TABLE_ROWS] = {")
    for index in range(tables.handler_bound):
        if index < tables.immediate_bound:
            header = tables.headers[index]
            named = name_methods(header.method, names)
            name = "NULL" if named.base_name is None else f'"{named.base_name}"'
            lines.append(
                f"    [{index:#04x}] = {{ 0x{header.method:04X}, 0, D3D8_RS_IMMEDIATE, "
                f"{named.sources}, {named.element}, {int(named.disputed)}, {name} }},"
            )
        elif index < tables.deferred_bound:
            mask = tables.deferred_masks[index - tables.immediate_bound]
            lines.append(
                f"    [{index:#04x}] = {{ 0, 0x{mask:04X}, D3D8_RS_DEFERRED, 0, 0, 0, NULL }},"
            )
        else:
            lines.append(f"    [{index:#04x}] = {{ 0, 0, D3D8_RS_HANDLER, 0, 0, 0, NULL }},")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def _report(tables: RenderStateTables, names: list[dict[int, str]], image: Image) -> None:
    print(
        f"immediate headers: base {tables.immediate_base:#010x} "
        f"bound {tables.immediate_bound:#x} ({tables.immediate_bound}) "
        f"route-D run {len(tables.headers)}"
    )
    print(
        f"deferred masks:    biased base {tables.deferred_base:#010x} "
        f"-> storage {tables.deferred_storage_base:#010x} "
        f"[{tables.immediate_bound:#x},{tables.deferred_bound:#x}) "
        f"= {len(tables.deferred_masks)} entries"
    )
    shadow = f"{tables.shadow_base:#010x}" if tables.shadow_base else "none"
    print(f"shadow array:      base {shadow} (guest-visible state, not a parameter table)")
    base = f"{tables.handler_base:#010x}" if tables.handler_base else "none"
    print(
        f"handlers:          biased base {base} "
        f"[{tables.deferred_bound:#x},{tables.handler_bound:#x}) "
        f"= {len(tables.handlers)} entries"
    )
    print(f"dispatch sites:    {len(tables.sites)} bounded scale-4 accesses found in total")
    agreed, compared, bad = tables.handler_agreement()
    print(
        f"handler agreement: {agreed} of {compared} indices agree between the pointer "
        f"table (route D) and the inlined compare chain (route C)"
        + (f"; DISAGREE at {[hex(i) for i in bad]}" if bad else "")
    )

    bad_fields = [
        index
        for index, header in enumerate(tables.headers)
        if header.count != EXPECTED_COUNT
        or header.subchannel != EXPECTED_SUBCHANNEL
        or header.non_incrementing
    ]
    print(
        f"header fields:     {len(tables.headers) - len(bad_fields)} of {len(tables.headers)} "
        f"are one parameter, subchannel 0, auto-incrementing"
        + (f"; odd at {[hex(i) for i in bad_fields]}" if bad_fields else "")
    )

    union = {}
    for table in names:
        union.update(table)
    print(
        "slot alignment, the off-by-one-slot check "
        f"(route-D run length must equal route-C bound {tables.immediate_bound}):"
    )
    for item in alignment_margin(image, tables.immediate_base, tables.immediate_bound, union):
        verdict = "VIABLE" if item.viable(tables.immediate_bound) else "rejected"
        tag = "  <- the recovered base" if item.shift_bytes == 0 else ""
        print(
            f"  base{item.shift_bytes:+5d}: run {item.run_length:3d}, {item.runs:3d} +4 runs, "
            f"{item.on_documented_base:3d} on a documented base  {verdict}{tag}"
        )

    print("\nheader table geometry:")
    for run in header_geometry(tables.headers):
        named = name_methods(run.first_method, names)
        print(
            f"  [{run.first_index:3d}..{run.first_index + run.length - 1:3d}] "
            f"{run.length:2d} x +4 from {run.first_method:#06x}  {named.label()}"
        )

    print("\nper-index methods:")
    for index, header in enumerate(tables.headers):
        named = name_methods(header.method, names)
        print(f"  [{index:3d}] {header.raw:#010x} {header.method:#06x}  {named.label()}")

    singles = [
        index
        for index, header in enumerate(tables.headers)
        if name_methods(header.method, names).sources == 1
    ]
    unnamed = [
        index
        for index, header in enumerate(tables.headers)
        if name_methods(header.method, names).sources == 0
    ]
    print(
        f"\nnaming: {len(tables.headers) - len(singles) - len(unnamed)} doubly derived, "
        f"{len(singles)} single source {[hex(i) for i in singles]}, "
        f"{len(unnamed)} unnamed {[hex(i) for i in unnamed]}"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.d3dscan.rstable",
        description="Recover the render-state dispatch tables from a linked XDK image.",
    )
    parser.add_argument("xbe", type=Path, help="path to the XBE image")
    parser.add_argument(
        "--code-section",
        action="append",
        default=None,
        help="code section to search for dispatch sites; repeatable "
        "(default: .text and D3D, i.e. the game's inlined copy and the library's own)",
    )
    parser.add_argument(
        "--method-names",
        type=Path,
        action="append",
        default=None,
        help="an NV2A register reference header. REPEAT IT: a name confirmed by two "
        "independent references is doubly derived, one by a single reference is marked",
    )
    parser.add_argument(
        "--null",
        metavar="SECTION",
        action="append",
        default=None,
        help="report the maximal-header-run length distribution over this section's "
        "bytes, exhaustively, as the null model for a run of a given length; repeatable",
    )
    parser.add_argument(
        "--emit-c",
        type=Path,
        help="write the committed C table here instead of reporting",
    )
    parser.add_argument(
        "--check",
        type=Path,
        help="compare an existing generated C file against a fresh recovery; "
        "exit 1 on any difference",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    image = load_image(args.xbe)

    names: list[dict[int, str]] = [parse_method_names(path) for path in args.method_names or ()]

    if args.null:
        for name in args.null:
            section = next((s for s in image.xbe.sections if s.name == name), None)
            if section is None:
                raise SystemExit(f"no section named {name!r}")
            runs = run_length_null(image.section_body(section))
            total = sum(runs.values())
            print(
                f"{name}: {total} 4-aligned dwords, {total - runs[0]} plausible "
                f"= {(total - runs[0]) / total:.2%}"
            )
            for threshold in (2, 4, 8, 16, 32, 64, 92):
                hits = sum(count for length, count in runs.items() if length >= threshold)
                print(
                    f"  maximal run >= {threshold:3d} starting here: {hits:6d} = {hits / total:.3e}"
                )
            longest = max(runs) if runs else 0
            print(f"  longest run observed: {longest}")
        return 0

    tables = recover(image, args.code_section or [".text", "D3D"])

    # FAIL LOUDLY on any disagreement between the two derivation routes, in every mode,
    # before anything is emitted or compared. Neither route is preferred.
    findings = cross_check(tables, names)
    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        print(
            f"cross-check FAILED: {len(findings)} disagreement(s) between derivation "
            "routes; refusing to emit or report a single-route table",
            file=sys.stderr,
        )
        return 2

    if args.emit_c is not None:
        args.emit_c.write_text(emit_c(tables, names), encoding="utf-8")
        print(f"wrote {args.emit_c} ({tables.handler_bound} rows)")
        return 0

    if args.check is not None:
        want = emit_c(tables, names)
        got = args.check.read_text(encoding="utf-8")
        if want == got:
            print(f"{args.check}: matches a fresh recovery from {args.xbe}")
            return 0
        print(f"{args.check}: DIFFERS from a fresh recovery from {args.xbe}")
        return 1

    _report(tables, names, image)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
