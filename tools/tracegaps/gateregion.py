# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T592: the loading bar gate reads no vblank counter, and nothing else can move it.

THE CLAIM. The owner thread (start 0x3801D9) runs the title's loading bar gate `sub_00156840`
(called from 0x3D60C): it tests the flag [0x4E7A94], and when that is 0 it sets the stage, then
spins `while (1.0f > [0x749848])`, stores [0x4E7A94] = 1 and tail jumps to `sub_00155550`, which sets
the stop flag and calls the wait wrapper 0x3800BF on the worker's thread handle. The worker (start
0x156CB0) advances [0x749848] once per change of the vblank counter [0x563918], so the model has to
deliver the worker's blanks while the owner is runnable. That is deterministic for the worker only
if the owner cannot sample the counter in that time.

WHAT IS PROVEN (every item is measured over the retail image, the report prints each fact).
1. From the entry with [0x4E7A94] == 0 (the one conditional branch that tests it is pruned) the
   control flow, followed through every `jcc` and `jmp` and stopped at the first `call`, reaches no
   instruction that touches the counter address 0x563918 (as a memory operand or an immediate), has
   no indirect transfer, no `ret` and no computed memory operand, and its only call is the wait
   wrapper 0x3800BF. So after the call of 0x156840 the owner's next call is 0x3800BF.
2. The direct closure of 0x3800BF does not reach the counter getter 0x22030 and has no unresolved
   indirect call, so the wrapper reaches no reader before it blocks. The wait is untimed (-1) on the
   worker's thread, so the owner wakes only after the worker ended.
3. The counter [0x563918] is named by exactly the callback `0x22020` (the only writer) and the getter
   `0x22030` (the only reader), and the getter has exactly the two registered callers (T371).
4. [0x4E7A94] has two writers, the gate itself and the loading bar start function `0x156D80`. The
   T424 entry barrier holds for both: the direct closure of the worker's, the loader's and the
   network poll's start routine is outside every function that reaches a writer, and no address of
   such a function is taken except as a CreateThread start routine, so those threads cannot execute
   a writer and no other thread flips the flag between the call and the owner's own read.
   The same barrier over the running flag's writers (0x155550, 0x156D80, T599) is OPEN: its callers are
   the title's call tree (about a thousand functions) and the image takes their addresses in data and
   immediates that are no CreateThread start. T603 narrowed it with evidence (`addresstaking`, `saturate`):
   the jump table rules exclude 1854 of the 2669 occurrences (the compiler's bounded `switch` tables inside
   executable sections and runs nothing references), 814 remain (immediates that hand a callback to a setter,
   vtable and callback slots, tables with no provable bound), and the saturated closure of each thread start
   (every indirect site a rule resolves added) holds no writer but still has unresolved sites (the shared
   `call [reg + disp]` fields have no sound pool without types), so no start closes and only the direct-edge
   result holds for the running flag. The host backs that assumption with a runtime falsifier (T604,
   `recomp_second_vblank_note_call`): a non-owner call of a writer entry while the owner is in the gate or the
   worker's blanks run stops the boot by name.

ASSUMPTIONS (named, as in T424): every direct transfer is a decodable `call rel32`/`jmp rel32`, no
code is generated at run time, the guest's own indirect calls are not followed (a closure with an
unresolved indirect site is reported, not trusted), and a memory operand that names an address by
a base register is reported separately so a counter access through a pointer would show.
"""

from __future__ import annotations

import collections
from collections.abc import Mapping
from dataclasses import dataclass, field

import capstone
from capstone import x86 as cs_x86

from tools.tracegaps import addresstaking, saturate, threadreach
from tools.tracegaps.code import Code
from tools.tracegaps.reach import Closure, ReachGraph

GATE = 0x156840
FLAG = 0x4E7A94
#: The loading bar's running flag: the stop function 0x155550 returns at once when it is 0.
RUNNING = 0x74A9AC
#: Entry conditions the walk assumes, each pins one branch: [FLAG] == 0 (the gate is not passed yet,
#: `jne` not taken) and [RUNNING] != 0 (the bar is running, `je` not taken). The host checks both on
#: the owner at the call of the gate, and their only writers are owner-only code (claim 4).
ENTRY_CONDITIONS = {FLAG: "zero", RUNNING: "nonzero"}
COUNTER = 0x563918
GETTER = 0x22030
CALLBACK = 0x22020
WAIT_WRAPPER = 0x3800BF
READERS = (0x1538C0, 0x156CB0)
#: Thread starts whose direct closure must not contain a writer of the flag.
OTHER_THREAD_STARTS = {"worker": 0x156CB0, "loader": 0x30160, "network poll": 0x3C0FE0}
LOADING_BAR_START = 0x156D80
STOP_FUNCTION = 0x155550

ASSUMPTIONS = (
    "every direct call and jmp is a decoded `call rel32`/`jmp rel32` (a call hidden in bytes the sweep cannot decode would be missed)",
    "no code is generated or loaded at run time, the image is the code",
    "indirect calls are not followed: a closure with an unresolved indirect site is reported and its claim is limited to direct edges",
    "no indirect transfer from another thread enters a caller of the stop function 0x155550 (T599 MEASURED the entry barrier over those callers OPEN, T603 narrowed it with jump table rules and per start saturated closures but no start closes: unresolved indirect sites remain in every one, see item 4, so this stays an assumption and the verdict is not proven). It is BACKED BY A RUNTIME FALSIFIER (T604): while the owner is in the gate or the worker's blanks run, a call of 0x155550, 0x156D80, 0x156D10 (the start body) or the gate 0x156840 by a thread other than the owner is a named host stop (a tail jump is invisible to it, the only direct jumps into the entries are the gate's and 0x156D80's, both named, and nothing jumps to the stores at 0x15556B and 0x156D31), so a wrong isolation fails by name instead of silently",
    "T603 rules add: an address is named only by an imm32, a disp32 or a stored dword, never computed from a base and an offset (a run of code addresses nothing names is no pointer table, a closed jump table is read only by its bounded `jmp [reg*4 + base]`), a global function pointer slot is written only through operands that name it, a kernel import slot only by the loader, and exceptions or asynchronous callbacks the thread does not register itself are not modelled (the callback services NtReadFile and NtWriteFile must pass a literal null routine)",
    "the owner reads [0x4E7A94] once at entry, so a write after that read cannot change the path (it is stored by the owner itself)",
)


@dataclass
class Region:
    """What the owner can execute from the gate's entry up to its first call."""

    entry: int
    instructions: set[int] = field(default_factory=set)
    calls: set[int] = field(default_factory=set)
    indirect: list[int] = field(default_factory=list)
    returns: list[int] = field(default_factory=list)
    counter_touches: list[int] = field(default_factory=list)
    computed_operands: list[int] = field(default_factory=list)
    absolute_operands: set[int] = field(default_factory=set)
    flag_writes: list[int] = field(default_factory=list)
    pruned: list[int] = field(default_factory=list)
    unknown: list[int] = field(default_factory=list)


@dataclass(frozen=True)
class StartVerdict:
    """The saturated closure of one thread start (T603 part c): does it hold a running flag writer?"""

    name: str
    start: int
    functions: int
    #: indirect sites resolved by rule (`saturate.RULE_*`)
    resolved: dict[str, int]
    #: sites no rule resolves, by `Site.kind`
    unresolved: dict[str, int]
    #: callback service calls (APC, DPC, timer) whose routine is not proven null
    callback_imports: int
    callback_nulls: int
    #: writers inside the saturated closure
    writers_reached: tuple[int, ...]

    @property
    def unresolved_sites(self) -> int:
        return sum(self.unresolved.values())

    @property
    def closed(self) -> bool:
        """Closed only when every site is resolved, no callback is unexplained and no writer is in."""
        return (
            self.functions > 0
            and not self.unresolved_sites
            and not self.callback_imports
            and not self.writers_reached
        )


@dataclass
class Report:
    region: Region
    wait_reaches_getter: bool
    wait_unresolved: int
    wait_functions: int
    counter_refs: list[tuple[int, str]]
    getter_callers: list[int]
    flag_refs: list[tuple[int, str, int | None]]
    flag_writer_functions: dict[int, list[int]]
    thread_overlap: dict[str, list[int]]
    thread_unresolved: dict[str, int]
    thread_unexplained: dict[str, list[int]]
    #: The writers of the running flag (the stop function and the loading bar start), the T424
    #: entry barrier over their callers, and its measured seams (T599).
    running_writer_functions: tuple[int, ...]
    running_members: int
    running_unattributed: list[int]
    #: address-taking occurrences of the members that are not a CreateThread start, by hit kind
    running_unexplained: dict[str, int]
    running_taken_functions: int
    #: Rule (a) and (b) of T603 over those occurrences (`addresstaking.classify`): occurrences
    #: excluded by rule, the residue by role, and the distinct taken addresses left.
    running_total_occurrences: int = 0
    running_explained: dict[str, int] = field(default_factory=dict)
    running_roles: dict[str, int] = field(default_factory=dict)
    running_excluded_roles: dict[str, int] = field(default_factory=dict)
    running_taken_entries: int = 0
    running_taken_interiors: int = 0
    #: Part (c): the saturated closure of each other thread's start.
    starts: dict[str, StartVerdict] = field(default_factory=dict)

    @property
    def region_closed(self) -> bool:
        region = self.region
        return (
            bool(region.instructions)
            and region.calls == {WAIT_WRAPPER}
            and not region.indirect
            and not region.returns
            and not region.counter_touches
            and not region.computed_operands
            and not region.unknown
            and len(region.pruned) == len(ENTRY_CONDITIONS)
        )

    @property
    def wrapper_closed(self) -> bool:
        return (
            not self.wait_reaches_getter and self.wait_unresolved == 0 and self.wait_functions > 0
        )

    @property
    def counter_sites_named(self) -> bool:
        return bool(self.counter_refs) and {role for _, role in self.counter_refs} == {
            "callback write",
            "getter read",
        }

    @property
    def flag_isolated(self) -> bool:
        return (
            bool(self.flag_writer_functions)
            and bool(self.running_writer_functions)
            and not any(self.thread_overlap.values())
            and not any(self.thread_unexplained.values())
            and self.running_barred
        )

    @property
    def starts_closed(self) -> bool:
        """Every other thread's saturated closure is closed (T603 part c): resolved, writer free."""
        return bool(self.starts) and all(verdict.closed for verdict in self.starts.values())

    @property
    def running_barred(self) -> bool:
        """The running flag's writers are owner-only. Either the T424 entry barrier holds (every member
        of the callers' closure is attributed and no address-taking of one is left unexplained after
        T603 rules a and b) or every other thread's saturated closure is closed (part c)."""
        return (
            bool(self.running_writer_functions)
            and self.running_members > 0
            and not self.running_unattributed
            and (not self.running_unexplained or self.starts_closed)
        )

    @property
    def proven(self) -> bool:
        return (
            self.region_closed
            and self.wrapper_closed
            and self.counter_sites_named
            and self.flag_isolated
        )


def _memory_operands(insn: capstone.CsInsn) -> list[cs_x86.X86Op]:
    return [op for op in insn.operands if op.type == cs_x86.X86_OP_MEM]


def _tested_address(insn: capstone.CsInsn, loaded: dict[int, int]) -> int | None:
    """The address a compare against zero tests: `test reg, reg` or `cmp reg, 0` on a register
    just loaded from a condition address, or `cmp [address], 0` / `cmp [address], imm 0`."""
    if insn.mnemonic == "test" and len(insn.operands) == 2:
        first, second = insn.operands
        if (
            first.type == cs_x86.X86_OP_REG
            and second.type == cs_x86.X86_OP_REG
            and first.reg == second.reg
        ):
            return loaded.get(first.reg)
        return None
    if insn.mnemonic != "cmp" or len(insn.operands) != 2:
        return None
    left, right = insn.operands
    if not (right.type == cs_x86.X86_OP_IMM and right.imm == 0):
        return None
    if left.type == cs_x86.X86_OP_REG:
        return loaded.get(left.reg)
    if left.type == cs_x86.X86_OP_MEM and left.mem.base == 0 and left.mem.index == 0:
        address = left.mem.disp & 0xFFFFFFFF
        return address if address in ENTRY_CONDITIONS else None
    return None


def _taken_branch_is_excluded(mnemonic: str, condition: str) -> bool:
    """The `jcc` that the entry condition makes never taken: `jne` when the value is zero,
    `je` when it is not."""
    if condition == "zero":
        return mnemonic in ("jne", "jnz")
    return mnemonic in ("je", "jz")


def walk(code: Code, entry: int = GATE) -> Region:
    """Flood the owner's path from `entry` under ENTRY_CONDITIONS, stopping at `call` and `ret`.

    The state carried per path is which registers hold a condition address and which address the
    last compare tested, enough to prune the one branch each condition makes impossible.
    """
    region = Region(entry=entry)
    State = tuple[int, frozenset[tuple[int, int]], int | None]
    seen: set[State] = set()
    stack: list[State] = [(entry, frozenset(), None)]
    while stack:
        address, loaded_set, testing = stack.pop()
        if (address, loaded_set, testing) in seen:
            continue
        seen.add((address, loaded_set, testing))
        insn = code.insn_at(address)
        if insn is None:
            region.unknown.append(address)
            continue
        region.instructions.add(address)
        loaded = dict(loaded_set)
        for op in _memory_operands(insn):
            if op.mem.base == 0 and op.mem.index == 0:
                disp = op.mem.disp & 0xFFFFFFFF
                region.absolute_operands.add(disp)
                if disp == COUNTER:
                    region.counter_touches.append(address)
                if disp in ENTRY_CONDITIONS and op.access & capstone.CS_AC_WRITE:
                    region.flag_writes.append(address)
            else:
                region.computed_operands.append(address)
        for op in insn.operands:
            if op.type == cs_x86.X86_OP_IMM and (op.imm & 0xFFFFFFFF) == COUNTER:
                region.counter_touches.append(address)
        mnemonic = insn.mnemonic
        follow = address + insn.size
        if mnemonic == "call":
            operand = insn.operands[0]
            if operand.type == cs_x86.X86_OP_IMM:
                region.calls.add(operand.imm & 0xFFFFFFFF)
            else:
                region.indirect.append(address)
            continue
        if mnemonic in ("ret", "retf", "iret"):
            region.returns.append(address)
            continue
        if mnemonic == "jmp":
            operand = insn.operands[0]
            if operand.type == cs_x86.X86_OP_IMM:
                stack.append((operand.imm & 0xFFFFFFFF, frozenset(), None))
            else:
                region.indirect.append(address)
            continue
        if capstone.CS_GRP_JUMP in insn.groups:
            operand = insn.operands[0]
            if operand.type != cs_x86.X86_OP_IMM:
                region.indirect.append(address)
                continue
            if testing is not None and _taken_branch_is_excluded(
                mnemonic, ENTRY_CONDITIONS[testing]
            ):
                region.pruned.append(address)
                stack.append((follow, frozenset(), None))
                continue
            stack.append((operand.imm & 0xFFFFFFFF, frozenset(), None))
            stack.append((follow, frozenset(), None))
            continue
        # straight-line instruction: track which register holds a condition address
        tested = _tested_address(insn, loaded)
        next_loaded = dict(loaded)
        if tested is None:
            if (
                mnemonic == "mov"
                and len(insn.operands) == 2
                and insn.operands[0].type == cs_x86.X86_OP_REG
            ):
                source = insn.operands[1]
                register = insn.operands[0].reg
                next_loaded.pop(register, None)
                if (
                    source.type == cs_x86.X86_OP_MEM
                    and source.mem.base == 0
                    and source.mem.index == 0
                    and (source.mem.disp & 0xFFFFFFFF) in ENTRY_CONDITIONS
                ):
                    next_loaded[register] = source.mem.disp & 0xFFFFFFFF
            else:
                for reg in insn.regs_write:
                    next_loaded.pop(reg, None)
        stack.append((follow, frozenset(next_loaded.items()), tested))
    region.counter_touches = sorted(set(region.counter_touches))
    region.computed_operands = sorted(set(region.computed_operands))
    region.pruned = sorted(set(region.pruned))
    return region


def new_graph(code: Code) -> ReachGraph:
    """The reach graph the analysis starts from, with the entries the proof names."""
    return ReachGraph(
        code, known_entries=(GATE, STOP_FUNCTION, WAIT_WRAPPER, *OTHER_THREAD_STARTS.values())
    )


def register_thread_starts(graph: ReachGraph) -> list[threadreach.ThreadSite]:
    """Add the CreateThread start routines to the graph's known entries and return the sites.

    Called after the owner index is built (`analyse` reads `real_owners` first), as it always was:
    the index is cached, so entries added here do not reshape it.
    """
    graph.known_entries = graph.known_entries | {
        threadreach.XBE_ENTRY_FALLBACK,
        threadreach.TRAMPOLINE,
    }
    sites = threadreach.thread_sites(graph)
    starts = {
        next(iter(site.start.values)) for site in sites if site.start.bounded and site.start.values
    }
    graph.known_entries = graph.known_entries | frozenset(starts)
    return threadreach.thread_sites(graph)


def thread_overlap(
    closures: Mapping[str, Closure], *barriers: threadreach.Barrier
) -> dict[str, list[int]]:
    """Per thread, the functions of its direct closure inside the callers' closure of any barrier."""
    members: set[int] = set()
    for item in barriers:
        members |= set(item.backward.members)
    return {name: sorted(set(closure.parent) & members) for name, closure in closures.items()}


def verdict_of(
    name: str, start: int, saturated: saturate.Saturated, writers: tuple[int, ...]
) -> StartVerdict:
    """What the saturated closure of `start` says about the running flag `writers`."""
    return StartVerdict(
        name=name,
        start=start,
        functions=len(saturated.closure.parent),
        resolved=dict(sorted(saturated.by_rule().items())),
        unresolved=dict(
            sorted(collections.Counter(site.kind for site in saturated.unresolved).items())
        ),
        callback_imports=len(saturated.callback_imports),
        callback_nulls=len(saturated.callback_nulls),
        writers_reached=tuple(saturated.reaches(writers)),
    )


def analyse(code: Code) -> Report:
    graph = new_graph(code)
    region = walk(code)
    wait = graph.closure([WAIT_WRAPPER])
    counter_refs: list[tuple[int, str]] = []
    for insn in code.absolute_operand_refs(COUNTER):
        role = "?"
        for op in insn.operands:
            if op.type == cs_x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                role = "callback write" if op.access & capstone.CS_AC_WRITE else "getter read"
        counter_refs.append((insn.address, role))
    getter_callers = sorted(t.address for t in code.callers(GETTER))
    flag_refs: list[tuple[int, str, int | None]] = []
    writer_functions: dict[int, list[int]] = {}
    running_writers: set[int] = set()
    for condition in ENTRY_CONDITIONS:
        for insn in code.absolute_operand_refs(condition):
            writes = any(
                op.type == cs_x86.X86_OP_MEM and op.access & capstone.CS_AC_WRITE
                for op in insn.operands
            )
            owners = graph.real_owners(insn.address)
            owner = owners[0] if owners else None
            flag_refs.append(
                (insn.address, f"{condition:#x} {'write' if writes else 'read'}", owner)
            )
            if writes and condition == FLAG:
                writer_functions.setdefault(owner if owner is not None else 0, []).append(
                    insn.address
                )
            elif writes and owner is not None:
                running_writers.add(owner)
    # T424's entry barrier for each flag's writers: every function that reaches a writer by direct
    # edges (the backward closure), every address-taking occurrence of those bodies explained as a
    # CreateThread start routine, and no other thread's direct closure inside it. T599 made the scan
    # one pass over the image (`ReachGraph.address_hits`), which is what lets the running flag's
    # barrier (the title's call tree over the stop function 0x155550) finish in seconds.
    sites = register_thread_starts(graph)
    unresolved: dict[str, int] = {}
    closures = {name: graph.closure([start]) for name, start in OTHER_THREAD_STARTS.items()}
    # One barrier per flag for all three threads: the callers' closure and its address-taking hits do
    # not depend on the thread, only the overlap does.
    first = next(iter(closures.values()))
    gate = threadreach.barrier(graph, tuple(sorted(writer_functions)), first, sites)
    running = threadreach.barrier(graph, tuple(sorted(running_writers)), first, sites)
    overlap = thread_overlap(closures, gate, running)
    for name, closure in closures.items():
        unresolved[name] = len(closure.unresolved)
    unexplained = sorted(
        {address for address, _ in gate.unexplained} | set(gate.backward.unattributed)
    )
    unexplained_by_thread = dict.fromkeys(OTHER_THREAD_STARTS, unexplained)
    classification = addresstaking.classify(graph, running.backward.members, running.unexplained)
    running_unexplained = collections.Counter(hit.kind for _, hit in classification.residue)
    writers = tuple(sorted(running_writers))
    resolver = saturate.SiteResolver(graph)
    claims = {"network poll": threadreach.poll_claims(graph)}
    starts: dict[str, StartVerdict] = {}
    for name, start in OTHER_THREAD_STARTS.items():
        saturated = saturate.saturate(graph, start, resolver, claims.get(name))
        starts[name] = verdict_of(name, start, saturated, writers)
    return Report(
        region=region,
        wait_reaches_getter=GETTER in wait.parent or any(r in wait.parent for r in READERS),
        wait_unresolved=len(wait.unresolved),
        wait_functions=len(wait.parent),
        counter_refs=sorted(counter_refs),
        getter_callers=getter_callers,
        flag_refs=sorted(flag_refs),
        flag_writer_functions=writer_functions,
        thread_overlap=overlap,
        thread_unresolved=unresolved,
        thread_unexplained=unexplained_by_thread,
        running_writer_functions=writers,
        running_members=len(running.backward.members),
        running_unattributed=running.backward.unattributed,
        running_unexplained=dict(sorted(running_unexplained.items())),
        running_taken_functions=len(classification.entries) + len(classification.interiors),
        running_total_occurrences=classification.total,
        running_explained=classification.explained,
        running_roles=classification.roles,
        running_excluded_roles=classification.excluded_roles,
        running_taken_entries=len(classification.entries),
        running_taken_interiors=len(classification.interiors),
        starts=starts,
    )


def render(report: Report) -> str:
    region = report.region
    lines = [
        f"T592 loading bar gate region proof (entry {GATE:#x}, flag {FLAG:#x} == 0, counter {COUNTER:#x})",
        "",
        "1. the owner's path from the gate entry to its first call",
        f"   instructions reached: {len(region.instructions)}, entry-condition branches pruned: {[hex(a) for a in region.pruned]} (conditions {[(hex(a), c) for a, c in ENTRY_CONDITIONS.items()]})",
        f"   calls: {[hex(c) for c in sorted(region.calls)]} (expected only {WAIT_WRAPPER:#x})",
        f"   indirect transfers {len(region.indirect)}, returns {len(region.returns)}, computed memory operands {len(region.computed_operands)}, undecodable {len(region.unknown)}",
        f"   instructions touching {COUNTER:#x}: {[hex(a) for a in region.counter_touches]}",
        f"   absolute memory operands: {[hex(a) for a in sorted(region.absolute_operands)]}",
        f"   closed: {report.region_closed}",
        "",
        f"2. the wait wrapper {WAIT_WRAPPER:#x}: {report.wait_functions} functions in its direct closure, reaches a reader: {report.wait_reaches_getter}, unresolved indirect sites {report.wait_unresolved}, closed: {report.wrapper_closed}",
        "",
        f"3. the counter {COUNTER:#x}, instructions naming it: {[(hex(a), role) for a, role in report.counter_refs]}",
        f"   direct callers of the getter {GETTER:#x}: {[hex(a) for a in report.getter_callers]}",
        f"   only a callback write and a getter read: {report.counter_sites_named}",
        "",
        f"4. the entry conditions {[hex(a) for a in ENTRY_CONDITIONS]}: {[(hex(a), kind, hex(o) if o else None) for a, kind, o in report.flag_refs]}",
        f"   writer functions: {[hex(f) for f in sorted(report.flag_writer_functions)]}",
    ]
    for name, start in OTHER_THREAD_STARTS.items():
        lines.append(
            f"   {name} start {start:#x}: direct closure inside the callers of a writer: {[hex(f) for f in report.thread_overlap[name]]}, unexplained address-taking {[hex(f) for f in report.thread_unexplained[name]]} (its own direct closure has {report.thread_unresolved[name]} unresolved indirect sites)"
        )
    lines += [
        f"   running flag {RUNNING:#x}: writer functions {[hex(f) for f in report.running_writer_functions]}, entry barrier over their callers: {report.running_members} functions, unattributed callers {[hex(a) for a in report.running_unattributed]}, barred: {report.running_barred}",
        f"     address-taking occurrences no CreateThread start explains: {report.running_total_occurrences}",
        f"     excluded by the jump table rules (T603 a): {sum(report.running_explained.values())} {report.running_explained}",
        f"     residue by role: {report.running_roles}, excluded by role (T603 b): {report.running_excluded_roles}",
        f"     left unexplained: {report.running_unexplained} at {report.running_taken_entries} function entries and {report.running_taken_interiors} interior addresses",
        "   saturated closures of the other threads (T603 c: every site a rule resolves is added, the writers are the running flag's):",
        *(
            f"     {name} start {verdict.start:#x}: {verdict.functions} functions, resolved {verdict.resolved}, unresolved {verdict.unresolved_sites} {verdict.unresolved}, callback services unexplained {verdict.callback_imports} (null routine {verdict.callback_nulls}), writers reached {[hex(w) for w in verdict.writers_reached]}, closed: {verdict.closed}"
            for name, verdict in report.starts.items()
        ),
        f"   conditions isolated to the owner: {report.flag_isolated}",
        "",
        f"PROVEN: {report.proven}",
        "",
        "assumptions:",
        *(f"  - {item}" for item in ASSUMPTIONS),
    ]
    return "\n".join(lines)


def to_json(report: Report) -> dict[str, object]:
    region = report.region
    return {
        "proven": report.proven,
        "region": {
            "instructions": len(region.instructions),
            "calls": sorted(region.calls),
            "indirect": region.indirect,
            "returns": region.returns,
            "counter_touches": region.counter_touches,
            "computed_operands": region.computed_operands,
            "pruned": region.pruned,
        },
        "wait_wrapper": {
            "functions": report.wait_functions,
            "reaches_getter": report.wait_reaches_getter,
            "unresolved": report.wait_unresolved,
        },
        "counter_refs": [[a, role] for a, role in report.counter_refs],
        "getter_callers": report.getter_callers,
        "flag_writer_functions": {hex(k): v for k, v in report.flag_writer_functions.items()},
        "thread_overlap": report.thread_overlap,
        "thread_unresolved": report.thread_unresolved,
        "thread_unexplained": {
            k: [hex(a) for a in v] for k, v in report.thread_unexplained.items()
        },
        "running_flag": {
            "writer_functions": [hex(f) for f in report.running_writer_functions],
            "members": report.running_members,
            "unattributed": [hex(a) for a in report.running_unattributed],
            "unexplained": report.running_unexplained,
            "taken_functions": report.running_taken_functions,
            "occurrences": report.running_total_occurrences,
            "explained": report.running_explained,
            "roles": report.running_roles,
            "excluded_roles": report.running_excluded_roles,
            "taken_entries": report.running_taken_entries,
            "taken_interiors": report.running_taken_interiors,
            "barred": report.running_barred,
        },
        "saturated_starts": {
            name: {
                "start": hex(verdict.start),
                "functions": verdict.functions,
                "resolved": verdict.resolved,
                "unresolved": verdict.unresolved,
                "callback_imports": verdict.callback_imports,
                "callback_nulls": verdict.callback_nulls,
                "writers_reached": [hex(w) for w in verdict.writers_reached],
                "closed": verdict.closed,
            }
            for name, verdict in report.starts.items()
        },
        "flag_isolated": report.flag_isolated,
    }
