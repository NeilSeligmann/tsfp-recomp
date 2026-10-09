# SPDX-License-Identifier: GPL-3.0-or-later
"""Recover MSVC switch jump tables from the retail XBE, anchored at function entries.

WHY THIS BLOCKS THE RECOMPILER. A static recompiler emits control flow from a
control-flow graph, and `jmp dword ptr [eax*4 + 0x18fc8]` has no successor until the
table behind it is read. Every unresolved indirect jump is a function whose CFG is
incomplete, so the recompiler either cannot emit it at all or emits something that
falls off the end of the switch. There is no runtime fallback available to us -- the
target set has to be known at translation time. 687 sites in `.text` is 687 functions
that otherwise cannot be translated correctly, and switch-heavy code is exactly the
game logic (state machines, message dispatch, menu handling) that matters most.

THE TABLES ARE IN `.text`, NOT `.rdata`. This is the single most important fact about
this binary and it contradicts the usual assumption. All 687 `jmp [idx*4 + imm]` sites
in `.text` have a table base that also lands in `.text`, typically a few hundred bytes
past the jump, inside or just past the containing function. MSVC emitted them inline.
So this module cannot treat `.text` as pure code and a read-only section as pure data:
`.text` is both, interleaved, and a table base landing in an executable region is the
NORMAL case rather than a red flag.

WHY DECODING IS ANCHORED AT KNOWN FUNCTION ENTRIES. A free-running linear sweep of
`.text` walks straight into the table dwords themselves, and a 4-byte code address in
this image decodes by coincidence as a plausible `jmp [idx*4 + imm]`: the high bytes of
an address like 0x0018fc8 supply a ModRM with mod=00 and a scaled-index SIB, which is
exactly the encoding wanted. So the decode is anchored at a function entry from the
exported bounds CSV and never free-runs.

AND WHY THE DECODE RANGE IS NOT THE GHIDRA BODY. An earlier version of this module
decoded exactly `[entry_va, body_max_va]` and recorded that the 120 `.text` sites with
no containing Ghidra function were "overwhelmingly phantoms manufactured by the sweep".
THAT WAS WRONG, and it was wrong in the direction that loses recall. Cross-validated
against an independent recovery (`sp00nznet/xboxrecomp`) on the same binary, 125 of the
134 sites it found and this module did not are recovered UNCHANGED by the recogniser
here -- same strict `jmp [idx*4 + imm]` form, same `cmp`/`ja` bound -- the moment the
decode is allowed to continue past `body_max_va`. 117 of the 125 produce a target set
IDENTICAL to the independent tool's, and all 125 were hand-checked against the bytes:
125 real, 0 doubtful, 0 false. They were never phantoms.

THE CAUSE IS GHIDRA NON-DISCOVERY, NOT GHIDRA TRUNCATION, and the difference matters
because it says how much is out there. Truncation is real -- 59 of the 75 Ghidra
jumptable-failure sites end their function's body within 16 bytes of the failing jump --
but it accounts for exactly 1 of these 125. MEASURED instead: the 125 sit in 89 distinct
holes totalling 151,670 bytes, Ghidra's bodies cover only 81.6% of `.text` (8,559 holes,
717,626 bytes), and of the 108 real function entries behind the 125 sites, 81 (75%) are
reached ONLY through a vtable slot or a function-pointer table. Ghidra's recursive
descent follows direct calls, so those entries were never enqueued and no function was
ever created for them. One hole at 0x001c5256-0x001c6800 is 5,546 bytes and holds 10 of
the 125 on its own, behind a 6-byte `mov eax, 0x2f / ret` stub.

So each function is decoded from its entry up to the next function's entry rather than to
its own `body_max_va`, adopting the hole that follows it. The anchor is unchanged --
still a known entry, still never a free-running sweep -- and the hole is bounded on both
sides by Ghidra entries, so no two functions decode the same byte. A site found past
`body_max_va` is reported with `in_gap` set, so nothing in the output implies Ghidra had
a body there, and `function_va` is read as the entry the decode was anchored at rather
than a containment claim. Pass `adopt_gaps=False` to `recover_image` for the old,
narrower behaviour.

WHY THE `cmp`/`ja` BOUND IS LOAD-BEARING. A table has no terminator. Without the
compiler's own range check the length is a guess, and the obvious guess -- read dwords
while each one looks like a code address -- runs off the end of the table into whatever
follows, which in `.text` is more code whose bytes frequently look like a code address.
The `cmp reg, N` / `ja default` pair immediately before the jump IS the length: the
compiler proved the index is in `[0, N]`, so the table has exactly `N + 1` entries.
MEASURED: 560 of the 566 located sites have that pair within 12 instructions of the
jump, so the bound is available almost always and the unbounded scan is a fallback for
a handful of sites rather than the main path. Sites resolved by the scan are flagged
`no_bound` so a consumer can treat them with suspicion.

WHY ENTRIES ARE FLAGGED AND NEVER TRUNCATED. When a bounded table contains an entry
that is not a plausible code address, the two explanations are "the bound is wrong" and
"our idea of what code is, is wrong". Silently dropping the entry picks the second
without evidence and hands the recompiler a switch that is quietly missing a case,
which is a miscompile rather than an error. So the entry is kept, the table carries a
`suspect` flag, and the CLI reports how many tables are suspect. Report, do not
silently truncate.

AND A FLAG MUST NOT OVERSTATE WHAT WAS CHECKED. Target alignment is decided by a second
decode that skips the recovered tables as data, and that decode stops dead at the first
byte it cannot read as an instruction. `target_misaligned` is therefore reserved for a
target the decode ran ACROSS and did not stop at, while a target at or past the decode's
high-water mark gets `target_unverified`: alignment unknown, not alignment wrong.
MEASURED: all 7 retail sites the single flag used to cover are of the second kind, 6 of
them the shared table 0x003c9ae8 in FUN_003c9800 where inline dword data at
0x003c9a8c-0x003c9ae8 is not in the skip set and desynchronises the stream at 0x003c9aa2.
For the same reason an unbounded base that only resolves under a small positive bias is
reported as `biased_table_base` rather than `no_plausible_entries` -- 4 of the 5 retail
sites with no plausible entries are one identifiable MSVC idiom -- and no table is emitted
for it, because the bias is the index minimum and that is a guess.

MEASURED CENSUS, indirect `jmp` over the 8 code sections of retail `default.xbe`
(`base_address=0x10000`, `size_of_image=0x88cd60`):

    form                      section    sites   recoverable here
    jmp [idx*4 + imm]         .text        687   yes
    jmp [base + disp]         .text         37   no, vtable / struct dispatch
    jmp reg                   .text         19   no, register-computed
    jmp [abs]                 .text         16   no, import thunk slot
    jmp [idx*8 + imm]         .text          4   no, scale 8, bases in .data ~0x52d520
    jmp [idx*4 + imm]         D3D           13   yes
    jmp [idx*4 + imm]         XGRPH          6   yes
    jmp [idx*4 + imm]         XONLINE        4   yes
    jmp [idx*4 + imm]         XNET           4   yes
    jmp [idx*4 + imm]         XMV            2   yes

And of the 687 `.text` scale-4 sites, decoding anchored at the containing Ghidra
function entry:

    outcome                                      sites
    direct form, no byte index table               352
    index-indirection via movzx byte table         214
    past body_max_va, inside the next entry's gap   120   recovered since, see above
    decode desync                                    1
    cmp/ja pair within 12 instructions of the jump  560 of the 566 located

THE TWO IDIOMS. Direct, where the jump's own index register is the one the compiler
range-checked:

    0x00018eac  dec eax
    0x00018ead  cmp eax, 7
    0x00018eb0  push esi           <- unrelated instructions sit between cmp and ja
    0x00018eb1  push edi
    0x00018eb2  ja 0x18efa
    0x00018eb4  jmp dword ptr [eax*4 + 0x18fc8]

And index-indirection, where a byte table maps the checked value onto a smaller set of
distinct targets. Note the target table at 0x19f64 comes BEFORE the byte table at
0x19f9c, so neither ordering can be assumed:

    0x00019dc1  cmp esi, 0x19
    0x00019dc4  ja 0x19f58
    0x00019dca  movzx ecx, byte ptr [esi + 0x19f9c]
    0x00019dd1  jmp dword ptr [ecx*4 + 0x19f64]

For the indirect form the bound belongs to the register that addresses the BYTE table
(`esi` above), not to the jump's index register (`ecx`), and the target table length is
`max(index_bytes) + 1` rather than `bound + 1`. Getting that backwards reads 26 dwords
where the real table has as few as a handful.

A `movzx` is only read as a byte-table load when its displacement lands inside the
image. `movzx eax, byte ptr [esi + 8]` is an ordinary struct field load with the same
instruction shape, and reading the 8 as a table base loses the bound (which is on the
jump's own `eax`) and falls back to the unbounded scan. MEASURED at 0x0022e9ca: the real
table is 5 entries, `cmp eax, 4` says so, and without the guard the scan returned 16.

EVERYTHING HERE IS PURE except nothing: `image_from_xbe` takes bytes, not a path, and
every other entry point is a function of its arguments. The false-positive null is
exhaustive rather than sampled, so it needs no seed to reproduce.
"""

from __future__ import annotations

from bisect import bisect_right
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from functools import cached_property

import capstone
from capstone import x86

from tools.codediff.boundaries import Function
from tools.xbe import parse_xbe

FORM_DIRECT = "direct"
FORM_INDEX_INDIRECTION = "index_indirection"

CAUSE_REGISTER_TARGET = "register_target"
CAUSE_ABSOLUTE_SLOT = "absolute_slot"
CAUSE_POINTER_BASE = "pointer_base"
CAUSE_STACK_SLOT = "stack_slot"
CAUSE_REGISTER_BASE = "register_base"
CAUSE_UNSUPPORTED_SCALE = "unsupported_scale"
CAUSE_TABLE_OUTSIDE_IMAGE = "table_outside_image"
CAUSE_NO_PLAUSIBLE_ENTRIES = "no_plausible_entries"
CAUSE_BIASED_TABLE_BASE = "biased_table_base"
CAUSE_INDEX_TABLE_UNREADABLE = "index_table_unreadable"

SUSPECT_TARGET_OUTSIDE_FUNCTION = "target_outside_function"
SUSPECT_TARGET_OUTSIDE_CODE = "target_outside_code"
SUSPECT_TARGET_MISALIGNED = "target_misaligned"
SUSPECT_TARGET_UNVERIFIED = "target_unverified"
SUSPECT_BOUND_OVERRUNS = "bound_overruns"
SUSPECT_NO_BOUND = "no_bound"
SUSPECT_INDEX_SPAN_OVERLAPS_TABLE = "index_span_overlaps_table"
SUSPECT_TABLE_BASE_SHIFTED = "table_base_shifted"
"""`table_va` is not the literal displacement encoded in the `jmp` at `jump_va`.

Two distinct idioms set this, both MEASURED on retail and neither a guess: an `and reg,
mask` bound whose adjacent zero-excluding branch proved the index minimum is 1, not 0
(`_and_zero_exclusion_branch`), which shifts the read forward past the dead slot the
compiler never populated; and a `neg`-indexed dispatch recovered by
`_scan_targets_backward`, where the encoded displacement is the HIGHEST address in the
table rather than the lowest, so `table_va` is normalised down to the lowest address to
keep `table_end_va` and the span-exclusion logic correct. In the second case `targets[i]`
is NOT necessarily the destination for switch value `i` -- ascending address order and
ascending original-index order coincide for every other table this module recovers, but
not for one read backward. A consumer that needs the exact index mapping has to re-derive
it from `jump_va`'s own bytes rather than assume one; see `_scan_targets_backward`.
"""
"""`bound + 1` bytes from the byte table would run over the target table's own base.

Two distinct objects cannot overlap, so the `movzx` displacement is not where the byte
table starts and `max(index_bytes) + 1` is a count taken over the target table's own
pointers. The length falls back to the unbounded scan, which is the only remaining
evidence. MEASURED on retail 0x003de180: reading 801 bytes from 0x003de1f4 swept the
dword table at 0x003de400 and the switch arms' own x87 code, `max()` came out 255, and
the table was claimed as 256 entries of which 245 are not code addresses. The real byte
table is 38 bytes at 0x003de410 with a maximum slot of 4, and the table has 4 entries --
which is also what the independent recovery reads, and it is right.
"""

LENGTH_FROM_BOUND = "bound"
LENGTH_FROM_SCAN = "scan"

#: A table entry is a 32-bit code address. Scale-8 sites exist (4 of them) but their
#: bases live in `.data` and they are not a switch table, so they are not handled.
ENTRY_SIZE = 4

#: Two entries is the shortest thing worth calling a table. One "entry" is just an
#: address-shaped dword, and the false-positive null at min_entries=1 is dominated by
#: coincidence rather than by structure.
DEFAULT_MIN_ENTRIES = 2

#: Instructions to look back for the cmp/ja pair. MEASURED: 12 catches 560 of the 566
#: located sites, because MSVC freely sinks unrelated `push`es and address arithmetic
#: between the range check and the jump.
DEFAULT_BOUND_WINDOW = 12

#: Sections marked executable in the retail XBE, in image order. A target landing
#: outside all of these is not code and the table carrying it is suspect.
DEFAULT_CODE_SECTIONS = (".text", "D3D", "XGRPH", "DSOUND", "XONLINE", "XNET", "XMV", "XPP")

#: Hard cap on entries read when there is no bound. Nothing stops an unbounded scan
#: except an implausible dword, and in `.text` a long run of plausible ones is possible,
#: so the scan is capped rather than trusted.
DEFAULT_SCAN_LIMIT = 512

#: Positive 4-byte biases probed before an unbounded site is filed as garbage. When the
#: switch index never takes the value 0, MSVC encodes the displacement as
#: `real_table - 4 * index_min`, so the dword AT the displacement is not an entry at all
#: but the tail of a preceding instruction. MEASURED: 4 of the 5 retail
#: `no_plausible_entries` sites are this idiom, all four inside FUN_003c9800 (the
#: hand-written `memmove`), two of them at bias 1. The cap is small on purpose: a large
#: bias would let any nearby run of code addresses explain any base.
MAX_BASE_BIAS_ENTRIES = 4

#: No x86 instruction is longer than this, including all prefixes. Same constant and
#: same reason as `tools/codediff/normalise.py`.
MAX_INSN_BYTES = 15

#: Decode a window at a time so a resynchronisation re-copies a bounded slice rather
#: than a whole function body. Must comfortably exceed MAX_INSN_BYTES.
CHUNK_BYTES = 4096

#: Conditional branches that implement an unsigned "index out of range" test. MSVC uses
#: `ja` for `cmp reg, N` / `jmp table[reg]`; `jae` appears where the compiler compared
#: against the count rather than the maximum. The `jnbe`/`jnb` spellings are the same
#: opcodes under their alternate mnemonics, accepted so a hand-assembled fixture works.
BOUND_BRANCHES = frozenset({"ja", "jnbe", "jae", "jnb"})

#: Conditional branches that can prove an `and reg, mask` result is never zero on the
#: path reaching the jump. `and` sets ZF from its own result, so these consume it
#: directly with no `test` in between -- see `_and_zero_exclusion_branch`.
ZERO_EXCLUDING_BRANCHES = frozenset({"je", "jne"})

#: Stack frame registers. A jump through `[esp + x]` or `[ebp + x]` is a computed
#: target, never a literal table base.
STACK_REGISTERS = frozenset({"esp", "ebp"})

#: Sub-register spellings collapsed onto the 32-bit register they alias, so a `cmp al,
#: 7` feeding `jmp [eax*4 + imm]` still matches. Anything not listed maps to itself.
_REG_GROUPS: tuple[tuple[str, ...], ...] = (
    ("eax", "ax", "al", "ah"),
    ("ebx", "bx", "bl", "bh"),
    ("ecx", "cx", "cl", "ch"),
    ("edx", "dx", "dl", "dh"),
    ("esi", "si", "sil"),
    ("edi", "di", "dil"),
    ("ebp", "bp", "bpl"),
    ("esp", "sp", "spl"),
)
_REG_FAMILY: dict[str, str] = {name: group[0] for group in _REG_GROUPS for name in group}


@dataclass(frozen=True)
class Region:
    """One loaded section, as both an address range and a byte source.

    `virtual_size` and `len(data)` differ on purpose. `.data` in the retail XBE is
    0x333594 bytes virtual against 0x96290 raw, so most of it is `.bss`-style
    uninitialised space that is part of the image but has no bytes in the file.
    `contains` answers the address question, `has_bytes` answers the byte question, and
    conflating them is how a reader ends up fabricating zeroes.
    """

    name: str
    base_va: int
    virtual_size: int
    data: bytes
    """Initialised bytes only. `len(data)` may be smaller than `virtual_size`."""

    executable: bool

    def contains(self, va: int) -> bool:
        """Whether `va` is inside this region's virtual address range."""
        return self.base_va <= va < self.base_va + self.virtual_size

    def has_bytes(self, va: int, count: int) -> bool:
        """Whether the whole `count`-byte span at `va` is inside `data`."""
        if count <= 0:
            return False
        offset = va - self.base_va
        return 0 <= offset and offset + count <= len(self.data)


@dataclass(frozen=True)
class Image:
    """A loaded image as an ordered set of regions, with failing reads.

    `read_u8`/`read_u32` return None rather than raising, because the whole point of an
    unbounded entry scan is to walk until it falls off the end of the initialised bytes.
    An exception there would have to be caught at every call site and would make the
    scan's stopping condition an error path instead of its normal one.
    """

    regions: tuple[Region, ...]

    @cached_property
    def _starts(self) -> tuple[int, ...]:
        """Region base addresses, ascending, for the containment bisect."""
        return tuple(region.base_va for region in self.regions)

    def region_at(self, va: int) -> Region | None:
        """The region containing `va`, or None."""
        index = bisect_right(self._starts, va) - 1
        if index < 0:
            return None
        region = self.regions[index]
        return region if region.contains(va) else None

    def read_u8(self, va: int) -> int | None:
        """The byte at `va`, or None when it is not in any region's initialised bytes."""
        region = self.region_at(va)
        if region is None or not region.has_bytes(va, 1):
            return None
        return region.data[va - region.base_va]

    def read_u32(self, va: int) -> int | None:
        """The little-endian dword at `va`, or None when it is not fully readable."""
        region = self.region_at(va)
        if region is None or not region.has_bytes(va, ENTRY_SIZE):
            return None
        offset = va - region.base_va
        return int.from_bytes(region.data[offset : offset + ENTRY_SIZE], "little")

    def in_code(self, va: int) -> bool:
        """Whether `va` is inside an executable region.

        Virtual containment, not byte availability: a code address in the uninitialised
        tail of an executable region is still a code address as far as the image layout
        is concerned, and treating it otherwise would make plausibility depend on how
        much of the section the linker bothered to store.
        """
        region = self.region_at(va)
        return region is not None and region.executable


@dataclass(frozen=True)
class JumpTable:
    """One recovered switch table, with the evidence that produced its length."""

    jump_va: int
    function_va: int | None
    form: str
    """FORM_DIRECT or FORM_INDEX_INDIRECTION."""

    table_va: int
    targets: tuple[int, ...]
    bound: int | None
    """The N from `cmp reg, N`, or None when no range check was found."""

    length_source: str
    """LENGTH_FROM_BOUND when the compiler's own range check set the length."""

    index_table_va: int | None
    index_bytes: tuple[int, ...]
    suspect: tuple[str, ...]
    """SUSPECT_* flags, sorted and deduplicated. Non-empty means look at this one."""

    in_gap: bool = False
    """The jump sits past `function_va`'s `body_max_va`, in the gap before the next entry.

    Kept separate from `suspect` deliberately: the suspect flags are claims about the
    TARGETS, while this is a claim about where the SITE was found. It says Ghidra had no
    function body covering this jump and the decode reached it by continuing past the
    declared end, so `function_va` names the function the decode was anchored at rather
    than one Ghidra agrees contains the jump.
    """

    @property
    def entry_count(self) -> int:
        return len(self.targets)

    @property
    def table_end_va(self) -> int:
        return self.table_va + ENTRY_SIZE * self.entry_count

    @property
    def distinct_targets(self) -> frozenset[int]:
        return frozenset(self.targets)


@dataclass(frozen=True)
class UnresolvedSite:
    """An indirect jump this module could not turn into a target set."""

    jump_va: int
    function_va: int | None
    cause: str
    detail: str
    """The disassembly text, so the residue can be eyeballed without re-decoding."""

    in_gap: bool = False
    """As `JumpTable.in_gap`: the site is past `function_va`'s declared `body_max_va`."""


@dataclass(frozen=True)
class Bound:
    """The compiler's own range check on a switch index."""

    value: int
    """The N in `cmp reg, N`, or the mask in `and reg, mask`. The register's value lies
    in `[minimum, value]`, so the table has `value - minimum + 1` entries."""

    register: str
    cmp_va: int
    """VA of the `cmp` or `and` instruction that set `value`."""

    branch_va: int
    """VA of the branch that proved the bound: the `ja`-family branch for a `cmp`, or the
    zero-excluding `je`/`jne` for an `and` whose `minimum` is 1. Equals `cmp_va` itself
    when an `and` bound has no such branch, since there is nothing else to report and the
    field has no `| None` spelling to fall back to."""

    minimum: int = 0
    """The smallest value the register can take, when that is provably 1 rather than 0:
    an `and reg, mask` bounds the register to `[0, mask]` from the arithmetic alone, no
    branch required, but an ADJACENT `je`/`jne` that excludes zero is what additionally
    proves the register never actually reaches 0 on the path reaching the jump. Always 0
    for a `cmp`-derived bound, which has no such idiom. See `_and_bound` and
    `SUSPECT_TABLE_BASE_SHIFTED`."""


@dataclass
class Recovery:
    """Everything found over one function or one whole image."""

    tables: list[JumpTable] = field(default_factory=list)
    """Ascending by (jump_va, table_va)."""

    unresolved: list[UnresolvedSite] = field(default_factory=list)
    """Ascending by jump_va."""

    @property
    def functions_with_tables(self) -> frozenset[int]:
        return frozenset(
            table.function_va for table in self.tables if table.function_va is not None
        )

    @property
    def distinct_targets(self) -> frozenset[int]:
        targets: set[int] = set()
        for table in self.tables:
            targets.update(table.targets)
        return frozenset(targets)

    @property
    def suspect_tables(self) -> list[JumpTable]:
        return [table for table in self.tables if table.suspect]

    def cause_counts(self) -> dict[str, int]:
        """Cause -> number of unresolved sites, descending by count then cause."""
        counts: dict[str, int] = {}
        for site in self.unresolved:
            counts[site.cause] = counts.get(site.cause, 0) + 1
        return dict(sorted(counts.items(), key=lambda item: (-item[1], item[0])))

    def entry_count_histogram(self) -> dict[int, int]:
        """Entry count -> number of tables with that many entries, ascending by key."""
        counts: dict[int, int] = {}
        for table in self.tables:
            counts[table.entry_count] = counts.get(table.entry_count, 0) + 1
        return dict(sorted(counts.items()))


@dataclass(frozen=True)
class NullResult:
    """How often the unbounded entry-scan rule fires on an arbitrary aligned address."""

    region_name: str
    offsets_tested: int
    offsets_fired: int
    excluded: int
    """4-aligned offsets skipped because they ARE part of a recovered table."""

    min_entries: int

    @property
    def rate(self) -> float:
        if self.offsets_tested == 0:
            return 0.0
        return self.offsets_fired / self.offsets_tested


def image_from_regions(regions: Sequence[Region]) -> Image:
    """An `Image` over `regions`, sorted ascending by `base_va`."""
    return Image(regions=tuple(sorted(regions, key=lambda region: region.base_va)))


def image_from_xbe(data: bytes, *, code_sections: Sequence[str] = DEFAULT_CODE_SECTIONS) -> Image:
    """Build an `Image` from raw XBE bytes.

    Every section becomes a region, executable exactly when its name is in
    `code_sections`. The XBE section flags carry an executable bit too, but the name set
    is used instead so a caller can decide what counts as code without re-parsing: the
    census this module is calibrated against was taken over those eight names.
    """
    executable = frozenset(code_sections)
    xbe = parse_xbe(data)
    regions = [
        Region(
            name=section.name,
            base_va=section.virtual_addr,
            virtual_size=section.virtual_size,
            data=data[section.raw_addr : section.raw_addr + section.raw_size],
            executable=section.name in executable,
        )
        for section in xbe.sections
    ]
    return image_from_regions(regions)


def scan_targets(image: Image, table_va: int, *, limit: int, min_entries: int) -> tuple[int, ...]:
    """Read dwords from `table_va` while each one is a plausible code target.

    Stops BEFORE the first dword that is unreadable or that does not land inside an
    executable region, and at `limit` entries. Returns `()` when fewer than
    `min_entries` were accepted, so a caller can treat "no table here" as falsy.

    THIS IS THE ONLY implementation of the unbounded rule. `false_positive_null` calls
    this same function on every aligned address in a region, which is what makes its
    rate the null for this rule rather than the null for something adjacent to it. Two
    copies of the rule would silently decouple and the null would stop meaning anything.
    """
    targets: list[int] = []
    va = table_va
    while len(targets) < limit:
        value = image.read_u32(va)
        if value is None or not image.in_code(value):
            break
        targets.append(value)
        va += ENTRY_SIZE
    if len(targets) < min_entries:
        return ()
    return tuple(targets)


def _scan_targets_backward(
    image: Image, table_va: int, *, limit: int, min_entries: int
) -> tuple[int, ...]:
    """Read dwords from `table_va` going DOWN while each one is a plausible code target.

    The mirror of `scan_targets`, for the MSVC idiom where the compiler negates the
    switch index before scaling it (`neg ecx` then `jmp dword ptr [ecx*4 + disp]`): the
    register's two's-complement value makes the accessed address DECREASE as the real,
    pre-negation index increases, so the entry for original index `k` lives at
    `table_va - ENTRY_SIZE*k` rather than `table_va + ENTRY_SIZE*k`. Entries come back in
    that same `k` order -- ascending original index, descending address -- which is
    exactly backwards from the ascending-address order every other table in this module
    uses, so `_resolve` reverses the result and renormalises `table_va` down to the
    lowest address before building a `JumpTable`; see `SUSPECT_TABLE_BASE_SHIFTED`.

    Tried only when the forward rule at the SAME base finds nothing, same as
    `_biased_base`: a real forward table is still a forward table, and this is strictly a
    fallback for the one idiom `scan_targets` cannot see at all by construction.

    MEASURED at retail 0x003c99c6 (`jmp dword ptr [ecx*4 + 0x3c9a98]`, immediately
    preceded by `neg ecx`, the switch inside `CopyDownVec`'s `memmove` fast path): the 8
    dwords from 0x3c9a98 down to 0x3c9a7c are all code addresses and the 9th, at
    0x3c9a78, is not -- exactly the 8 arms `cmp ecx, 8` / `jb` establishes before the
    `neg`. That is a consensus miss: the independent recovery this module is cross-checked
    against does not find it either (`their_entries` is 0 for this site), because it has
    no `cmp`/`ja` and no `and`/`je` either, only `cmp`/`jb`, which neither tool treats as
    a bound today.

    Same stopping rule as `scan_targets`: the first unreadable or implausible dword ends
    the run, `limit` caps it, and fewer than `min_entries` returns `()`.
    """
    targets: list[int] = []
    va = table_va
    while len(targets) < limit:
        value = image.read_u32(va)
        if value is None or not image.in_code(value):
            break
        targets.append(value)
        va -= ENTRY_SIZE
    if len(targets) < min_entries:
        return ()
    return tuple(targets)


def false_positive_null(
    image: Image,
    region_name: str,
    *,
    min_entries: int = DEFAULT_MIN_ENTRIES,
    scan_limit: int = DEFAULT_SCAN_LIMIT,
    exclude: frozenset[int] = frozenset(),
    exclude_spans: tuple[tuple[int, int], ...] = (),
) -> NullResult:
    """How often `scan_targets` fires on an address that is not part of a known table.

    EXHAUSTIVE, not sampled: every 4-byte-aligned VA inside the region's initialised
    bytes is tested, so the result reproduces exactly with no seed. `exclude` is the set
    of genuinely recovered table bases, which are skipped and counted separately -- a
    real table firing the rule is the signal, not the noise, and leaving them in would
    bias the null upwards by exactly the thing being measured.

    `exclude_spans` holds half-open `[start, end)` VA spans and skips every offset inside
    one, counting it in `excluded` exactly as an excluded base is. THIS IS THE DIFFERENCE
    BETWEEN TWO QUESTIONS. Excluding only the bases leaves the INTERIOR offsets of those
    same tables in the denominator, and those interiors satisfy the scan rule for the same
    reason the base does: on retail `.text` roughly 3,800 of the 4,221 fires are inside a
    table this module genuinely recovered. So the base-only number answers "how often does
    the rule fire at an arbitrary aligned address" and the span number answers "how often
    does the rule fire on data belonging to no table we recovered". Both are reported.

    A region with no initialised bytes, or a name no region has, gives a zero result
    rather than raising, so a caller can ask about `.rdata` in an image that lacks one.
    """
    region = next((candidate for candidate in image.regions if candidate.name == region_name), None)
    if region is None or not region.data:
        return NullResult(
            region_name=region_name,
            offsets_tested=0,
            offsets_fired=0,
            excluded=0,
            min_entries=min_entries,
        )

    start = region.base_va + (-region.base_va % ENTRY_SIZE)
    end = region.base_va + len(region.data) - ENTRY_SIZE
    # Merged once and bisected per offset. A linear span walk per offset would be ~550
    # spans x ~984,000 offsets on retail `.text`, which is the whole runtime of the tool.
    spans = _merge_spans(exclude_spans)
    span_starts = [low for low, _ in spans]
    tested = 0
    fired = 0
    excluded = 0
    for va in range(start, end + 1, ENTRY_SIZE):
        index = bisect_right(span_starts, va) - 1
        if va in exclude or (index >= 0 and va < spans[index][1]):
            excluded += 1
            continue
        tested += 1
        if scan_targets(image, va, limit=scan_limit, min_entries=min_entries):
            fired += 1
    return NullResult(
        region_name=region.name,
        offsets_tested=tested,
        offsets_fired=fired,
        excluded=excluded,
        min_entries=min_entries,
    )


def recover_function(
    image: Image,
    function: Function,
    *,
    decode_max_va: int | None = None,
    bound_window: int = DEFAULT_BOUND_WINDOW,
    min_entries: int = DEFAULT_MIN_ENTRIES,
    scan_limit: int = DEFAULT_SCAN_LIMIT,
) -> Recovery:
    """Recover every switch table reachable by decoding forward from one function entry.

    `decode_max_va` is the last address the decode may read, defaulting to the function's
    own `body_max_va`. `recover_image` raises it to the next function entry so a body
    Ghidra truncated at an unrecovered switch does not hide the rest of that function;
    see the module docstring. The anchor is the function entry either way.

    Pure and deterministic. Builds its own decoder, so it is usable standalone;
    `recover_image` shares one across functions because building 12,343 of them is pure
    overhead.
    """
    return _recover_one(
        _decoder(), image, function, decode_max_va, bound_window, min_entries, scan_limit
    )


def recover_image(
    image: Image,
    functions: Sequence[Function],
    *,
    adopt_gaps: bool = True,
    bound_window: int = DEFAULT_BOUND_WINDOW,
    min_entries: int = DEFAULT_MIN_ENTRIES,
    scan_limit: int = DEFAULT_SCAN_LIMIT,
) -> Recovery:
    """Recover switch tables over every function whose entry is inside the image.

    Functions whose `entry_va` is in no region are skipped silently: an exported bounds
    CSV can name addresses outside the sections we loaded, and that is a fact about the
    CSV rather than an error here.

    WITH `adopt_gaps` (the default) each function is decoded from its entry up to the
    byte before the NEXT function's entry rather than to its own `body_max_va`, so the
    code in a body Ghidra truncated at an unrecovered switch is still reached. The gap is
    bounded on both sides by Ghidra entries, so no two functions ever decode the same
    byte and the decode is still anchored, never a sweep. Sites found past `body_max_va`
    carry `in_gap`. See the module docstring for the measurement that forced this.

    One decoder is built and reused, and only each function's own byte range is decoded.
    Decoding the whole of `.text` once per function would be ~12,000x the work, and
    decoding it once globally is the free-running sweep this module exists to avoid.
    """
    decoder = _decoder()
    tables: list[JumpTable] = []
    unresolved: list[UnresolvedSite] = []
    limits = _gap_limits(image, functions) if adopt_gaps else {}
    for function in functions:
        if image.region_at(function.entry_va) is None:
            continue
        found = _recover_one(
            decoder,
            image,
            function,
            limits.get(function.entry_va),
            bound_window,
            min_entries,
            scan_limit,
        )
        tables.extend(found.tables)
        unresolved.extend(found.unresolved)
    tables.sort(key=lambda table: (table.jump_va, table.table_va))
    unresolved.sort(key=lambda site: site.jump_va)
    return Recovery(tables=tables, unresolved=unresolved)


def _gap_limits(image: Image, functions: Sequence[Function]) -> dict[int, int]:
    """entry_va -> the last address that function's decode may read.

    That is the byte before the NEXT function entry, which is what makes the gap safe to
    adopt: the next entry is Ghidra's own claim about where this run of code stops, so the
    adopted range can never reach another function's first instruction, and no two
    functions ever decode the same byte. The last entry of all runs to the end of its
    region; a limit that overshoots into a later region is clamped by `_read_available`
    to the initialised bytes of the region the entry is in, so it reads nothing extra.

    NEVER BELOW `body_max_va`. A row whose successor starts inside its own declared body
    would otherwise have its decode SHORTENED by gap adoption, which is the opposite of
    the point.

    AND ONLY INSIDE AN EXECUTABLE REGION. The bounds CSV has four rows whose `entry_va`
    is in `.data` or `.rdata`, and the holes after those four are 56 KB to 244 KB of pure
    data. Decoding them produces exactly the phantoms the module docstring warns about:
    MEASURED, adopting those four holes manufactured 399 bogus indirect-jump sites (344
    in `.data`, 55 in `.rdata`) and NOT ONE table. Every one of the 125 real tables gap
    adoption recovers is in an executable region. A function in a data section keeps its
    declared body and nothing more, which is what it had before gap adoption existed.
    """
    entries = sorted({function.entry_va for function in functions})
    limits: dict[int, int] = {}
    for function in functions:
        region = image.region_at(function.entry_va)
        if region is None or not region.executable:
            continue
        position = bisect_right(entries, function.entry_va)
        if position < len(entries):
            limit = entries[position] - 1
        else:
            limit = region.base_va + region.virtual_size - 1
        limits[function.entry_va] = max(limit, function.body_max_va)
    return limits


@dataclass
class _Pending:
    """A table under construction, before the alignment pass can finish its suspects."""

    jump_va: int
    form: str
    table_va: int
    targets: tuple[int, ...]
    bound: int | None
    length_source: str
    index_table_va: int | None
    index_bytes: tuple[int, ...]
    suspect: set[str]


def _decoder() -> capstone.Cs:
    """A detail-enabled 32-bit x86 decoder. Operand detail is needed throughout."""
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    return decoder


def _family(name: str) -> str:
    """The 32-bit register `name` aliases, or `name` itself when it is not a GPR."""
    return _REG_FAMILY.get(name, name)


def _read_available(image: Image, va: int, count: int) -> bytes:
    """Up to `count` bytes at `va`, truncated at the end of the containing region.

    Returns `b""` when `va` has no initialised byte at all. Does not stitch across
    regions: a function body never spans two sections, and silently joining two
    sections would decode the join as if it were contiguous code.
    """
    region = image.region_at(va)
    if region is None:
        return b""
    offset = va - region.base_va
    if offset < 0 or offset >= len(region.data):
        return b""
    return region.data[offset : min(offset + count, len(region.data))]


def _decode_body(
    decoder: capstone.Cs, image: Image, function: Function, decode_max_va: int
) -> list[capstone.CsInsn]:
    """Decode from `function.entry_va` to `decode_max_va`, resynchronising past bad bytes.

    Chunked for the same reason as `tools/codediff/normalise.py`: an instruction in the
    last MAX_INSN_BYTES of a non-final chunk may have been cut short by the chunk
    boundary rather than by the data, so it is left for the next chunk.

    Resynchronises by one byte rather than aborting, because a function body legitimately
    contains the jump tables this module is looking for and those bytes are not code.
    """
    length = decode_max_va - function.entry_va + 1
    if length <= 0:
        return []
    body = _read_available(image, function.entry_va, length)
    if not body:
        return []

    base_va = function.entry_va
    insns: list[capstone.CsInsn] = []
    total = len(body)
    offset = 0
    while offset < total:
        end = min(offset + CHUNK_BYTES, total)
        limit = end if end == total else end - MAX_INSN_BYTES
        progress = offset
        for insn in decoder.disasm(body[offset:end], base_va + offset):
            insn_offset = insn.address - base_va
            if insn_offset + insn.size > limit:
                break
            insns.append(insn)
            progress = insn_offset + insn.size
        if progress == offset:
            offset += 1
        else:
            offset = progress
    return insns


def _jump_operand(insn: capstone.CsInsn) -> x86.X86Op | None:
    """The single operand of an indirect `jmp`, or None when this is not one.

    A `jmp rel8`/`rel32` has an immediate operand and a known successor, so it is not
    an indirect jump and is filtered out here rather than classified as unresolved.
    """
    if insn.id != x86.X86_INS_JMP:
        return None
    operands = insn.operands
    if len(operands) != 1:
        return None
    operand = operands[0]
    if operand.type == x86.X86_OP_IMM:
        return None
    return operand


def _classify(insn: capstone.CsInsn, operand: x86.X86Op) -> tuple[str | None, int]:
    """(cause, table_va) for an indirect jump. A None cause means "recoverable".

    Order matters: the stack-frame test comes before the plain pointer-base test, so
    `jmp [ebp + 8]` is reported as a stack slot rather than as a generic pointer.
    """
    if operand.type == x86.X86_OP_REG:
        return CAUSE_REGISTER_TARGET, 0
    if operand.type != x86.X86_OP_MEM:
        return CAUSE_REGISTER_TARGET, 0

    mem = operand.mem
    base = _family(insn.reg_name(mem.base)) if mem.base else ""
    index = _family(insn.reg_name(mem.index)) if mem.index else ""

    if not base and not index:
        return CAUSE_ABSOLUTE_SLOT, 0
    if base in STACK_REGISTERS:
        return CAUSE_STACK_SLOT, 0
    if base and not index:
        return CAUSE_POINTER_BASE, 0
    if base and index:
        return CAUSE_REGISTER_BASE, 0
    # Index with no base. A zero displacement means the table starts at address 0, so
    # the base is register-computed in some way we cannot see and there is no literal.
    if mem.disp == 0:
        return CAUSE_REGISTER_BASE, 0
    if mem.scale != ENTRY_SIZE:
        return CAUSE_UNSUPPORTED_SCALE, 0
    return None, mem.disp


def _writes(insn: capstone.CsInsn, register: str) -> bool:
    """Whether `insn` writes any register aliasing the 32-bit `register`."""
    _, written = insn.regs_access()
    return any(_family(insn.reg_name(reg)) == register for reg in written)


def _index_indirection(
    image: Image, insns: list[capstone.CsInsn], jump_index: int, index_register: str
) -> tuple[int, str] | None:
    """(index_table_va, bound_register) when a byte table feeds the jump index.

    Walks back over the whole decoded body, not just the bound window, for the most
    recent instruction that writes the jump's index register. Only that one instruction
    decides the form: if the index came from anywhere other than a byte-table load, the
    jump is direct, and continuing to search past the real producer would match a
    `movzx` belonging to an unrelated earlier switch in the same function.

    THE DISPLACEMENT MUST LAND IN THE IMAGE. `movzx eax, byte ptr [esi + 8]` loading a
    struct FIELD has the same instruction shape as a byte-table load, and taking the 8
    for a table base loses the bound (which is on `eax`, not `esi`) and falls back to the
    unbounded scan. MEASURED at 0x0022e9ca: the real table is 5 entries and the scan
    returned 16. The discriminator is clean because a real index-table base is a full
    code address in `.text` while a field offset is far below `base_address` (0x10000),
    so a displacement in no region means the form is direct.
    """
    for position in range(jump_index - 1, -1, -1):
        insn = insns[position]
        if not _writes(insn, index_register):
            continue
        if insn.mnemonic != "movzx":
            return None
        operands = insn.operands
        if len(operands) != 2 or operands[1].type != x86.X86_OP_MEM or operands[1].size != 1:
            return None
        mem = operands[1].mem
        if mem.disp == 0:
            return None
        if image.region_at(mem.disp) is None:
            return None
        if mem.base and not mem.index:
            return mem.disp, _family(insn.reg_name(mem.base))
        if mem.index and not mem.base and mem.scale == 1:
            return mem.disp, _family(insn.reg_name(mem.index))
        return None
    return None


def _find_bound(
    insns: list[capstone.CsInsn], jump_index: int, bound_register: str, window: int
) -> Bound | None:
    """The bound on `bound_register` nearest the jump, within `window` insns.

    Searches backwards and takes the first match, which is the LAST such evidence in
    program order. An instruction on the wrong register is skipped rather than ending
    the search, because MSVC interleaves unrelated comparisons with the real range check.

    TWO SHAPES. `cmp bound_register, N` paired with a `ja`-family branch is the common
    one -- see `_cmp_bound`. `and bound_register, mask` is the other: the MSVC CRT
    `memcpy`/`memmove` alignment dispatch (retail `FUN_003c9800`) and the Internet
    checksum loop (retail `FUN_00443f40`) both reach their jump table this way, with no
    `cmp` anywhere nearby, and this module used to refuse all of them -- the `and` alone
    mathematically bounds the register to `[0, mask]`, no branch needed for the upper
    bound at all. See `_and_bound` for how the lower bound is handled. Both shapes are
    tried at every position in program order, so a function using one form nearer the
    jump than the other keeps using whichever is nearer, exactly as the `cmp`-only search
    always did.
    """
    lowest = max(0, jump_index - window)
    for position in range(jump_index - 1, lowest - 1, -1):
        insn = insns[position]
        if insn.mnemonic == "cmp":
            bound = _cmp_bound(insns, position, jump_index, bound_register)
            if bound is not None:
                return bound
        elif insn.mnemonic == "and":
            bound = _and_bound(insns, position, jump_index, bound_register)
            if bound is not None:
                return bound
    return None


def _cmp_bound(
    insns: list[capstone.CsInsn], position: int, jump_index: int, bound_register: str
) -> Bound | None:
    """`cmp bound_register, N` at `position`, paired with an unsigned-above branch.

    Unchanged behaviour, pulled out of `_find_bound` so the `and` shape can share the
    same backward walk without duplicating it.
    """
    insn = insns[position]
    operands = insn.operands
    if len(operands) != 2:
        return None
    if operands[0].type != x86.X86_OP_REG or operands[1].type != x86.X86_OP_IMM:
        return None
    register = insn.reg_name(operands[0].reg)
    if _family(register) != bound_register:
        return None
    # A negative immediate is not a switch range check: the compiler proves an
    # unsigned upper bound, so N is non-negative and N + 1 is the entry count.
    if operands[1].imm < 0:
        return None
    branch = _branch_between(insns, position, jump_index)
    if branch is None:
        return None
    return Bound(value=operands[1].imm, register=register, cmp_va=insn.address, branch_va=branch)


def _and_bound(
    insns: list[capstone.CsInsn], position: int, jump_index: int, bound_register: str
) -> Bound | None:
    """`and bound_register, mask` at `position`: bounds the register to `[0, mask]`.

    No paired branch is required for the UPPER bound -- ANDing with `mask` can never
    produce a value outside `[0, mask]`, a fact about the arithmetic rather than about
    control flow, unlike `cmp`/`ja` where the branch is what keeps an out-of-range value
    from ever reaching the jump. The LOWER bound is a separate question, answered by
    `_and_zero_exclusion_branch`: without it `minimum` stays 0, the read starts at the
    `and`'s own displacement, and a slot the compiler never actually populated (because
    the real minimum was 1) is read anyway and left for `_flag_targets` to catch via
    `SUSPECT_TARGET_OUTSIDE_CODE` -- flagged, never silently dropped, same as every other
    implausible entry this module finds.

    MEASURED on retail: `and edx, 3` at 0x003c982b and 0x003c99af (`FUN_003c9800`) have
    no adjacent zero-excluding branch and resolve with `minimum` 0, matching the 4 entries
    each already had from the unbounded scan exactly -- a length-source upgrade, not a
    new table. `and eax, 3` at 0x003c9848 and 0x003c99dc, same function, don't either, so
    their index-0 slot is read and flagged rather than assumed clean; both were refused
    outright as `biased_table_base` before this existed. `and ebx, 0xf` at 0x00443f8f
    (`FUN_00443f40`) has one immediately next (`je 0x443f9f`) and resolves with `minimum`
    1, 15 entries, nothing flagged -- also previously refused.

    REJECTS a candidate whose register is overwritten again before the jump, unlike
    `_cmp_bound`'s long-standing (and deliberately unchanged, see
    `test_a_scaled_movzx_index_is_not_a_byte_index_table`) willingness to use a `cmp`
    whatever comes between it and the jump. This guard is new precisely because `and`
    needed it: MEASURED on retail, `and edx, 0xfffffffc` at 0x003d5f89 -- an ordinary
    pointer-alignment idiom, nothing to do with a switch -- sat in this module's bound
    window and would have been read as a 4,294,967,292-entry table, because `sub edx,
    0x2a` between it and `jmp dword ptr [edx*4 + 0x3d6278]` at 0x003d5fa6 replaces edx's
    value first. `cmp` never produced a bound anywhere near that large on real input, so
    tightening only the shape that did was enough.
    """
    insn = insns[position]
    operands = insn.operands
    if len(operands) != 2:
        return None
    if operands[0].type != x86.X86_OP_REG or operands[1].type != x86.X86_OP_IMM:
        return None
    register = insn.reg_name(operands[0].reg)
    if _family(register) != bound_register:
        return None
    mask = operands[1].imm
    # A switch-bounding mask is always a contiguous run of low-order 1 bits (`2**k - 1`):
    # that is what makes it select "the low k bits" rather than clear or isolate some
    # other field. `and reg, mask + 1 == 0` for exactly that shape (0b0111 + 1 == 0b1000),
    # which is also what rejects a SIGN-EXTENDED negative immediate outright, without a
    # separate `< 0` check: an "align down" idiom like `and edx, 0xfffffffc` (clearing the
    # low 2 bits of a pointer, MEASURED on retail at 0x003d5f89, nothing to do with a
    # switch) fails this the same way whether capstone hands it back as -4 or as the
    # unsigned 4,294,967,292 -- both have a 1 bit above the trailing zeros.
    if mask <= 0 or (mask & (mask + 1)) != 0:
        return None
    for later in range(position + 1, jump_index):
        if _writes(insns[later], bound_register):
            return None
    branch_va = _and_zero_exclusion_branch(insns, position, jump_index)
    return Bound(
        value=mask,
        register=register,
        cmp_va=insn.address,
        branch_va=insn.address if branch_va is None else branch_va,
        minimum=0 if branch_va is None else 1,
    )


def _and_zero_exclusion_branch(
    insns: list[capstone.CsInsn], and_position: int, jump_index: int
) -> int | None:
    """VA of an adjacent `je`/`jne` that proves the `and`'s register is never zero.

    ADJACENT means the very next decoded instruction -- the one retail idiom this was
    characterised on (`FUN_00443f40`, `and ebx, 0xf` / `je 0x443f9f`) has no `test`
    between them at all, because the `and` already set ZF from its own result and the
    branch consumes it directly. Widening the search past the immediate next instruction
    would mean guessing how many unrelated instructions may sit in between, which is
    exactly the kind of guess `CAUSE_BIASED_TABLE_BASE`'s docstring warns against for the
    sibling idiom; the three retail `and`-bound sites with no adjacent branch (two at
    0x003c9848 and 0x003c99dc) are read with `minimum` 0 and their index-0 slot flagged
    rather than silently assumed excluded.

    `je target`: taking the branch means the register WAS zero, so the straight-line path
    that keeps going -- and eventually reaches the jump -- only runs when it is not. No
    constraint on `target` beyond being a real branch (never its own fallthrough), because
    whichever way it goes is irrelevant to the fallthrough case being proven zero-free.

    `jne target`: the mirror shape. Falling through here means the register WAS zero, so
    for that path to be excluded from reaching the jump, the TAKEN branch has to be what
    leads toward it: `target` must land between this instruction and `jump_index`
    inclusive, i.e. forward into the jump table dispatch rather than off to an unrelated
    part of the function.
    """
    position = and_position + 1
    if position >= jump_index:
        return None
    insn = insns[position]
    if insn.mnemonic not in ZERO_EXCLUDING_BRANCHES:
        return None
    operands = insn.operands
    if len(operands) != 1 or operands[0].type != x86.X86_OP_IMM:
        return None
    target = operands[0].imm
    if target == insn.address + insn.size:
        return None
    if insn.mnemonic == "je":
        return insn.address
    if insn.address < target <= insns[jump_index].address:
        return insn.address
    return None


def _branch_between(insns: list[capstone.CsInsn], cmp_index: int, jump_index: int) -> int | None:
    """VA of the first unsigned-above branch strictly between the two indices."""
    for position in range(cmp_index + 1, jump_index):
        if insns[position].mnemonic in BOUND_BRANCHES:
            return insns[position].address
    return None


def _read_bounded(image: Image, table_va: int, count: int) -> tuple[tuple[int, ...], bool]:
    """(targets, overran) reading exactly `count` dwords, stopping at the first unreadable.

    `overran` is True ONLY when a dword within `count` could not be READ at all, so the
    bound ran past the initialised bytes. A dword that reads cleanly but does not land in
    an executable region is a different finding, and the caller raises
    SUSPECT_TARGET_OUTSIDE_CODE for it alone: conflating the two would report a bound
    overrun for a table whose every dword was there. Entries are NEVER dropped for being
    implausible: the caller flags the table and keeps every entry, because a missing switch
    case is a miscompile while a flagged one is a question.
    """
    targets: list[int] = []
    for step in range(count):
        value = image.read_u32(table_va + ENTRY_SIZE * step)
        if value is None:
            return tuple(targets), True
        targets.append(value)
    return tuple(targets), False


def _biased_base(
    image: Image, table_va: int, *, limit: int, min_entries: int
) -> tuple[int, int] | None:
    """(smallest bias, entries it would yield) when a biased base explains `table_va`.

    THE MSVC BIASED-BASE IDIOM. When the switch index never takes the value 0 the
    compiler folds the minimum into the displacement, emitting
    `real_table - ENTRY_SIZE * index_min`, so the dword sitting AT the displacement is not
    an entry at all. MEASURED at 0x003c984d, `jmp dword ptr [eax*4 + 0x3c9860]`: the dword
    at 0x3c9860 is 0x90003c98, the tail of the preceding instruction plus a 0x90 padding
    byte, while 0x3c9864, 0x3c9868 and 0x3c986c are all code addresses. Four of the five
    retail sites that used to be filed as `no_plausible_entries` are this one idiom in
    FUN_003c9800, and filing a recognisable idiom as garbage hides a real finding.

    Returns None when no bias up to `MAX_BASE_BIAS_ENTRIES` makes the scan fire. The
    caller reports the bias and never builds a table from it: `index_min` is a guess.
    """
    for bias in range(1, MAX_BASE_BIAS_ENTRIES + 1):
        targets = scan_targets(
            image, table_va + ENTRY_SIZE * bias, limit=limit, min_entries=min_entries
        )
        if targets:
            return bias, len(targets)
    return None


def _read_index_bytes(image: Image, index_table_va: int, count: int) -> tuple[int, ...] | None:
    """Exactly `count` bytes from the byte index table, or None if any is unreadable."""
    values: list[int] = []
    for step in range(count):
        value = image.read_u8(index_table_va + step)
        if value is None:
            return None
        values.append(value)
    return tuple(values)


def _resolve(
    image: Image,
    insns: list[capstone.CsInsn],
    jump_index: int,
    table_va: int,
    bound_window: int,
    min_entries: int,
    scan_limit: int,
) -> _Pending | tuple[str, str]:
    """Turn one recoverable site into a `_Pending`, or into a (cause, detail) pair."""
    insn = insns[jump_index]
    detail = f"{insn.mnemonic} {insn.op_str}"
    mem = insn.operands[0].mem
    index_register = _family(insn.reg_name(mem.index))

    indirection = _index_indirection(image, insns, jump_index, index_register)
    if indirection is None:
        form = FORM_DIRECT
        index_table_va: int | None = None
        bound_register = index_register
    else:
        form = FORM_INDEX_INDIRECTION
        index_table_va, bound_register = indirection

    bound = _find_bound(insns, jump_index, bound_register, bound_window)
    suspect: set[str] = set()

    if bound is None:
        targets = scan_targets(image, table_va, limit=scan_limit, min_entries=min_entries)
        reversed_base = False
        if not targets:
            # The forward rule cannot see a `neg`-indexed dispatch by construction: see
            # `_scan_targets_backward`. Tried before `_biased_base` because, like the
            # forward scan, a successful run here IS the table rather than a guess that
            # needs a `cmp`/`and` to validate -- the contiguous-plausible-dword evidence
            # is the same evidence the forward path already trusts enough to emit on.
            backward = _scan_targets_backward(
                image, table_va, limit=scan_limit, min_entries=min_entries
            )
            if backward:
                targets = tuple(reversed(backward))
                reversed_base = True
        if not targets:
            biased = _biased_base(image, table_va, limit=scan_limit, min_entries=min_entries)
            if biased is None:
                return CAUSE_NO_PLAUSIBLE_ENTRIES, detail
            bias, entries = biased
            # NO TABLE IS EMITTED. The bias is `index_min`, and without a `cmp` bound the
            # index minimum is a guess, so the table's length is a guess too and this
            # module does not guess at a table length. This renames an unexplained
            # failure into a characterised one, and nothing more.
            return (
                CAUSE_BIASED_TABLE_BASE,
                f"{detail} (biased base, bias +{bias}: would yield {entries} entries)",
            )
        suspect.add(SUSPECT_NO_BOUND)
        emit_table_va = table_va
        if reversed_base:
            # The encoded displacement was the HIGHEST address in the table, not the
            # lowest: renormalise so `table_end_va` and the span-exclusion logic stay
            # correct for this table like every other one. See
            # `SUSPECT_TABLE_BASE_SHIFTED`.
            emit_table_va = table_va - ENTRY_SIZE * (len(targets) - 1)
            suspect.add(SUSPECT_TABLE_BASE_SHIFTED)
        return _Pending(
            jump_va=insn.address,
            form=form,
            table_va=emit_table_va,
            targets=targets,
            bound=None,
            length_source=LENGTH_FROM_SCAN,
            index_table_va=index_table_va,
            index_bytes=(),
            suspect=suspect,
        )

    if bound.minimum > 0:
        suspect.add(SUSPECT_TABLE_BASE_SHIFTED)

    index_bytes: tuple[int, ...] = ()
    if form == FORM_INDEX_INDIRECTION and index_table_va is not None:
        read_index_table_va = index_table_va + bound.minimum
        byte_count = bound.value - bound.minimum + 1
        if read_index_table_va <= table_va < read_index_table_va + byte_count:
            # The byte table cannot cover the target table's own base: see
            # SUSPECT_INDEX_SPAN_OVERLAPS_TABLE. The bound is still recorded, because a
            # range check WAS found -- it is the byte table that is not where the movzx
            # said it was -- but it can no longer set the length, so the scan does.
            targets = scan_targets(image, table_va, limit=scan_limit, min_entries=min_entries)
            if not targets:
                return CAUSE_NO_PLAUSIBLE_ENTRIES, detail
            suspect.add(SUSPECT_INDEX_SPAN_OVERLAPS_TABLE)
            return _Pending(
                jump_va=insn.address,
                form=form,
                table_va=table_va,
                targets=targets,
                bound=bound.value,
                length_source=LENGTH_FROM_SCAN,
                index_table_va=index_table_va,
                index_bytes=(),
                suspect=suspect,
            )
        read = _read_index_bytes(image, read_index_table_va, byte_count)
        if read is None:
            return CAUSE_INDEX_TABLE_UNREADABLE, detail
        index_bytes = read
        # The byte table maps the bound's values onto a smaller set of slots, so the
        # target table is as long as the largest slot it names, not as long as the byte
        # table.
        count = max(index_bytes) + 1
        read_table_va = table_va
        effective_bound = bound.value
        reported_index_table_va = read_index_table_va
    else:
        read_table_va = table_va + ENTRY_SIZE * bound.minimum
        count = bound.value - bound.minimum + 1
        effective_bound = bound.value - bound.minimum
        reported_index_table_va = index_table_va

    targets, overran = _read_bounded(image, read_table_va, count)
    if not targets:
        return CAUSE_NO_PLAUSIBLE_ENTRIES, detail
    if overran:
        suspect.add(SUSPECT_BOUND_OVERRUNS)
    return _Pending(
        jump_va=insn.address,
        form=form,
        table_va=read_table_va,
        targets=targets,
        bound=effective_bound,
        length_source=LENGTH_FROM_BOUND,
        index_table_va=reported_index_table_va,
        index_bytes=index_bytes,
        suspect=suspect,
    )


def _recover_one(
    decoder: capstone.Cs,
    image: Image,
    function: Function,
    decode_max_va: int | None,
    bound_window: int,
    min_entries: int,
    scan_limit: int,
) -> Recovery:
    """`recover_function` with the decoder supplied, so `recover_image` can share one."""
    if decode_max_va is None:
        decode_max_va = function.body_max_va
    insns = _decode_body(decoder, image, function, decode_max_va)
    pending: list[_Pending] = []
    unresolved: list[UnresolvedSite] = []

    for position, insn in enumerate(insns):
        operand = _jump_operand(insn)
        if operand is None:
            continue
        cause, table_va = _classify(insn, operand)
        detail = f"{insn.mnemonic} {insn.op_str}"
        if cause == CAUSE_UNSUPPORTED_SCALE:
            detail = f"{detail} (scale {operand.mem.scale})"
        in_gap = insn.address > function.body_max_va
        if cause is not None:
            unresolved.append(
                UnresolvedSite(
                    jump_va=insn.address,
                    function_va=function.entry_va,
                    cause=cause,
                    detail=detail,
                    in_gap=in_gap,
                )
            )
            continue
        if image.region_at(table_va) is None:
            unresolved.append(
                UnresolvedSite(
                    jump_va=insn.address,
                    function_va=function.entry_va,
                    cause=CAUSE_TABLE_OUTSIDE_IMAGE,
                    detail=detail,
                    in_gap=in_gap,
                )
            )
            continue
        outcome = _resolve(image, insns, position, table_va, bound_window, min_entries, scan_limit)
        if isinstance(outcome, _Pending):
            pending.append(outcome)
        else:
            cause, detail = outcome
            unresolved.append(
                UnresolvedSite(
                    jump_va=insn.address,
                    function_va=function.entry_va,
                    cause=cause,
                    detail=detail,
                    in_gap=in_gap,
                )
            )

    _flag_targets(decoder, image, function, pending, decode_max_va)
    tables = [
        JumpTable(
            jump_va=item.jump_va,
            function_va=function.entry_va,
            form=item.form,
            table_va=item.table_va,
            targets=item.targets,
            bound=item.bound,
            length_source=item.length_source,
            index_table_va=item.index_table_va,
            index_bytes=item.index_bytes,
            suspect=tuple(sorted(item.suspect)),
            in_gap=item.jump_va > function.body_max_va,
        )
        for item in pending
    ]
    tables.sort(key=lambda table: (table.jump_va, table.table_va))
    unresolved.sort(key=lambda site: site.jump_va)
    return Recovery(tables=tables, unresolved=unresolved)


def _flag_targets(
    decoder: capstone.Cs,
    image: Image,
    function: Function,
    pending: list[_Pending],
    decode_max_va: int,
) -> None:
    """Add the target validity suspects to every pending table, in place.

    THE TWO ALIGNMENT FLAGS ARE NOT THE SAME CLAIM. `target_misaligned` says the
    alignment decode ran ACROSS this address and did not stop there, so the target is
    genuinely not an instruction boundary. `target_unverified` says the decode never
    reached the address at all, so nothing is known about its alignment. Only the first is
    evidence of a wrong target, and MEASURED on retail all 7 sites that the single flag
    used to cover were of the second kind: 6 are the shared table 0x003c9ae8 in
    FUN_003c9800, where unrecovered inline dword data at 0x003c9a8c-0x003c9ae8 desynchronises
    the stream around 0x003c9aa2 and it never reaches targets 0x3c9af8 onwards. Reporting
    those as misaligned said "we did not look" in the voice of "we looked and it is wrong".

    A target outside a code region, or outside the function, gets no alignment flag at
    all: there is nothing to align against in the first case, and nothing was decoded
    there in the second.
    """
    if not pending:
        return
    limit_va = _decode_limit(image, function, decode_max_va)
    starts, high_water_va = _instruction_starts(decoder, image, function, pending, limit_va)
    for item in pending:
        for target in item.targets:
            inside_code = image.in_code(target)
            inside_function = function.entry_va <= target <= function.body_max_va
            if not inside_code:
                item.suspect.add(SUSPECT_TARGET_OUTSIDE_CODE)
            if not inside_function:
                item.suspect.add(SUSPECT_TARGET_OUTSIDE_FUNCTION)
            if not inside_code or not inside_function:
                continue
            if target >= high_water_va:
                item.suspect.add(SUSPECT_TARGET_UNVERIFIED)
            elif target not in starts:
                item.suspect.add(SUSPECT_TARGET_MISALIGNED)


def _decode_limit(image: Image, function: Function, decode_max_va: int) -> int:
    """Exclusive end of the byte range that can be decoded for this function."""
    region = image.region_at(function.entry_va)
    if region is None:
        return function.entry_va
    available = region.base_va + len(region.data)
    return min(decode_max_va + 1, available)


def _instruction_starts(
    decoder: capstone.Cs,
    image: Image,
    function: Function,
    pending: list[_Pending],
    limit_va: int,
) -> tuple[set[int], int]:
    """(instruction start VAs, high-water mark) from a pass that knows where the tables are.

    THE HIGH-WATER MARK IS THE VA ONE PAST THE LAST BYTE THIS PASS ACCOUNTED FOR, which is
    what separates "not an instruction start" from "never looked at". The stream ends
    rather than resynchronising, so it routinely stops well short of `limit_va`: on retail
    FUN_003c9800 it halts around 0x003c9aa2 on unrecovered inline dword data and every
    later address in the function is simply unknown to it. A skipped table span counts as
    accounted for -- those bytes are known data, so a target inside one IS known not to be
    an instruction -- but everything at or past the stopping point is not.

    WHY THE FIRST PASS CANNOT BE USED. The first pass does not yet know the tables
    exist, so it decodes straight into them: a run of code addresses is consumed as
    instructions, the stream desynchronises at a byte boundary of its own choosing, and
    every "instruction start" it reports from there until it happens to re-align is
    fictional. Testing a target against that set would reject real targets and accept
    bogus ones. This pass skips the recovered table and byte-table ranges as DATA, which
    is what keeps the stream aligned across them.

    THE DECODE IS ANCHORED AT THE FUNCTION ENTRY AND NOWHERE ELSE. Seeding it at each
    recovered target as well makes every target an instruction start by construction,
    which reduces `target_misaligned` to "the target is inside a recovered table" and
    makes the flag vacuous for the case it exists to catch: a target pointing into the
    middle of a real instruction. One seed costs the coverage of a case block the linear
    stream cannot reach, and that is the right trade, because an uncovered block reads as
    unverified -- a question -- while a self-confirming seed reads as proof.

    A span to skip may legitimately fall outside [entry_va, limit_va): on this binary the
    target table frequently sits just PAST `body_max_va` rather than inside the function
    (for FUN_00019d70 the body ends at 0x19f62 and the table starts at 0x19f64). The
    stream clamps every span lookup to `limit_va`, so such a span is simply never reached.
    """
    spans = _merge_spans(_table_spans(pending))
    starts: set[int] = set()
    high_water_va = _decode_stream(decoder, image, function.entry_va, limit_va, spans, starts)
    return starts, high_water_va


def _table_spans(pending: list[_Pending]) -> list[tuple[int, int]]:
    """Half-open byte ranges occupied by recovered target tables and byte index tables."""
    spans: list[tuple[int, int]] = []
    for item in pending:
        spans.append((item.table_va, item.table_va + ENTRY_SIZE * len(item.targets)))
        if item.index_table_va is not None and item.index_bytes:
            spans.append((item.index_table_va, item.index_table_va + len(item.index_bytes)))
    return spans


def _merge_spans(spans: Iterable[tuple[int, int]]) -> tuple[tuple[int, int], ...]:
    """Overlapping or touching half-open ranges merged, ascending. Empty ones dropped."""
    merged: list[list[int]] = []
    for low, high in sorted(span for span in spans if span[1] > span[0]):
        if merged and low <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], high)
        else:
            merged.append([low, high])
    return tuple((low, high) for low, high in merged)


def _span_at(spans: tuple[tuple[int, int], ...], va: int) -> tuple[int, int] | None:
    """The span containing `va`, or None."""
    index = bisect_right([span[0] for span in spans], va) - 1
    if index < 0:
        return None
    span = spans[index]
    return span if va < span[1] else None


def _next_span_start(spans: tuple[tuple[int, int], ...], va: int, limit: int) -> int:
    """Start of the first span after `va`, clamped to `limit`."""
    for low, _ in spans:
        if low > va:
            return min(low, limit)
    return limit


def _decode_stream(
    decoder: capstone.Cs,
    image: Image,
    seed: int,
    limit_va: int,
    spans: tuple[tuple[int, int], ...],
    starts: set[int],
) -> int:
    """Decode from `seed` to `limit_va`, skipping `spans`, adding starts in place.

    Returns the high-water mark: the VA one past the last byte the stream accounted for,
    which is `limit_va` only when it ran the whole range. A skipped span counts as
    accounted for, an address at or past the return value does not, and `_flag_targets`
    needs exactly that line to tell a misaligned target from an unverified one.

    Ends the stream rather than resynchronising when a byte does not decode. A resync
    here would invent instruction starts, and this set exists precisely to decide
    whether an address is a real instruction start.

    Returns early when the stream reaches an address another stream already covered,
    which keeps the total work linear in the function body however many seeds there are.
    """
    va = seed
    while va < limit_va:
        if va in starts:
            return va
        span = _span_at(spans, va)
        if span is not None:
            va = span[1]
            continue
        stop = min(_next_span_start(spans, va, limit_va), limit_va)
        body = _read_available(image, va, stop - va)
        if not body:
            return va
        progress = 0
        for insn in decoder.disasm(body, va):
            if insn.address - va + insn.size > len(body):
                break
            starts.add(insn.address)
            progress = insn.address - va + insn.size
        if progress == 0:
            return va
        va += progress
        if va < stop:
            # The stream stopped short of a known data boundary, so it is desynchronised
            # with respect to that boundary. Anything further would be guesswork.
            return va
    return va
