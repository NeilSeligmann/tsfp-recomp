# SPDX-License-Identifier: GPL-3.0-or-later
"""Recover the XDK call surface from a real XBE, and generate a stub table.

The Xbox has no user-mode graphics driver: `D3D8.lib` is statically linked into
the executable and *is* the driver. So the surface to replace is not an import
list but the set of addresses in a linked XDK section that game code calls.

MEASURED on TimeSplitters: Future Perfect retail `default.xbe` -- **236 distinct
functions across 831 transfer sites**, of which D3D is 83/358 and DirectSound
41/83. Per-section figures are printed by the CLI; transcribing them into prose
is how they went stale twice, so regenerate rather than trust a number here.

WHY ADDRESSES RATHER THAN NAMES. The XBE carries no names for these; `.XTLID`
names 142 of the 236, and only 11 of the 83 D3D entries. An address plus a
site count is what we actually know, and the count matters because it ranks the
work: the function called from 40 sites is worth implementing before the one
called from 1.

THE COUNTS WERE NEVER AN UPPER BOUND, AND SAYING SO WAS THE BUG
===============================================================

Every previous revision of this module warned at length that its counts were
*upper bounds*, because it found call sites by sweeping `.text` for the bytes
`E8`/`E9` and those bytes occur inside other instructions and inside data. The
warning was tested and is almost entirely wrong, in the expensive direction.

TEST 1, the entry-point test. A real call target is the START of a function, so
its bytes begin a function and the address before it ends one. Over the 239 rows
the byte sweep produced, classifying the bytes AT each target and the bytes
BEFORE it:

    rows                                                239
      `.XTLID`-named (a name is a symbol start address)  142   all entry-like
      unnamed but entry-like by its own bytes             93
      neither -- suspect                                   4   1.7%

    preceded by `ret`/`jmp`/`int3`/alignment padding      234   97.9%

So 235 of 239 rows are function entries on their own evidence, and the 4 that are
not hold 4 sites out of 819, i.e. **0.5% of sites**. The warning was real but
two orders of magnitude smaller than its prominence implied.

TEST 2, decode instead of sweep, which settles it. Decoding `.text` properly
removes **all 4** suspect rows, plus 2 more whose bytes were entry-like by luck,
for 6 false-positive targets -- and in the same pass it RECOVERS 3 real targets
and 12 real sites the sweep had missed. The sweep's `offset += 5` skip, added to
stop overlapping scans double-counting, steps over an `E8` that begins a genuine
instruction whenever a false `E8` precedes it by less than five bytes. So the
naive sweep was wrong in BOTH directions at once, its under-count was larger than
its over-count, and the fix for both is the same: decode.

After decoding, the entry-point test leaves **0 suspect rows of 236**. The upper
bound language is therefore retired rather than repeated: it described a defect
this module no longer has, and it discouraged exactly the use the table is for.

WHAT ELSE WAS LOOKED FOR, AND WHAT WAS FOUND
============================================

A `rel32` sweep sees only direct transfers, so four indirect paths were added and
measured. Three of them fire ZERO times on retail, and that is a result worth
keeping rather than a reason to delete the code -- an absent path is only known to
be absent because something looked. Each is exercised by a synthetic fixture in
`tests/test_gen_d3d8_surface.py`, so "fires 0 times" is a measurement about this
executable and not an untested code path.

`KIND_SLOT`, `call`/`jmp dword ptr [disp32]`. The import-table shape: the slot
holds the target. 324 such sites exist in `.text` and **not one slot holds an XDK
address**. 289 of them go through the xboxkrnl thunk table at 0x475780, which is
a different boundary and is `tools/lift/callsites.py`'s business, not ours.

`KIND_TABLE`, `call`/`jmp dword ptr [index*scale + disp32]`. A dispatch table with
a constant base is fully resolvable: read its entries out of the image. 710 such
sites exist in `.text` and **no table holds an XDK address**.

`KIND_REGISTER`, an XDK address loaded into a register and then called --
`mov reg, imm32` or `mov reg, dword ptr [disp32]`, then `call`/`jmp reg`. This is
the shape that cost `tools/lift/callsites.py` all twelve of ordinal 24's sites, so
it was worth checking. 13 instructions in `.text` load an immediate that lands in
an XDK section and **all 13 are false positives**: `0x400000` (a bit mask, loaded
by `test ebx, 0x400000`), `0x3e3ac4`/`0x3e3ad4` (D3D's own globals, used as loop
sentinels), and three more with no evidence of being code. None is reachable, none
is `.XTLID`-named, none is ever called. They are rejected by `is_entry_point`;
see "the gate" below, because this is what it is for.

`KIND_THUNK` and `KIND_VTABLE`, and this one is real
----------------------------------------------------

COM-shaped vtable dispatch WAS the suspected explanation for `IDirectSoundBuffer`
looking undersampled, and the mechanism does exist -- but not where it was looked
for first. No XDK function address appears in any vtable: each of the 37 named
`IDirectSound*` addresses occurs exactly ONCE in the whole 6,270,976-byte file,
and that occurrence is inside `.XTLID` itself. DSOUND does have vtables in
`.rdata`, and they are referenced only from DSOUND's own constructors.

What game vtables hold is the address of a stub in the game's own `.text`:

    0x003d193d  lea ecx, [ebp-0x418]     <- `this` adjustment for a base subobject
    0x003d1943  jmp 0x003fc8a8           <- tail jump into XGRPH

63 of these exist. Each is a C++ adjustor thunk for a virtual override that tail
-calls into XGRPH or DSOUND, each begins at a verified function entry, and each is
referenced exactly once as a 4-byte-aligned dword, all 63 of them in `.rdata`,
i.e. one vtable slot apiece. They are reached by `call dword ptr [reg + disp]`.

THE HONEST SIZE OF THAT WIN IS ZERO SITES, and it has to be said plainly because
the obvious thing to do with 63 newly-understood thunks is to add 63 to the
counts. Not one of the 63 is the target of a `call rel32` or a `jmp rel32`; they
are reached only through their vtable slot. And the sweep already attributed each
thunk's own outgoing `jmp rel32` as one site of its XDK target. One stub maps to
exactly one vtable slot, so "one site per stub" is already the right magnitude and
counting the slot as well would double it. `vtable_refs` therefore travels beside
`total_sites` rather than inside it: 14 XDK functions are known to be reached by
virtual dispatch, and `0x003ef236` sits behind 25 vtable slots, which is ranking
information the table should carry -- but it is not 25 more call instructions.

WHY DISPLACEMENT MATCHING IS NOT USED, measured rather than assumed. The tempting
rule for the 1,134 `call dword ptr [reg + disp]` sites is to match `disp/4` against
a slot index in a discovered vtable. It carries no information: of 197 candidate
vtables in this image, 197 have an XDK-reaching pointer at slot 1, 116 at slot 2
and 104 at slot 3, against 136/192/207 call sites at those displacements. Even the
best slot leaves 15 candidates. `ambiguous_vtable_sites` counts those sites and
attributes none of them, because an ambiguous site would otherwise be added to
every candidate at once and inflate the whole table.

THE GATE, and why an indirect form needs one
============================================

The direct forms need no filtering: a decoded `call rel32` whose displacement
lands in an XDK section IS a call into the XDK. Every indirect form instead rests
on a weaker premise -- "this dword/immediate lands inside an XDK section, so it
must be a function there" -- and that premise is false far more often than it is
true, because **the XDK sections hold data as well as code**. `mov ebx, dword ptr
[0x3e3ab8]` reads one of D3D's own globals.

So every indirect attribution is gated on `is_entry_point`: the bytes must decode,
and the address before them must end a function or be alignment padding. Measured
against the population it exists to reject -- the 77 places where `.text` names an
XDK-range address as plain data via `push imm32` or `mov [mem], imm32`, 26 distinct
values -- the gate rejects **26 of 26**. The four that `.XTLID` can even name are
`XDEVICE_TYPE_*_TABLE`, which are data symbols. Ungated, those 77 references would
become 77 call sites and 26 functions, with zero true positives among them.

And the gate reproduces the direct measurement it did not take part in: of the 233
rows a decoded sweep confirms, `is_entry_point` accepts **233**, and of the 6 the
decode exposed as false positives it accepts **0**.

FALSE-POSITIVE NULL. `false_positive_null` asks how often the gate fires on an
address that is not a recovered entry point, EXHAUSTIVELY over every byte of every
XDK section, no sampling and so no seed: **13,832 of 674,897 = 2.05%** (D3D 4.57%,
XMV 0.67%). Read it as the cost of the gate's permissiveness: an indirect form
pointing at a uniformly random XDK address would be wrongly accepted about one
time in fifty. That is the honest reason the three zero-firing forms are reported
as zero and not quietly trusted -- at this null, a handful of fires would have been
indistinguishable from noise, and zero fires is the only unambiguous reading
available.

DECODING IS LINEAR AND MUST RESYNCHRONISE. `.text` interleaves jump tables,
alignment and read-only data with code. Capstone's `disasm` stops at the first
undecodable byte and silently returns a SHORT iterator -- a kernel scan in this
project reported 0 call sites for exactly that reason. `decode_section` mirrors the
chunked, resynchronising sweep in `tools/codediff/normalise.py`, which is also the
cross-check: on retail `.text` both yield 1,154,998 instructions and the same 132
undecodable offsets, and `test_decode_section_agrees_with_normalise_text` asserts
that agreement rather than leaving it to a comment. It is not simply called,
because `normalise_text` returns operands already blanked and this module needs to
read the displacements it blanks.

ORIGINS. The scan's origin defaults to `.text` alone, because the surface this
table describes is the boundary the GAME crosses. Every other executable section
in the image is itself an XDK library, so widening the origin set measures
XDK-to-XDK traffic instead -- a different and much larger question. `--origins`
asks for it explicitly; the generated table is always the `.text` surface.
"""

from __future__ import annotations

import argparse
import struct
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

import capstone
from capstone import x86 as cs_x86

from tools.codediff.normalise import CHUNK_BYTES, MAX_INSN_BYTES
from tools.xbe import Xbe, XbeSection, parse_xbe

#: XDK libraries linked into TSFP. `.text` is the game; these are the surface.
XDK_SECTIONS = ("D3D", "XGRPH", "DSOUND", "XONLINE", "XNET", "XMV", "XPP", "DOLBY")

#: Where control transfers are scanned FROM. `.text` is the game's own code and the
#: only non-XDK executable section in the image, so this is the game's boundary.
DEFAULT_ORIGINS = (".text",)

OPCODE_CALL_REL32 = 0xE8
OPCODE_JMP_REL32 = 0xE9

#: A decoded `call`/`jmp` whose displacement is IP-relative. The only direct path.
KIND_DIRECT = "direct"
#: `call`/`jmp dword ptr [disp32]` -- the slot at a constant address holds the target.
KIND_SLOT = "slot"
#: `call`/`jmp dword ptr [index*scale + disp32]` -- a dispatch table at a constant base.
KIND_TABLE = "table"
#: An XDK address loaded into a register, then called or jumped through.
KIND_REGISTER = "register"
#: A `call`/`jmp rel32` reaching an XDK function through a stub in game `.text`.
KIND_THUNK = "thunk"
#: Every attribution form, in the order a report should list them.
ATTRIBUTION_KINDS = (KIND_DIRECT, KIND_SLOT, KIND_TABLE, KIND_REGISTER, KIND_THUNK)

#: Bytes that end a function, as the byte immediately before a candidate entry.
#: `0xC3` is `ret`; `0xC2` is `ret imm16` and so sits three bytes back.
RET_NEAR = 0xC3
RET_IMM16 = 0xC2
JMP_REL8 = 0xEB
#: Alignment padding. MSVC pads with `int3` between functions and `nop` within them.
PADDING_BYTES = frozenset({0x90, 0xCC})
#: The same padding, as capstone decodes it. A padding byte is not a function start.
PADDING_MNEMONICS = frozenset({"int3", "nop"})

#: Instructions that must decode cleanly at a candidate entry before it is believed.
#: Three is enough to reject the misdecoded bytes the byte sweep produced (`sldt word
#: ptr [eax]` and friends) without demanding that a short function be long.
ENTRY_DECODE_INSNS = 3

#: How far back to look for the terminator or padding that ends the previous function.
ENTRY_LOOKBEHIND = 8

#: Most instructions a stub may contain before the tail `jmp`. The real population on
#: retail is two -- `lea ecx, [ebp-N]` then `jmp` -- and a longer body is a function
#: that happens to tail-call, whose callers are NOT sites of the tail target.
MAX_THUNK_INSNS = 4

#: Bytes per dispatch-table entry, and the alignment a vtable reference must satisfy.
ENTRY_SIZE = 4

#: Entries read from a constant-base dispatch table before giving up. The table's real
#: length is not encoded in the `call`, so the scan stops at the first entry that is
#: not a gated XDK entry point and is capped here regardless.
TABLE_SCAN_LIMIT = 64

#: Registers a `__stdcall`/`__cdecl` callee may destroy, so a value held in one of them
#: does not survive an intervening `call`. Same rule, and the same reason, as
#: `tools/lift/callsites.py`: the bindings that matter live in callee-saved registers.
VOLATILE_REGISTERS = frozenset({"eax", "ecx", "edx"})

#: Instructions to follow a register binding forward. Beyond this the binding is
#: dropped rather than assumed to hold.
REGISTER_TRACK_INSNS = 64


@dataclass
class CallTarget:
    """One address in an XDK section that game code transfers control to."""

    address: int
    section: str
    call_sites: int = 0
    jump_sites: int = 0
    name: str | None = None
    #: Attribution form -> how many sites that form contributed. Sums to `total_sites`,
    #: so a surprising count can be traced to the rule that produced it.
    kinds: dict[str, int] = field(default_factory=dict)
    #: 4-byte-aligned dwords in the image holding the address of a `.text` stub that
    #: tail-jumps here: one C++ vtable slot each. Deliberately NOT part of
    #: `total_sites` -- a slot is a dispatch entry point, not a call instruction, and
    #: the stub's own tail `jmp` is already counted. See the module docstring.
    vtable_refs: int = 0

    @property
    def total_sites(self) -> int:
        return self.call_sites + self.jump_sites

    def record(self, kind: str, *, is_call: bool) -> None:
        """Attribute one transfer site to this target."""
        if is_call:
            self.call_sites += 1
        else:
            self.jump_sites += 1
        self.kinds[kind] = self.kinds.get(kind, 0) + 1


@dataclass
class Thunk:
    """A stub in game `.text` whose tail `jmp` leaves for an XDK section.

    Its own `jmp` is already a site of `target`. What this object adds is a second
    way in: `callers`, the `call`/`jmp rel32` sites that reach the STUB (zero on
    retail), and `vtable_refs`, the vtable slots that hold its address (63 on
    retail, one apiece).
    """

    address: int
    target: int
    section: str
    callers: int = 0
    vtable_refs: int = 0


@dataclass
class Surface:
    """The recovered call surface, grouped by XDK section."""

    targets: dict[int, CallTarget] = field(default_factory=dict)
    #: Stub address -> what it is, for every adjustor thunk found in an origin section.
    thunks: dict[int, Thunk] = field(default_factory=dict)
    #: `call dword ptr [reg + disp]` sites, which reach a vtable slot through a
    #: register this scan cannot resolve. Counted and attributed to NOTHING: see the
    #: module docstring on why displacement matching carries no information.
    ambiguous_vtable_sites: int = 0
    #: Offsets in each origin section where decoding failed, for the resync report.
    undecodable: dict[str, int] = field(default_factory=dict)

    def by_section(self) -> dict[str, list[CallTarget]]:
        grouped: dict[str, list[CallTarget]] = {}
        for target in self.targets.values():
            grouped.setdefault(target.section, []).append(target)
        for entries in grouped.values():
            entries.sort(key=lambda t: (-t.total_sites, t.address))
        return dict(sorted(grouped.items()))

    def summary(self) -> dict[str, tuple[int, int]]:
        """section -> (distinct functions, total transfer sites)."""
        return {
            section: (len(entries), sum(t.total_sites for t in entries))
            for section, entries in self.by_section().items()
        }

    def kind_totals(self) -> dict[str, int]:
        """Attribution form -> sites it contributed, across every section."""
        totals = dict.fromkeys(ATTRIBUTION_KINDS, 0)
        for target in self.targets.values():
            for kind, count in target.kinds.items():
                totals[kind] = totals.get(kind, 0) + count
        return totals

    def vtable_ref_totals(self) -> dict[str, int]:
        """section -> vtable slots reaching it through a `.text` adjustor thunk."""
        totals: dict[str, int] = {}
        for target in self.targets.values():
            if target.vtable_refs:
                totals[target.section] = totals.get(target.section, 0) + target.vtable_refs
        return dict(sorted(totals.items()))


@dataclass
class NullResult:
    """How often `is_entry_point` fires where no entry point was recovered.

    Exhaustive over a section's initialised bytes, so it reproduces with no seed.
    """

    section: str
    tested: int
    fired: int
    excluded: int

    @property
    def rate(self) -> float:
        return self.fired / self.tested if self.tested else 0.0


class Image:
    """Random access to an XBE by virtual address, plus section lookup.

    Holds no decoded state: every method is a function of the bytes and the section
    table, so the same image always answers the same way.
    """

    def __init__(self, data: bytes, xbe: Xbe) -> None:
        self.data = data
        self.xbe = xbe
        self._decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def offset(self, va: int) -> int | None:
        """File offset of `va`, or None when it is outside the initialised bytes."""
        offset = self.xbe.va_to_offset(va)
        if offset is None or not 0 <= offset < len(self.data):
            return None
        return offset

    def read_u32(self, va: int) -> int | None:
        offset = self.offset(va)
        if offset is None or offset + 4 > len(self.data):
            return None
        (value,) = struct.unpack_from("<I", self.data, offset)
        return value

    def section_body(self, section: XbeSection) -> bytes:
        """A section's initialised bytes, clipped to the file."""
        start = section.raw_addr
        end = min(start + section.raw_size, len(self.data))
        if start >= len(self.data) or end <= start:
            return b""
        return self.data[start:end]

    def decodes_at(self, va: int, count: int = ENTRY_DECODE_INSNS) -> bool:
        """Do at least `count` consecutive instructions decode starting at `va`?

        Consecutive is the point: capstone will happily skip forward, so each
        instruction is required to begin exactly where the previous one ended.
        """
        offset = self.offset(va)
        if offset is None:
            return False
        window = self.data[offset : offset + MAX_INSN_BYTES * count]
        position = 0
        decoded = 0
        for insn in self._decoder.disasm(window, va):
            if insn.address - va != position:
                break
            position += insn.size
            decoded += 1
            if decoded >= count:
                return True
        return False

    def preceded_by_function_end(self, va: int) -> bool:
        """Do the bytes before `va` end a function, or pad up to its alignment?

        Byte-level on purpose. The alternative, trusting a linear decode of the
        preceding bytes, needs a correctly-anchored instruction stream, and inside
        an XDK section -- which interleaves code and data -- there is none to be
        had. The cost is that a `0xCC` which is really some instruction's `disp8`
        reads as padding; `false_positive_null` is what puts a number on that, and
        `find_thunks` uses the decoded predecessor instead precisely because in an
        origin section the decoded stream does exist.
        """
        offset = self.offset(va)
        if offset is None or offset < ENTRY_LOOKBEHIND:
            return False
        window = self.data[offset - ENTRY_LOOKBEHIND : offset]
        end = len(window)
        while end > 0 and window[end - 1] in PADDING_BYTES:
            end -= 1
        if end != len(window):
            return True
        if window[end - 1] == RET_NEAR:
            return True
        if end >= 3 and window[end - 3] == RET_IMM16:
            return True
        if end >= 5 and window[end - 5] == OPCODE_JMP_REL32:
            return True
        if end >= 2 and window[end - 2] == JMP_REL8:
            return True
        return False

    def is_entry_point(self, va: int) -> bool:
        """Does `va` look like the start of a function?

        Both halves are needed and neither is sufficient. The bytes must decode, which
        rejects a misdecode landing mid-instruction, and the address before them must
        end a function, which rejects a data value that merely happens to disassemble.
        """
        return self.preceded_by_function_end(va) and self.decodes_at(va)


def _section_containing(sections: list[XbeSection], address: int) -> XbeSection | None:
    for section in sections:
        if section.virtual_addr <= address < section.virtual_addr + section.virtual_size:
            return section
    return None


def decode_section(data: bytes, base_va: int) -> tuple[list[capstone.CsInsn], list[int]]:
    """Decode a code section linearly, resynchronising past undecodable bytes.

    Returns `(instructions, undecodable_offsets)`, both ascending.

    THE ALGORITHM IS `tools/codediff/normalise.py`'s, deliberately: chunked so a
    resync re-decodes a bounded slice, an instruction crossing a non-final chunk's
    tail left for the next chunk rather than trusted, and one byte of advance when a
    chunk yields nothing. `normalise_text` is not simply called because it returns
    operands already blanked and this module needs to read the very displacements it
    blanks. `test_decode_section_agrees_with_normalise_text` pins the two together so
    the duplication cannot drift silently; on retail `.text` both produce 1,154,998
    instructions and the same 132 undecodable offsets.
    """
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    instructions: list[capstone.CsInsn] = []
    undecodable: list[int] = []
    total = len(data)
    offset = 0
    while offset < total:
        end = min(offset + CHUNK_BYTES, total)
        limit = end if end == total else end - MAX_INSN_BYTES
        progress = offset
        for insn in md.disasm(data[offset:end], base_va + offset):
            insn_offset = insn.address - base_va
            if insn_offset + insn.size > limit:
                break
            instructions.append(insn)
            progress = insn_offset + insn.size
        if progress == offset:
            undecodable.append(offset)
            offset += 1
        else:
            offset = progress
    return instructions, undecodable


def _relative_target(insn: capstone.CsInsn) -> int | None:
    """The absolute target of an IP-relative `call`/`jmp`, or None."""
    if insn.mnemonic not in ("call", "jmp"):
        return None
    if capstone.CS_GRP_BRANCH_RELATIVE not in insn.groups:
        return None
    operands = insn.operands
    if len(operands) != 1 or operands[0].type != cs_x86.X86_OP_IMM:
        return None
    return operands[0].imm


def _constant_memory_operand(insn: capstone.CsInsn) -> tuple[int, bool] | None:
    """`(displacement, is_indexed)` for a memory operand with a constant base.

    `[disp32]` gives `(disp, False)` -- the slot itself. `[index*scale + disp32]`
    gives `(disp, True)` -- a dispatch table based at `disp`. A form with a BASE
    register gives None: its effective address depends on a runtime value, so no
    dword can be read for it, which is exactly the vtable case this module refuses
    to guess at.
    """
    if insn.mnemonic not in ("call", "jmp"):
        return None
    operands = insn.operands
    if len(operands) != 1 or operands[0].type != cs_x86.X86_OP_MEM:
        return None
    mem = operands[0].mem
    if mem.base != 0 or mem.segment != 0:
        return None
    return mem.disp, mem.index != 0


def _register_operand_name(insn: capstone.CsInsn) -> str | None:
    """The register a `call`/`jmp reg` dispatches through, or None."""
    if insn.mnemonic not in ("call", "jmp"):
        return None
    operands = insn.operands
    if len(operands) != 1 or operands[0].type != cs_x86.X86_OP_REG:
        return None
    return insn.reg_name(operands[0].reg)


def _xdk_value_load(insn: capstone.CsInsn, image: Image) -> tuple[str, int] | None:
    """`(register, value)` when this instruction loads a plausible XDK code address.

    Two shapes, both of which `tools/lift/callsites.py` had to learn the hard way:
    `mov reg, imm32` with the address as a literal, and `mov reg, dword ptr [disp32]`
    where the address is read out of a constant slot. The VALUE is returned unfiltered;
    gating it on `is_entry_point` is the caller's job, so the gate lives in one place.
    """
    if insn.mnemonic != "mov":
        return None
    operands = insn.operands
    if len(operands) != 2 or operands[0].type != cs_x86.X86_OP_REG:
        return None
    register = insn.reg_name(operands[0].reg)
    source = operands[1]
    if source.type == cs_x86.X86_OP_IMM:
        return register, source.imm
    if source.type == cs_x86.X86_OP_MEM:
        mem = source.mem
        if mem.base != 0 or mem.index != 0 or mem.segment != 0:
            return None
        value = image.read_u32(mem.disp)
        if value is None:
            return None
        return register, value
    return None


def _written_registers(insn: capstone.CsInsn) -> set[str]:
    """Registers this instruction writes, by capstone's own operand analysis."""
    _, written = insn.regs_access()
    return {insn.reg_name(register) for register in written}


def find_thunks(
    image: Image,
    instructions: list[capstone.CsInsn],
    xdk_sections: list[XbeSection],
) -> dict[int, Thunk]:
    """Stubs in an origin section whose tail `jmp` leaves for an XDK section.

    A stub is a SHORT function -- at most `MAX_THUNK_INSNS` instructions, no `call`
    among them -- that begins at a function entry and ends in a `jmp` into the XDK.
    On retail these are C++ adjustor thunks: `lea ecx, [ebp-N]` then `jmp`, the
    `this` fixup for a virtual override that tail-calls an XDK routine.

    THE ENTRY TEST USES THE DECODED PREDECESSOR, not the preceding byte, and the
    difference is not theoretical: at 0x003d1938 the byte before the `jmp` is `0xCC`,
    which a byte test reads as `int3` padding and therefore as a function boundary.
    It is the `disp8` of `lea ecx, [ebp-0x34]`. Two stubs are wrongly admitted that
    way. Inside an origin section the decoded instruction stream exists, so the
    predecessor can simply be looked up instead of inferred.

    The body is required to hold no `call` because a stub that calls is a function,
    and promoting a real function's callers into sites of whatever it tail-jumps to
    is precisely the error that makes a count untrustworthy.
    """
    # Index the stream by where each instruction ENDS, so a candidate's decoded
    # predecessor is an O(1) lookup rather than a backward re-decode.
    ends_at: dict[int, capstone.CsInsn] = {insn.address + insn.size: insn for insn in instructions}

    def starts_a_function(va: int) -> bool:
        previous = ends_at.get(va)
        if previous is None:
            # Nothing decoded ends here: either the section's first instruction or a
            # resync boundary. Padding before it is still good evidence.
            return image.preceded_by_function_end(va)
        if previous.mnemonic in ("ret", "retf", "int3", "jmp"):
            return True
        return previous.mnemonic == "nop"

    thunks: dict[int, Thunk] = {}
    for position, insn in enumerate(instructions):
        if insn.mnemonic in PADDING_MNEMONICS:
            # Padding is not a function start, and skipping it is not cosmetic: every
            # `int3` in a run makes the byte after it look like a boundary, so a stub
            # preceded by eight bytes of padding would otherwise be found nine times,
            # at the stub and at each of the padding bytes within MAX_THUNK_INSNS.
            continue
        if not starts_a_function(insn.address):
            continue
        expected_va = insn.address
        for step in range(position, min(position + MAX_THUNK_INSNS, len(instructions))):
            body = instructions[step]
            if body.address != expected_va:
                break  # a resync gap: the body is not contiguous, so it is not a stub
            expected_va = body.address + body.size
            if body.mnemonic == "call":
                break  # a stub that calls is a function; its callers are not our sites
            if body.mnemonic != "jmp":
                continue
            target = _relative_target(body)
            if target is None:
                break  # an indirect tail jump this scan cannot resolve
            section = _section_containing(xdk_sections, target)
            if section is not None and image.is_entry_point(target):
                thunks[insn.address] = Thunk(
                    address=insn.address, target=target, section=section.name
                )
            break
    return thunks


def count_aligned_references(image: Image, wanted: set[int]) -> dict[int, int]:
    """How often each wanted address appears as a 4-byte-aligned dword in the image.

    This is how a vtable slot is found without having to decide what a vtable is:
    the address of an adjustor thunk is not an integer any computation produces, so
    every aligned occurrence of one is a slot holding it. Scanned over every
    section's initialised bytes, in one pass per section rather than per address.
    """
    counts = dict.fromkeys(wanted, 0)
    if not wanted:
        return counts
    for section in image.xbe.sections:
        body = image.section_body(section)
        base = section.virtual_addr
        skew = -base % ENTRY_SIZE
        for offset in range(skew, len(body) - ENTRY_SIZE + 1, ENTRY_SIZE):
            (value,) = struct.unpack_from("<I", body, offset)
            if value in counts:
                counts[value] += 1
    return counts


def recover_surface(
    data: bytes,
    xbe: Xbe,
    *,
    sections: tuple[str, ...] = XDK_SECTIONS,
    origins: tuple[str, ...] = DEFAULT_ORIGINS,
    extended: bool = True,
) -> Surface:
    """Scan the origin sections for control transfers into the named XDK sections.

    `extended=False` restricts the scan to direct `call`/`jmp rel32`, which is what
    every indirect form is measured against. It still DECODES: the naive byte sweep
    it replaces is not reachable from here, because it was wrong in both directions
    and keeping it would invite a comparison against a broken baseline.
    """
    wanted = set(sections)
    xdk_sections = [s for s in xbe.sections if s.name in wanted]
    origin_sections = [s for s in xbe.sections if s.name in set(origins)]
    if not xdk_sections or not origin_sections:
        return Surface()

    image = Image(data, xbe)
    surface = Surface()

    def target_for(address: int, section: str) -> CallTarget:
        entry = surface.targets.get(address)
        if entry is None:
            entry = CallTarget(address=address, section=section)
            surface.targets[address] = entry
        return entry

    def attribute(address: int, kind: str, *, is_call: bool) -> bool:
        """Record a site, if `address` is a gated XDK entry point."""
        section = _section_containing(xdk_sections, address)
        if section is None:
            return False
        if kind is not KIND_DIRECT and not image.is_entry_point(address):
            # Every indirect form rests on "a dword landing in an XDK section is a
            # function there", which is false far more often than true because those
            # sections hold data too. See the module docstring on the gate.
            return False
        target_for(address, section.name).record(kind, is_call=is_call)
        return True

    for origin in origin_sections:
        body = image.section_body(origin)
        if not body:
            continue
        instructions, undecodable = decode_section(body, origin.virtual_addr)
        surface.undecodable[origin.name] = len(undecodable)

        for insn in instructions:
            is_call = insn.mnemonic == "call"
            if insn.mnemonic not in ("call", "jmp"):
                continue

            target = _relative_target(insn)
            if target is not None:
                attribute(target, KIND_DIRECT, is_call=is_call)
                continue
            if not extended:
                continue

            constant = _constant_memory_operand(insn)
            if constant is not None:
                displacement, indexed = constant
                if not indexed:
                    value = image.read_u32(displacement)
                    if value is not None:
                        attribute(value, KIND_SLOT, is_call=is_call)
                else:
                    # A constant table base is fully resolvable: read its entries.
                    # Stop at the first that is not a gated XDK entry point, since
                    # nothing in the instruction says how long the table is.
                    for step in range(TABLE_SCAN_LIMIT):
                        value = image.read_u32(displacement + ENTRY_SIZE * step)
                        if value is None or not attribute(value, KIND_TABLE, is_call=is_call):
                            break
                continue

            if insn.operands and insn.operands[0].type == cs_x86.X86_OP_MEM:
                # `call dword ptr [reg + disp]`: a vtable slot reached through a
                # register. Counted, attributed to nothing. See the module docstring.
                surface.ambiguous_vtable_sites += 1

        if extended:
            _attribute_register_calls(image, instructions, xdk_sections, attribute)
            _attribute_thunks(image, instructions, xdk_sections, surface, target_for)

    return surface


#: `(address, kind, *, is_call) -> was it attributed`. `recover_surface` closes over the
#: surface being built and hands this to the per-form helpers, so the entry-point gate
#: and the section test live in exactly one place. Spelled `...` because `Callable`
#: cannot express the keyword-only argument.
Attribute = Callable[..., bool]
#: `(address, section) -> the CallTarget`, created on first use.
TargetFor = Callable[[int, str], "CallTarget"]


def _attribute_register_calls(
    image: Image,
    instructions: list[capstone.CsInsn],
    xdk_sections: list[XbeSection],
    attribute: Attribute,
) -> None:
    """Attribute `mov reg, <xdk address>` ... `call reg` pairs.

    The binding is followed forward by FALL-THROUGH and dies at the first of: a write
    to the register, an intervening `call` when the register is one a callee may
    destroy, an unconditional transfer of control, or `REGISTER_TRACK_INSNS`. A
    CONDITIONAL branch does not end it, the same choice and for the same reason as
    `tools/lift/callsites.py`: the sites that hide from a naive scan are exactly the
    ones behind a branch. The cost is the same too -- a join can admit a path on which
    the register holds something else -- so a register-attributed site is one that CAN
    reach the target rather than one that always does.
    """
    for position, insn in enumerate(instructions):
        load = _xdk_value_load(insn, image)
        if load is None:
            continue
        register, value = load
        # Pre-filtered before the forward walk begins: `.text` holds hundreds of
        # thousands of `mov reg, <something>` and only a handful name an XDK address.
        if _section_containing(xdk_sections, value) is None:
            continue
        for step in range(
            position + 1, min(position + 1 + REGISTER_TRACK_INSNS, len(instructions))
        ):
            following = instructions[step]
            dispatch = _register_operand_name(following)
            if dispatch == register:
                attribute(value, KIND_REGISTER, is_call=following.mnemonic == "call")
                break
            if following.mnemonic == "call" and register in VOLATILE_REGISTERS:
                break
            if register in _written_registers(following):
                break
            if following.mnemonic in ("jmp", "ret", "retf"):
                break


def _attribute_thunks(
    image: Image,
    instructions: list[capstone.CsInsn],
    xdk_sections: list[XbeSection],
    surface: Surface,
    target_for: TargetFor,
) -> None:
    """Find `.text` adjustor thunks and attribute what reaches them.

    Two ways in, counted differently and on purpose:

    * a `call`/`jmp rel32` to the STUB is a real call instruction that the direct
      scan threw away, because the stub is in `.text` and not in an XDK section.
      It is attributed as a `KIND_THUNK` site. On retail there are none.
    * a vtable slot holding the stub's address is a dispatch entry point, not an
      instruction, and the stub's own tail `jmp` is already counted as a site. It is
      recorded in `vtable_refs` and left OUT of `total_sites`; adding it would double
      the magnitude of a path that is already represented.
    """
    thunks = find_thunks(image, instructions, xdk_sections)
    if not thunks:
        return
    surface.thunks.update(thunks)

    for insn in instructions:
        target = _relative_target(insn)
        thunk = thunks.get(target) if target is not None else None
        if thunk is not None:
            thunk.callers += 1
            target_for(thunk.target, thunk.section).record(
                KIND_THUNK, is_call=insn.mnemonic == "call"
            )

    references = count_aligned_references(image, set(thunks))
    for address, thunk in thunks.items():
        thunk.vtable_refs = references.get(address, 0)
        entry = surface.targets.get(thunk.target)
        if entry is not None:
            entry.vtable_refs += thunk.vtable_refs


def false_positive_null(
    image: Image,
    section_name: str,
    *,
    recovered: frozenset[int] = frozenset(),
) -> NullResult:
    """How often `is_entry_point` fires at an address that is not a recovered entry.

    EXHAUSTIVE, not sampled: every byte address in the section's initialised bytes is
    tested, so the result reproduces with no seed. `recovered` holds the addresses the
    scan actually attributed; they are skipped and counted in `excluded`, because a
    real entry point passing the gate is the signal and leaving it in the denominator
    would bias the null upwards by the very thing being measured.

    EVERY byte, not every 4-byte-aligned one. x86 functions are not required to be
    aligned, the gate is offered arbitrary dwords read out of slots and tables, and
    testing only aligned addresses would quietly measure a stricter rule than the one
    `recover_surface` applies.

    `is_entry_point` here is the SAME method the attribution path calls. Two copies of
    the rule would decouple and the null would stop being the null for anything.
    """
    section = image.xbe.section_by_name(section_name)
    if section is None:
        return NullResult(section=section_name, tested=0, fired=0, excluded=0)
    body = image.section_body(section)
    if not body:
        return NullResult(section=section_name, tested=0, fired=0, excluded=0)

    tested = fired = excluded = 0
    for va in range(section.virtual_addr, section.virtual_addr + len(body)):
        if va in recovered:
            excluded += 1
            continue
        tested += 1
        if image.is_entry_point(va):
            fired += 1
    return NullResult(section=section.name, tested=tested, fired=fired, excluded=excluded)


def entry_point_audit(image: Image, surface: Surface) -> dict[str, int]:
    """How many recovered rows survive the entry-point test.

    The measurement that retired the upper-bound warning. A row is `suspect` when it
    is neither `.XTLID`-named nor entry-like on its own bytes; `named` and `entry_like`
    are the two independent halves, reported separately so a change in either is
    visible rather than averaged away.
    """
    named = entry_like = preceded = suspect = 0
    for target in surface.targets.values():
        is_named = target.name is not None
        is_entry = image.is_entry_point(target.address)
        named += is_named
        entry_like += is_entry
        preceded += image.preceded_by_function_end(target.address)
        if not is_named and not is_entry:
            suspect += 1
    return {
        "rows": len(surface.targets),
        "named": named,
        "entry_like": entry_like,
        "preceded_by_function_end": preceded,
        "suspect": suspect,
    }


def apply_xtlid_names(surface: Surface, names: dict[int, str]) -> int:
    """Attach known XDK names to targets. Returns how many were named."""
    applied = 0
    for address, name in names.items():
        target = surface.targets.get(address)
        if target is not None:
            target.name = name
            applied += 1
    return applied


HEADER_TEMPLATE = """\
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GENERATED by tools/gen_d3d8_surface.py. Do not edit; regenerate instead.
 *
 * The XDK call surface recovered from a real executable: every address in a
 * linked XDK section that game code transfers control to, with how many sites do
 * so.
 *
 * The Xbox has no user-mode graphics driver -- D3D8.lib is statically linked and
 * IS the driver -- so this, not an import list, is the boundary to replace.
 *
 * Keyed by address because the executable carries no names for these. Where
 * .XTLID supplies one it is recorded as a comment. The site count ranks the work:
 * implement what is called from forty places before what is called from one.
 *
 * Counts are MEASURED, not upper bounds. Sites are found by DECODING every
 * instruction in the game's .text, resynchronising past data, and are not a byte
 * scan for E8/E9 -- which this generator used to do, and which was wrong in both
 * directions at once: 6 of its 239 rows were misdecodes and it also missed 3 real
 * targets and 12 real sites. Every recovered row passes an independent
 * entry-point test: its bytes begin a function and the address before it ends one.
 * Indirect forms are additionally gated on that same test, whose exhaustive
 * false-positive null over the XDK sections is 2.05%%.
 */

#ifndef TSFP_XBOX_XDK_SURFACE_H
#define TSFP_XBOX_XDK_SURFACE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t address;
    const char *section;
    const char *name; /* NULL when unknown */
    uint32_t sites;
} xdk_surface_entry;

#define XDK_SURFACE_COUNT %(count)d

extern const xdk_surface_entry xdk_surface[XDK_SURFACE_COUNT];

/** Look up a surface entry by address, or NULL. */
const xdk_surface_entry *xdk_surface_find(uint32_t address);

/** Distinct entries in one section. */
size_t xdk_surface_section_count(const char *section);

#endif /* TSFP_XBOX_XDK_SURFACE_H */
"""

SOURCE_TEMPLATE = """\
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GENERATED by tools/gen_d3d8_surface.py. Do not edit; regenerate instead.
 *
%(summary)s */

#include "xdk_surface.h"

#include <string.h>

const xdk_surface_entry xdk_surface[XDK_SURFACE_COUNT] = {
%(entries)s};

const xdk_surface_entry *xdk_surface_find(uint32_t address)
{
    /* Linear scan. The table is a few hundred entries and lookups happen at
     * registration time, not per call. */
    for (size_t i = 0; i < XDK_SURFACE_COUNT; i++) {
        if (xdk_surface[i].address == address) {
            return &xdk_surface[i];
        }
    }
    return NULL;
}

size_t xdk_surface_section_count(const char *section)
{
    if (!section) {
        return 0;
    }
    size_t total = 0;
    for (size_t i = 0; i < XDK_SURFACE_COUNT; i++) {
        if (strcmp(xdk_surface[i].section, section) == 0) {
            total++;
        }
    }
    return total;
}
"""


def render_sources(surface: Surface) -> tuple[str, str]:
    """Render (header, source) for the recovered surface."""
    grouped = surface.by_section()
    ordered = [target for entries in grouped.values() for target in entries]
    vtable_refs = surface.vtable_ref_totals()

    lines = []
    for section, entries in grouped.items():
        sites = sum(t.total_sites for t in entries)
        suffix = ""
        if vtable_refs.get(section):
            # Not added to `sites`: a slot is a dispatch entry point, not a call
            # instruction, and the thunk's own tail jump is already counted.
            suffix = f", {vtable_refs[section]} vtable slots"
        lines.append(f" * {section:10s} {len(entries):4d} functions, {sites:5d} sites{suffix}\n")
    summary = "".join(lines)

    body = []
    for target in ordered:
        name = f'"{target.name}"' if target.name else "NULL"
        body.append(
            f'    {{{target.address:#010x}, "{target.section}", {name}, {target.total_sites}}},\n'
        )

    header = HEADER_TEMPLATE % {"count": len(ordered)}
    source = SOURCE_TEMPLATE % {"summary": summary, "entries": "".join(body)}
    return header, source


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Recover the XDK call surface from an XBE and generate a table."
    )
    parser.add_argument("xbe", type=Path, help="path to the XBE")
    parser.add_argument("--xtlid", type=Path, default=None, help="xtlid.xml, to name what it can")
    parser.add_argument("--out-dir", type=Path, default=Path("src/xbox"), help="where to write")
    parser.add_argument(
        "--origins",
        default=",".join(DEFAULT_ORIGINS),
        help=(
            "comma-separated sections to scan FROM. The default is the game's own "
            "code; every other executable section is itself an XDK library, so "
            "widening this measures XDK-to-XDK traffic, a different question."
        ),
    )
    parser.add_argument(
        "--direct-only",
        action="store_true",
        help="scan only decoded call/jmp rel32, the baseline the indirect forms are measured on",
    )
    parser.add_argument(
        "--null",
        action="store_true",
        help="also report the exhaustive false-positive null for the entry-point gate (slow)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    data = args.xbe.read_bytes()
    xbe = parse_xbe(data)
    origins = tuple(name for name in args.origins.split(",") if name)
    surface = recover_surface(data, xbe, origins=origins, extended=not args.direct_only)
    image = Image(data, xbe)

    named = 0
    if args.xtlid is not None and args.xtlid.is_file():
        from tools.xtlid import parse_xtlid_db, resolve_xtlid

        database = parse_xtlid_db(args.xtlid.read_text(encoding="utf-8", errors="replace"))
        resolved, _ = resolve_xtlid(xbe.xtlid, database)
        named = apply_xtlid_names(surface, {a: e.name for a, e in resolved.items()})

    print(f"origins: {', '.join(origins)}")
    for name, count in sorted(surface.undecodable.items()):
        print(f"  {name}: {count} undecodable offsets resynchronised past")
    print()
    print(f"{'section':10s} {'functions':>10s} {'sites':>7s} {'sites/fn':>9s} {'vtable':>8s}")
    vtable_refs = surface.vtable_ref_totals()
    functions = sites = 0
    for section, (count, section_sites) in surface.summary().items():
        functions += count
        sites += section_sites
        print(
            f"{section:10s} {count:>10d} {section_sites:>7d} "
            f"{section_sites / count:>9.1f} {vtable_refs.get(section, 0):>8d}"
        )
    print(f"{'TOTAL':10s} {functions:>10d} {sites:>7d}")
    print(f"\n{named} entries named from .XTLID")

    print("\nsites by attribution form:")
    for kind, count in surface.kind_totals().items():
        print(f"  {kind:10s} {count:6d}")
    print(
        f"  {'(ambiguous vtable, attributed to nothing)':42s} {surface.ambiguous_vtable_sites:6d}"
    )
    print(f"  {'(.text adjustor thunks found)':42s} {len(surface.thunks):6d}")

    audit = entry_point_audit(image, surface)
    print("\nentry-point test over every recovered row:")
    for key, value in audit.items():
        print(f"  {key:26s} {value:6d}")

    if args.null:
        print("\nfalse-positive null for the entry-point gate (exhaustive):")
        recovered = frozenset(surface.targets)
        total_tested = total_fired = 0
        for name in XDK_SECTIONS:
            result = false_positive_null(image, name, recovered=recovered)
            if not result.tested:
                continue
            total_tested += result.tested
            total_fired += result.fired
            print(
                f"  {result.section:10s} tested {result.tested:8d} "
                f"fired {result.fired:7d} {100 * result.rate:6.3f}%"
            )
        if total_tested:
            print(
                f"  {'TOTAL':10s} tested {total_tested:8d} "
                f"fired {total_fired:7d} {100 * total_fired / total_tested:6.3f}%"
            )

    header, source = render_sources(surface)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "xdk_surface.h").write_text(header, encoding="utf-8")
    (args.out_dir / "xdk_surface.c").write_text(source, encoding="utf-8")
    print(f"\nwrote {args.out_dir}/xdk_surface.{{h,c}}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
