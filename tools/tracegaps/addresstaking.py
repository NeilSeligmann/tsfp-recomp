# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T603: explain address-taking occurrences of the running flag's callers with evidence.

T599 left the entry barrier over the callers of the running flag writers OPEN: the image takes the
address of the title's functions in 2669 places that no CreateThread start explains. This module
removes occurrences that CANNOT hand an address to a transfer another thread makes, one rule at a
time, each rule with the evidence it rests on. A rule excludes an occurrence only when its
conditions are all proven from the image, and an occurrence no rule covers stays in the residue.

RULE (a), JUMP TABLES. Of the 2669 occurrences, 1986 are aligned dwords in executable bytes that sit
next to another code address (`Hit.kind == "table"`), almost all of them the `switch` tables the
compiler leaves inside `.text`. Such a dword at slot `L` holding `T` is explained when

  1. it lies in a RUN (maximal aligned dwords that are each a decoded instruction address) that is
     CLOSED: every address-taking occurrence of every byte address of the run (an immediate, a
     displacement, a stored dword or another table dword) and every direct transfer into it is
     one of the two instruction forms below. Nothing else can read the run, so no pointer escapes.
     An address stays NAMED only by such an operand: this is the existing T424 assumption that a
     code address is taken as an imm32, a stored dword or an operand and never computed from a base
     and an offset (`threadreach.ASSUMPTIONS`).
  2. the forms are `jmp dword ptr [reg*4 + base]` with `base` 4-aligned whose index is BOUNDED by the
     compiler's pattern (see `jump_table`): a `cmp reg, N` whose `ja` is the only way to the
     `jmp`, with no write of the flags or of `reg` between them, and no branch target between the
     `cmp` and the `jmp` (so nothing skips the bound), then the `jmp` reads slots `0..N` and every
     one of those slots is a decoded instruction of the jump's section. The two level form
     (`movzx reg2, byte [reg + bytes]` first, bytes `0..N` read from the image) bounds the slots by
     the largest byte. The other form is a one byte read (`movzx`) of the byte table.
  3. the slot is read by no jump at all (its dword is in the byte table or past every bound), or
     every jump that can read it sits in a function whose flood contains `T`. A read of `L` then
     only continues the thread inside the function it already executes (`ReachGraph.body` adds
     every table arm to the body of the function holding the jump), it hands no address to any
     other function.

RULE (a'), UNREFERENCED RUNS. A run with no address-taking occurrence and no transfer into it is
read by nothing (same named assumption), so its dwords, bytes of code that happen to spell an
address, are no pointer. 448 of the 592 runs of the retail image are of this kind.

RULE (a''), THE TABLE'S OWN ADDRESS. A displacement occurrence whose address lies inside a closed run
and is the base of a verified jump (or of its byte table) is the jump naming its table, not a code
address anyone transfers to.

Soundness conditions are code: `jump_table` refuses (with its reason) anything it cannot prove,
`TableRules.run` refuses a run with any other reference, and `explain` excludes only through
`TableRules`. Every refusal keeps the occurrence in the residue.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable
from collections.abc import Set as AbstractSet
from dataclasses import dataclass, field

import capstone
from capstone import x86 as cs_x86

from tools.tracegaps.flow import FAMILY_OF
from tools.tracegaps.reach import DATA_SECTIONS, TAKING_KINDS, Hit, ReachGraph

MASK32 = 0xFFFFFFFF
#: Value of a register at the entry of an arm, proven by the caller: (address, register family) to
#: an upper bound (unsigned) or None. See `TableRules.entry_bound`.
EntryBound = Callable[[int, str], int | None]
#: Instructions scanned back from a `jmp [reg*4 + base]` for its bound.
BOUND_WINDOW = 16
#: Largest index bound accepted (slots). Real tables are far smaller.
MAX_ENTRIES = 4096
#: Slots below an arm's slot searched for the base of a table that reads it (T610).
MAX_ARM_TABLE = 16

RULE_JUMP_TABLE = "jump-table"
RULE_UNREAD_SLOT = "jump-table-unread-slot"
RULE_UNREFERENCED = "unreferenced-run"
RULE_TABLE_OPERAND = "jump-table-operand"
TABLE_RULES = (RULE_JUMP_TABLE, RULE_UNREAD_SLOT, RULE_UNREFERENCED, RULE_TABLE_OPERAND)


@dataclass(frozen=True)
class JumpTable:
    """A `jmp [reg*4 + base]` proven to read only `entries` slots, every one a code address."""

    jump: int
    base: int
    entries: int
    #: address of the `cmp reg, N` that bounds the index
    bound_at: int
    #: (address, length) of the byte table a `movzx` reads first, or None for the direct form
    byte_table: tuple[int, int] | None
    targets: tuple[int, ...]
    #: slots whose value is outside every section: a jump through one leaves the image (T607)
    off_image: tuple[int, ...] = ()

    @property
    def end(self) -> int:
        return self.base + 4 * self.entries

    def reads(self, slot: int) -> bool:
        """Whether the dword at byte address `slot` is one of the dwords the jump can read."""
        return self.base <= slot < self.end and (slot - self.base) % 4 == 0


def _family(insn: capstone.CsInsn, register: int) -> str:
    return FAMILY_OF.get(insn.reg_name(register), "")


def _writes(insn: capstone.CsInsn) -> tuple[set[str], bool]:
    """(register families written, whether the flags are written) as capstone reports them."""
    _, written = insn.regs_access()
    families = {FAMILY_OF.get(insn.reg_name(register), "") for register in written}
    flags = "eflags" in {insn.reg_name(register) for register in written}
    families.discard("")
    return families, flags


def _is_transfer(insn: capstone.CsInsn) -> bool:
    return bool(
        {capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL, capstone.CS_GRP_RET, capstone.CS_GRP_IRET}
        & set(insn.groups)
    ) or insn.mnemonic in ("int3", "hlt", "ud2", "int")


def _byte_read(insn: capstone.CsInsn) -> tuple[int, int] | None:
    """For `movzx reg, byte ptr [reg2 + disp]` the (destination register, source register) pair."""
    if insn.mnemonic != "movzx" or len(insn.operands) != 2:
        return None
    destination, source = insn.operands
    if destination.type != cs_x86.X86_OP_REG or destination.size != 4:
        return None
    if source.type != cs_x86.X86_OP_MEM or source.size != 1:
        return None
    mem = source.mem
    if (mem.base == 0) == (mem.index == 0) or (mem.index != 0 and mem.scale != 1):
        return None
    return destination.reg, mem.base or mem.index


def jump_table(
    graph: ReachGraph,
    address: int,
    taken: AbstractSet[int] = frozenset(),
    entry_bound: EntryBound | None = None,
) -> JumpTable | str:
    """The bounded table the `jmp` at `address` reads, or the reason it is not proven.

    Reads backward from the `jmp` through the straight line (no gap, no transfer) for the
    compiler's pattern `cmp reg, N ; ja default ; [movzx reg2, byte [reg + bytes] ;] jmp [reg2*4 +
    base]`, with other instructions allowed in between only if they write neither the flags (before
    the `cmp` is found) nor the index register. A branch target anywhere from the `cmp` (exclusive)
    to the `jmp` refuses the table, because something could then reach the `jmp` without the bound.
    So does an instruction there whose address is in `taken` (an address-taking occurrence, see
    `TableRules.taken_among`): an indirect transfer could enter there too.

    T610: `and reg, reg2` bounds the index by the entry value of `reg` (`entry_bound`, an upper
    bound the caller proved for the instruction's address), the result never exceeds it.
    """
    code = graph.code
    image = code.image
    jump = code.insn_at(address)
    if jump is None or jump.mnemonic != "jmp" or len(jump.operands) != 1:
        return "not a jmp"
    operand = jump.operands[0]
    if operand.type != cs_x86.X86_OP_MEM or operand.size != 4:
        return "not a memory jmp"
    mem = operand.mem
    if mem.base != 0 or mem.index == 0 or mem.scale != 4:
        return "not [reg*4 + base]"
    base = mem.disp & MASK32
    if base % 4:
        return "table base is not 4-aligned"
    register = _family(jump, mem.index)
    if not register:
        return "index register unknown"
    section = image.section_at(address)
    targets = code.branch_targets()
    byte_read: int | None = None
    passed_ja = False
    cursor = jump
    for _ in range(BOUND_WINDOW):
        if cursor.address in targets or cursor.address in taken:
            return "a branch target lies between the bound and the jmp"
        previous = code.previous_insn(cursor)
        if previous is None or previous.address + previous.size != cursor.address:
            return "code gap before the jmp"
        written, flags = _writes(previous)
        if not passed_ja:
            mask = _mask_of(previous, register, entry_bound) if byte_read is None else None
            if mask is not None:
                return _close(
                    graph, address, base, mask, previous.address, None, section, masked=True
                )
            if previous.mnemonic == "ja" and previous.operands[0].type == cs_x86.X86_OP_IMM:
                passed_ja = True
            elif (
                byte_read is None
                and _is_conditional_jump(previous)
                and (found := _mask_behind(graph, previous, register, taken, entry_bound)) is not None
            ):
                return _close(graph, address, base, found[0], found[1], None, section, masked=True)
            elif _is_transfer(previous):
                return "a transfer between the bound and the jmp"
            elif (pair := _byte_read(previous)) is not None and _family(
                previous, pair[0]
            ) == register:
                if byte_read is not None:
                    return "two byte table reads"
                byte_read = previous.operands[1].mem.disp & MASK32
                register = _family(previous, pair[1])
                if not register:
                    return "byte table index register unknown"
            elif register in written:
                return "index register rewritten after the bound"
        else:
            first = previous.operands[0] if previous.operands else None
            if (
                previous.mnemonic == "cmp"
                and len(previous.operands) == 2
                and first is not None
                and first.type == cs_x86.X86_OP_REG
                and first.size == 4
                and _family(previous, first.reg) == register
            ):
                limit = _compared_constant(graph, previous, taken)
                if isinstance(limit, str):
                    return limit
                return _close(graph, address, base, limit, previous.address, byte_read, section)
            if _is_transfer(previous):
                return "a transfer between the cmp and the ja"
            if flags:
                return "the flags are rewritten between the cmp and the ja"
            if register in written:
                return "index register rewritten between the cmp and the ja"
        cursor = previous
    return "no bound found in the window"


def _is_conditional_jump(insn: capstone.CsInsn) -> bool:
    """A `jcc rel`: it falls through with every register unchanged."""
    return (
        capstone.CS_GRP_JUMP in insn.groups
        and insn.mnemonic != "jmp"
        and len(insn.operands) == 1
        and insn.operands[0].type == cs_x86.X86_OP_IMM
    )


def _mask_behind(
    graph: ReachGraph,
    conditional: capstone.CsInsn,
    register: str,
    taken: AbstractSet[int],
    entry_bound: EntryBound | None = None,
) -> tuple[int, int] | None:
    """(mask, address of its `and`) when only conditional jumps and instructions that leave
    `register` alone lie between an `and register, 2^k - 1` and `conditional` (inclusive), and no
    branch target or address-taking occurrence lies after the `and`. The conditional jumps fall
    through, so every path to the `jmp` passed the `and`."""
    code = graph.code
    targets = code.branch_targets()
    cursor = conditional
    for _ in range(BOUND_WINDOW):
        if cursor.address in targets or cursor.address in taken:
            return None
        previous = code.previous_insn(cursor)
        if previous is None or previous.address + previous.size != cursor.address:
            return None
        mask = _mask_of(previous, register, entry_bound)
        if mask is not None:
            return mask, previous.address
        written, _ = _writes(previous)
        if register in written:
            return None
        if _is_transfer(previous) and not _is_conditional_jump(previous):
            return None
        cursor = previous
    return None


def _mask_of(
    insn: capstone.CsInsn, register: str, entry_bound: EntryBound | None = None
) -> int | None:
    """N for `and reg, N` with N = 2^k - 1 (k >= 1) on the index register's family, else None.

    `and reg, reg2` (T610) gives the entry bound of `reg` when `entry_bound` proves one: an `and`
    only clears bits, so the result is at most the value `reg` had.
    """
    if insn.mnemonic != "and" or len(insn.operands) != 2:
        return None
    first, second = insn.operands
    if first.type != cs_x86.X86_OP_REG or first.size != 4:
        return None
    if _family(insn, first.reg) != register:
        return None
    if second.type == cs_x86.X86_OP_REG and second.size == 4 and entry_bound is not None:
        bound = entry_bound(insn.address, register)
        return bound if bound is not None and 0 < bound < MAX_ENTRIES else None
    if second.type != cs_x86.X86_OP_IMM:
        return None
    mask = second.imm & MASK32
    return mask if 0 < mask < MAX_ENTRIES and mask & (mask + 1) == 0 else None


def _compared_constant(
    graph: ReachGraph, compare: capstone.CsInsn, taken: AbstractSet[int]
) -> int | str:
    """The constant a `cmp reg, N` bounds against: its immediate, or the `mov reg2, N` that sets
    the second register with no branch target and no other write of it in between."""
    code = graph.code
    second = compare.operands[1]
    if second.type == cs_x86.X86_OP_IMM:
        return second.imm & MASK32
    if second.type != cs_x86.X86_OP_REG or second.size != 4:
        return "the bound is not a register or an immediate"
    register = _family(compare, second.reg)
    targets = code.branch_targets()
    cursor = compare
    for _ in range(BOUND_WINDOW):
        if cursor.address in targets or cursor.address in taken:
            return "a branch target lies between the bound constant and the cmp"
        previous = code.previous_insn(cursor)
        if previous is None or previous.address + previous.size != cursor.address:
            return "code gap before the bound constant"
        written, _ = _writes(previous)
        if register in written:
            source = previous.operands[1] if len(previous.operands) == 2 else None
            if (
                previous.mnemonic == "mov"
                and source is not None
                and source.type == cs_x86.X86_OP_IMM
            ):
                return source.imm & MASK32
            return "the bound register is not set by a mov of a constant"
        if _is_transfer(previous):
            return "a transfer before the bound constant"
        cursor = previous
    return "no bound constant found in the window"


def _close(
    graph: ReachGraph,
    jump: int,
    base: int,
    bound: int,
    bound_at: int,
    byte_read: int | None,
    section: object,
    *,
    masked: bool = False,
) -> JumpTable | str:
    """Size the table from the bound and check every slot it can read is a code address.

    A MASKED bound (`and reg, 2^k - 1`) also allows a slot whose value is outside every section: a
    CRT tail table keeps padding bytes in a slot its guard excludes (`memcpy`: `and edx, 3` with
    slot 0 unused), and a jump through a value that is no address of the image runs no guest code.
    """
    code = graph.code
    image = code.image
    if bound >= MAX_ENTRIES:
        return f"bound {bound:#x} is not a small constant"
    byte_table: tuple[int, int] | None = None
    entries = bound + 1
    if byte_read is not None:
        data = image.read(byte_read, bound + 1)
        if data is None:
            return "byte table is not readable"
        byte_table = (byte_read, bound + 1)
        entries = max(data) + 1
    targets: list[int] = []
    off_image: list[int] = []
    for slot in range(entries):
        value = image.u32(base + 4 * slot)
        if masked and value is not None and image.section_at(value) is None:
            off_image.append(slot)
            continue
        if value is None or code.insn_at(value) is None or image.section_at(value) is not section:
            return f"slot {slot} is not an instruction of the jump's section"
        targets.append(value)
    if not targets:
        return "no slot is an instruction of the jump's section"
    return JumpTable(jump, base, entries, bound_at, byte_table, tuple(targets), tuple(off_image))


@dataclass
class Run:
    """A maximal stretch of aligned dwords that are decoded instruction addresses, and its readers."""

    low: int
    high: int
    #: verified jumps that name the run, by jump address
    jumps: dict[int, JumpTable] = field(default_factory=dict)
    #: `movzx reg, byte [..]` instructions that read bytes of the run
    byte_readers: list[int] = field(default_factory=list)
    #: references no rule accepts: (where, why). A run with one is not closed.
    refusals: list[tuple[int, str]] = field(default_factory=list)

    @property
    def closed(self) -> bool:
        return not self.refusals

    @property
    def unreferenced(self) -> bool:
        return self.closed and not self.jumps and not self.byte_readers


@dataclass(frozen=True)
class Explanation:
    """Why an occurrence was excluded: the rule and the instruction or run it rests on."""

    rule: str
    evidence: int


class TableRules:
    """Rule (a): runs, their closure, and the explanation of each table hit."""

    def __init__(self, graph: ReachGraph) -> None:
        self.graph = graph
        self.code = graph.code
        self._runs: dict[int, Run] = {}
        self._jumps: dict[int, JumpTable | str] = {}
        self._hits: dict[int, list[Hit]] = {}
        self._covered: set[int] = set()
        self._taken: set[int] = set()
        self._taken_covered: set[int] = set()

    # ------------------------------------------------------------------ runs

    def run_bounds(self, location: int) -> tuple[int, int]:
        """The maximal aligned dwords around `location`, each a decoded instruction address."""
        image = self.code.image

        def code_word(address: int) -> bool:
            value = image.u32(address)
            return value is not None and self.code.insn_at(value) is not None

        low = location
        while code_word(low - 4):
            low -= 4
        high = location
        while code_word(high):
            high += 4
        return low, high

    def _index(self, addresses: Iterable[int]) -> None:
        """Add the hits of the addresses not indexed yet (one image scan for all of them)."""
        missing = sorted(set(addresses) - self._covered)
        if not missing:
            return
        self._hits.update(self.graph.address_hits(missing))
        self._covered.update(missing)

    def _index_taken(self, addresses: Iterable[int]) -> None:
        missing = sorted(set(addresses) - self._taken_covered)
        if not missing:
            return
        found = self.graph.address_hits(missing)
        self._taken.update(a for a, hits in found.items() if self.graph.address_taking(hits))
        self._taken_covered.update(missing)

    def _window(self, jump: int) -> list[int]:
        """The addresses of the straight line before a `jmp`, as far back as `jump_table` reads."""
        found: list[int] = []
        insn = self.code.insn_at(jump)
        while insn is not None and len(found) <= 2 * BOUND_WINDOW + 2:
            found.append(insn.address)
            previous = self.code.previous_insn(insn)
            if previous is None or previous.address + previous.size != insn.address:
                break
            insn = previous
        return found

    def prepare(self, locations: Iterable[int]) -> None:
        """Index every address-taking hit of every byte address in the runs around `locations`.

        One scan of the image for all runs (`ReachGraph.address_hits`), then one for the straight
        lines before the jumps that name them. Every byte address of a run counts, a table can be
        named at any of its slots. A window that starts before the run only reads a mix of bytes,
        never one of the run's dwords. Anything not prepared is indexed when first asked.
        """
        wanted: set[int] = set()
        for location in locations:
            low, high = self.run_bounds(location)
            wanted.update(range(low, high))
        self._index(wanted)
        lines: set[int] = set()
        for address in wanted:
            for hit in self._hits.get(address, ()):
                insn = self.code.insn_at(hit.where) if hit.kind == "displacement" else None
                if insn is not None and insn.mnemonic == "jmp":
                    lines.update(self._window(hit.where))
        self._index_taken(lines)

    def jump(self, address: int) -> JumpTable | str:
        if address not in self._jumps:
            self._index_taken(self._window(address))
            self._jumps[address] = jump_table(self.graph, address, self._taken, self.entry_bound)
        return self._jumps[address]

    def entry_bound(self, address: int, register: str) -> int | None:
        """T610: an upper bound of `register` where the instruction at `address` starts an arm.

        The arm is entered only through jump table slots: nothing falls into `address` (the bytes
        before it up to an unconditional `jmp` or `ret` hold no branch target and no address-taking
        occurrence), no direct transfer or immediate names it, and each occurrence is a slot of a
        CLOSED run that only proven jumps read. Every such jump is preceded in its straight line by
        `mov register, imm` with no later write of `register`, no transfer but conditional jumps
        and no branch target or address-taking occurrence after the `mov`. The bound is the largest
        of those immediates, None when any condition fails.
        """
        code = self.code
        insn = code.insn_at(address)
        if insn is None:
            return None
        targets = code.branch_targets()
        if address in targets:
            return None
        section = code.image.section_at(address)
        slots: list[int] = []
        for hit in self.graph.address_hits([address])[address]:
            if hit.kind not in TAKING_KINDS:
                continue
            slot = hit.location if hit.kind == "table" else hit.where
            if (
                hit.kind not in ("table", "data")
                or slot is None
                or slot % 4
                or code.image.section_at(slot) is not section
            ):
                return None
            slots.append(slot)
        if not slots or not self._no_fall_in(insn):
            return None
        bound = 0
        for slot in slots:
            readers = self._readers(slot)
            if not readers:
                return None
            for table in readers:
                value = self._constant_before(table.jump, register)
                if value is None:
                    return None
                bound = max(bound, value)
        return bound

    def _no_fall_in(self, insn: capstone.CsInsn) -> bool:
        """Nothing executes into `insn`: it follows an unconditional `jmp` or `ret` (through bytes
        with no branch target or address-taking occurrence), or the end of a closed run of
        table dwords, which only the jumps that name the run read."""
        code = self.code
        targets = code.branch_targets()
        run = self.run(insn.address - 4)
        if run.high == insn.address and run.closed:
            return True
        cursor = insn
        for _ in range(BOUND_WINDOW):
            previous = code.previous_insn(cursor)
            if previous is None or previous.address + previous.size != cursor.address:
                return False
            if previous.mnemonic in ("jmp", "ret", "int3") and len(previous.operands) <= 1:
                return True
            self._index_taken([previous.address])
            if previous.address in targets or _is_transfer(previous):
                return False
            if previous.address in self._taken:
                return False
            cursor = previous
        return False

    def _readers(self, slot: int) -> list[JumpTable]:
        """Every jump that can read the dword at `slot`, or [] when any occurrence of a base from
        which a 16 slot table reaches it is not a proven `jmp [reg*4 + base]` (a read we cannot
        see). The same named assumption as the runs: a table is read through its base operand."""
        bases = [slot - 4 * back for back in range(MAX_ARM_TABLE)]
        found: list[JumpTable] = []
        for _base, hits in self.graph.address_hits(bases).items():
            for hit in hits:
                if hit.kind not in TAKING_KINDS:
                    continue
                insn = self.code.insn_at(hit.where) if hit.kind == "displacement" else None
                if insn is None or insn.mnemonic != "jmp":
                    return []
                table = self.jump(hit.where)
                if not isinstance(table, JumpTable):
                    return []
                if table.reads(slot):
                    found.append(table)
        return found

    def _constant_before(self, jump: int, register: str) -> int | None:
        """The `mov register, imm` value that reaches the `jmp` at `jump` on every path, else None."""
        code = self.code
        targets = code.branch_targets()
        cursor = code.insn_at(jump)
        for _ in range(BOUND_WINDOW):
            if cursor is None:
                return None
            self._index_taken([cursor.address])
            previous = code.previous_insn(cursor)
            if previous is None or previous.address + previous.size != cursor.address:
                return None
            written, _ = _writes(previous)
            if register in written:
                source = previous.operands[1] if len(previous.operands) == 2 else None
                if (
                    previous.mnemonic != "mov"
                    or source is None
                    or source.type != cs_x86.X86_OP_IMM
                    or cursor.address in targets
                    or cursor.address in self._taken
                ):
                    return None
                return source.imm & MASK32
            if cursor.address in targets or cursor.address in self._taken:
                return None
            if _is_transfer(previous) and not _is_conditional_jump(previous):
                return None
            cursor = previous
        return None

    def run(self, location: int) -> Run:
        low, high = self.run_bounds(location)
        if low not in self._runs:
            self._index(range(low, high))
            self._runs[low] = self._build(low, high)
        return self._runs[low]

    def _build(self, low: int, high: int) -> Run:
        run = Run(low, high)
        for address in range(low, high):
            for hit in self._hits.get(address, ()):
                if hit.kind not in TAKING_KINDS:
                    continue
                self._classify_reference(run, address, hit)
        return run

    def _classify_reference(self, run: Run, address: int, hit: Hit) -> None:
        if hit.kind != "displacement":
            run.refusals.append((hit.where, f"{hit.kind} occurrence names {address:#x}"))
            return
        insn = self.code.insn_at(hit.where)
        if insn is not None and insn.mnemonic == "jmp":
            table = self.jump(hit.where)
            if isinstance(table, JumpTable) and run.low <= table.base and table.end <= run.high:
                run.jumps[hit.where] = table
                return
            run.refusals.append(
                (hit.where, table if isinstance(table, str) else "table leaves the run")
            )
            return
        if insn is not None and _byte_read(insn) is not None:
            run.byte_readers.append(hit.where)
            return
        run.refusals.append(
            (hit.where, f"displacement of {insn.mnemonic if insn else '?'} names {address:#x}")
        )

    # ------------------------------------------------------------------ the rules

    def explain(self, address: int, hit: Hit) -> Explanation | None:
        """The rule that excludes the occurrence of `address` described by `hit`, if one does."""
        if hit.kind == "table" and hit.location is not None:
            return self._explain_slot(address, hit.location)
        if hit.kind == "displacement":
            return self._explain_operand(address, hit)
        return None

    def _explain_slot(self, address: int, location: int) -> Explanation | None:
        run = self.run(location)
        if not run.closed:
            return None
        if run.unreferenced:
            return Explanation(RULE_UNREFERENCED, run.low)
        readers = [table for table in run.jumps.values() if table.reads(location)]
        if not readers:
            return Explanation(RULE_UNREAD_SLOT, run.low)
        for table in readers:
            owners = self.graph.enclosing_entries(table.jump)
            if not owners or any(address not in self.graph.body(owner) for owner in owners):
                return None
        return Explanation(RULE_JUMP_TABLE, readers[0].jump)

    def _explain_operand(self, address: int, hit: Hit) -> Explanation | None:
        """The jump naming its own table (or the movzx its byte table) is no transfer target."""
        insn = self.code.insn_at(hit.where)
        if insn is None:
            return None
        run = self.run(address - address % 4)
        if not run.closed or not (run.low <= address < run.high):
            return None
        if hit.where in run.jumps and run.jumps[hit.where].base == address:
            return Explanation(RULE_TABLE_OPERAND, hit.where)
        if hit.where in run.byte_readers:
            return Explanation(RULE_TABLE_OPERAND, hit.where)
        return None


def explain_hits(
    graph: ReachGraph, occurrences: Iterable[tuple[int, Hit]]
) -> tuple[list[tuple[int, Hit]], dict[str, list[tuple[int, Hit, Explanation]]]]:
    """Split `occurrences` into the residue and the excluded ones by rule (a)."""
    items = list(occurrences)
    rules = TableRules(graph)
    rules.prepare(
        [
            hit.location
            if hit.kind == "table" and hit.location is not None
            else address - address % 4
            for address, hit in items
            if hit.kind in ("table", "displacement")
        ]
    )
    residue: list[tuple[int, Hit]] = []
    explained: dict[str, list[tuple[int, Hit, Explanation]]] = {}
    for address, hit in items:
        verdict = rules.explain(address, hit)
        if verdict is None:
            residue.append((address, hit))
        else:
            explained.setdefault(verdict.rule, []).append((address, hit, verdict))
    return residue, explained


# ---------------------------------------------------------------------------------------------
# Part (b): what each remaining occurrence is.

ROLE_COMPARE = "compare"
ROLE_ARGUMENT = "argument"
ROLE_STORE = "store"
ROLE_REGISTER = "register"
ROLE_ARITHMETIC = "arithmetic"
ROLE_OPERAND = "operand"
ROLE_DATA_TABLE = "data-pointer-run"
ROLE_DATA_SLOT = "data-pointer-slot"
ROLE_CODE_DATA = "data-in-code"
ROLE_TABLE = "unverified-table"
ROLE_OTHER = "other"
#: Roles an occurrence is EXCLUDED by: its value reaches no transfer. The rest stay in the residue.
EXCLUDING_ROLES = frozenset({ROLE_COMPARE})
#: Instructions scanned forward from a `push imm` for the call that consumes it.
CONSUMER_WINDOW = 14


@dataclass(frozen=True)
class Role:
    """What an occurrence is, how it is used, and whether the use excludes it.

    `detail` is the evidence (callee, store form, container, refusal). `entry` says the taken
    address is the entry of a function of the callers' closure, not an interior address.
    """

    kind: str
    detail: str
    entry: bool

    @property
    def excluded(self) -> bool:
        return self.kind in EXCLUDING_ROLES


def _consumer(graph: ReachGraph, push: capstone.CsInsn) -> str:
    """The direct callee that follows a `push imm` in a straight line, as text."""
    code = graph.code
    cursor: capstone.CsInsn | None = push
    for _ in range(CONSUMER_WINDOW):
        cursor = code.next_insn(cursor) if cursor is not None else None
        if cursor is None:
            return "no call"
        if cursor.mnemonic == "call":
            target = cursor.operands[0]
            if target.type == cs_x86.X86_OP_IMM:
                return f"call {target.imm & MASK32:#x}"
            return "indirect call"
        if capstone.CS_GRP_JUMP in cursor.groups or capstone.CS_GRP_RET in cursor.groups:
            return "branch before any call"
    return "no call within the window"


def _store_form(insn: capstone.CsInsn) -> str:
    destination = insn.operands[0]
    mem = destination.mem
    if mem.base == 0 and mem.index == 0:
        return f"global [{mem.disp & MASK32:#x}]"
    return f"field [reg + {mem.disp & MASK32:#x}]"


def role_of(
    graph: ReachGraph, entries: frozenset[int], address: int, hit: Hit, rules: TableRules
) -> Role:
    """Classify one occurrence by the instruction or data around it. Never excludes by guess."""
    entry = address in entries
    code = graph.code
    if hit.kind == "table":
        why = "no run information"
        if hit.location is not None:
            refusals = rules.run(hit.location).refusals
            why = refusals[0][1] if refusals else "run closed"
        return Role(ROLE_TABLE, why, entry)
    if hit.kind == "data":
        section = graph.code.image.section_at(hit.where)
        name = section.name if section is not None else "?"
        if name not in DATA_SECTIONS:
            return Role(ROLE_CODE_DATA, name, entry)
        near = [
            graph.is_code(value)
            for delta in (-4, 4)
            if (value := code.image.u32(hit.where + delta)) is not None
        ]
        kind = ROLE_DATA_TABLE if any(near) else ROLE_DATA_SLOT
        return Role(kind, name, entry)
    insn = code.insn_at(hit.where)
    if insn is None:
        return Role(ROLE_OTHER, "no instruction", entry)
    mnemonic = insn.mnemonic
    if hit.kind == "immediate":
        if mnemonic in ("cmp", "test"):
            return Role(ROLE_COMPARE, mnemonic, entry)
        if mnemonic == "push":
            return Role(ROLE_ARGUMENT, _consumer(graph, insn), entry)
        if mnemonic == "mov" and insn.operands[0].type == cs_x86.X86_OP_MEM:
            return Role(ROLE_STORE, _store_form(insn), entry)
        if mnemonic == "mov":
            return Role(ROLE_REGISTER, "mov reg, imm", entry)
        return Role(ROLE_ARITHMETIC, mnemonic, entry)
    return Role(ROLE_OPERAND, mnemonic, entry)


@dataclass
class Classification:
    """Everything rules (a) and (b) say about the occurrences of one barrier."""

    total: int
    #: rule -> number of occurrences it excluded
    explained: dict[str, int]
    #: role kind -> occurrences, for the residue after (a)
    roles: dict[str, int]
    #: role kind -> occurrences excluded by (b)
    excluded_roles: dict[str, int]
    #: what is left after (a) and (b)
    residue: list[tuple[int, Hit]]
    #: distinct taken addresses left, split into function entries and interior addresses
    entries: dict[int, list[Role]]
    interiors: dict[int, list[Role]]

    @property
    def remaining(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for _, hit in self.residue:
            counts[hit.kind] = counts.get(hit.kind, 0) + 1
        return dict(sorted(counts.items()))


def classify(
    graph: ReachGraph, entries: Iterable[int], occurrences: Iterable[tuple[int, Hit]]
) -> Classification:
    """Apply rule (a) and the excluding roles of (b), then describe what is left."""
    items = list(occurrences)
    entry_set = frozenset(entries)
    rest, explained = explain_hits(graph, items)
    rules = TableRules(graph)
    rules.prepare(
        [hit.location for _, hit in rest if hit.kind == "table" and hit.location is not None]
    )
    roles: dict[str, int] = {}
    excluded: dict[str, int] = {}
    residue: list[tuple[int, Hit]] = []
    taken_entries: dict[int, list[Role]] = {}
    taken_interiors: dict[int, list[Role]] = {}
    for address, hit in rest:
        role = role_of(graph, entry_set, address, hit, rules)
        roles[role.kind] = roles.get(role.kind, 0) + 1
        if role.excluded:
            excluded[role.kind] = excluded.get(role.kind, 0) + 1
            continue
        residue.append((address, hit))
        target = taken_entries if role.entry else taken_interiors
        target.setdefault(address, []).append(role)
    return Classification(
        total=len(items),
        explained={rule: len(found) for rule, found in sorted(explained.items())},
        roles=dict(sorted(roles.items())),
        excluded_roles=dict(sorted(excluded.items())),
        residue=residue,
        entries=taken_entries,
        interiors=taken_interiors,
    )
