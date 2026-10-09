# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T424: can the thread started at 0x3C0FE0 reach the vblank counter getter, and what is it?
T593 generalised the entry point: `analyse(code, start=...)` and `--start 0x30160` run the same
entry barrier for any thread start routine the image creates (the loader thread 0x30160 closes it).

THE CLAIM BEING CLOSED. T371 found four `CreateThread` sites and could not exclude that the
thread created at 0x3C108A (start routine 0x3C0FE0) reaches the counter readers. It reported a
direct-call closure of 12671 functions with 719 indirect calls. That figure is an artefact of
`tools.vblank_probe.parse_call_graph` (the text slice for each function ran into the next
function's doc comment, so every function got an edge to its lexical successor and the chain
0x3C0FE0 -> 0x3800D1 -> 0x3800DF -> 0x38014B -> 0x3801D9 (the CRT start, which calls `main`)
pulled in the whole title). Read from the retail image the direct closure of 0x3C0FE0 is six
functions and one unresolved indirect call.

THE ARGUMENT (an entry barrier, so no indirect call has to be resolved one by one).
Let B be every function that reaches the counter getter 0x22030 through DIRECT calls and tail
jumps (the backward closure, `ReachGraph.callers_closure`). A thread that is not in B can
only ENTER B through a transfer that is not a direct call, that is an indirect call or jump, a
kernel-made entry (thread start, DPC, APC, timer, interrupt, exception handler) or a return.
Each of those needs the entry's address to exist as a value: an `imm32` that takes it, a stored
pointer, or an operand. So take every occurrence of every address in the bodies of B in the
image (`ReachGraph.address_hits`). If the only address-taking occurrences are the start routine
pointers passed to `CreateThread`, and those reach only the kernel and the thread trampoline,
the thread cannot enter B and cannot execute the getter. The same is done for the functions that
CREATE the reader threads (B2), which shows the thread cannot start a reader either.

WHAT IS MEASURED AND WHAT IS ASSUMED are separate lists in the report. The assumptions are the
standard ones for a static reachability claim about a retail image and are named, not hidden.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import capstone

from tools.tracegaps.code import Code
from tools.tracegaps.flow import ValueSet
from tools.tracegaps.reach import TAKING_KINDS, Backward, Closure, Hit, ReachGraph

#: Title CreateThread wrapper (stdcall, six arguments) and the kernel service it calls.
WRAPPER = 0x37FEB5
THREAD_SLOT = 0x475894
THREAD_KERNEL_NAME = "PsCreateSystemThreadEx"
#: Thread trampoline the wrapper hands to the kernel as the SystemRoutine, and its one call
#: through the start routine pointer.
TRAMPOLINE = 0x37FE1D
TRAMPOLINE_CALL = 0x37FE8C
#: Title wrapper over KeSetBasePriorityThread (handle, priority) and the one over NtClose.
PRIORITY_WRAPPER = 0x37FC24
CLOSE_WRAPPER = 0x37CBC4
#: The counter getter (`mov eax, [0x563918]; ret`) and its two direct callers.
GETTER = 0x22030
READERS = (0x1538C0, 0x156CB0)
#: The XBE entry point is not a call target, it is entered by the kernel.
XBE_ENTRY_FALLBACK = 0x38024D
#: The thread under study and the registry that drives its loop.
START = 0x3C0FE0
#: Title wrapper over NtWaitForSingleObjectEx (handle, milliseconds, alertable), the loader
#: thread's 16 ms wait (T586). Its literal arguments are read for any start routine.
WAIT_WRAPPER = 0x380029
#: Words a thread parked or running beside the delivery must not be able to touch: the counter
#: the getter reads and the T587 state words of the worker and the owner loops.
GUARDED_WORDS = {
    0x563918: "vblank counter",
    0x7497D8: "worker last seen counter",
    0x7497A8: "worker frame counter",
    0x74A9B0: "worker stop flag",
    0x74A9AC: "worker running flag",
    0x7A58B0: "worker frame word",
}
POLL_FUNCTION = 0x3C0F00
SLEEP_WRAPPER = 0x3800D1
REGISTRAR = 0x3C0E30
#: Callback table the poll function walks (8 bytes per entry: routine, argument), and the
#: function that starts the thread.
CALLBACK_TABLE = (0x772398, 0x7724A0)
CREATOR_FUNCTION = 0x3C1020
TIMER_WALKER = 0x3BC4B0
SOCKET_OPTION_SETTER = 0x3BCD20
#: Instruction addresses the sink check expects (read from the image, asserted by the tests).
WRAPPER_KERNEL_CALL = 0x37FEEE
TRAMPOLINE_TAKEN_AT = 0x37FEC7
#: Where the wrapper reads the default stack size from when the argument is 0.
STACK_COMMIT_SLOT = 0x10130
WRAPPER_ARGUMENTS = (
    "lpThreadAttributes",
    "dwStackSize",
    "lpStartAddress",
    "lpParameter",
    "dwCreationFlags",
    "lpThreadId",
)

ASSUMPTIONS = (
    "every direct call and jmp is a decoded `call rel32`/`jmp rel32` (a call hidden in bytes the sweep cannot decode would be missed)",
    "a code address is taken as an imm32, a stored dword or an instruction operand, never computed from a base and an offset",
    "the `.XTLID` section (library identification for tools) is not called through at run time",
    "no code is loaded or generated at run time (XeLoadSection, self-modifying code), the image is the code",
    "a thread's entry is the start routine its creator passed, and the kernel runs it only through the trampoline",
)


@dataclass(frozen=True)
class ThreadSite:
    """One call of the CreateThread wrapper with its six arguments as value sets."""

    call: int
    function: int
    arguments: tuple[ValueSet, ...]

    @property
    def start(self) -> ValueSet:
        return self.arguments[2]


@dataclass
class Barrier:
    """One entry-barrier computation: the callers' closure of `targets` and its seams."""

    targets: tuple[int, ...]
    backward: Backward
    #: functions of the backward closure that the thread reaches through direct edges
    direct_overlap: list[int]
    #: address -> address-taking hits (see `ReachGraph.address_hits`), entries and interiors
    taking: dict[int, list[Hit]]
    #: address-taking `immediate` hits that are NOT a start routine passed to the wrapper
    unexplained: list[tuple[int, Hit]]
    #: wrapper start routine -> the push that takes it
    sinks: dict[int, int]
    #: coincidences dismissed, by kind, so the dismissal is visible
    dismissed: dict[str, int]

    @property
    def barred(self) -> bool:
        return not self.direct_overlap and not self.unexplained and not self.backward.unattributed


@dataclass
class Report:
    start: int
    sites: list[ThreadSite]
    creator: int
    creator_site: ThreadSite
    stack_commit: int | None
    priority_argument: ValueSet | None
    direct: Closure
    getter: Barrier
    spawner: Barrier
    wrapper_callers: list[int]
    wrapper_tail_jumps: list[int]
    wrapper_pointer_hits: list[Hit]
    kernel_slot_refs: list[int]
    trampoline_taking: list[Hit]
    trampoline_transfers: list[int]
    trampoline_indirect_sites: list[int]
    start_taking: list[Hit]
    registrations: list[tuple[int, ValueSet]]
    table_references: list[int]
    timer_callbacks: list[tuple[int, ValueSet]]
    partial: Closure | None
    creation: Backward | None
    creation_taken: dict[int, list[Hit]]
    sleep_milliseconds: int | None
    #: calls of the NtWaitForSingleObjectEx wrapper inside the start routine's own body:
    #: (call address, handle, milliseconds, alertable) as value sets
    waits: list[tuple[int, ValueSet, ValueSet, ValueSet]]
    #: guarded word -> instructions of the direct closure that carry it as a displacement
    guarded_refs: dict[int, list[int]]

    owners: dict[int, list[int]] = field(default_factory=dict)

    def owners_of(self, address: int) -> list[int]:
        return self.owners.get(address, [])

    @property
    def verdict(self) -> str:
        if self.getter.barred and self.spawner.barred and self.sinks_ok:
            return "provably unreachable"
        return "NOT DERIVABLE"

    @property
    def sinks_ok(self) -> bool:
        return (
            self.wrapper_callers == sorted(site.call for site in self.sites)
            and not self.wrapper_tail_jumps
            and not [hit for hit in self.wrapper_pointer_hits if hit.kind in ("data", "table")]
            and self.kernel_slot_refs == [WRAPPER_KERNEL_CALL]
            and [hit.where for hit in self.trampoline_taking] == [TRAMPOLINE_TAKEN_AT]
            and not self.trampoline_transfers
            and self.trampoline_indirect_sites == [TRAMPOLINE_CALL]
        )


def _sleep_milliseconds(graph: ReachGraph, start: int = START) -> int | None:
    """The `push imm` right before the thread's call of the sleep wrapper."""
    code = graph.code
    for address in sorted(graph.body(start)):
        insn = code.insn_at(address)
        if insn is None or insn.mnemonic != "call" or insn.op_str != hex(SLEEP_WRAPPER):
            continue
        before = code.previous_insn(insn)
        if before is not None and before.mnemonic == "push" and before.op_str.startswith("0x"):
            return int(before.op_str, 16)
    return None


def wait_calls(graph: ReachGraph, start: int) -> list[tuple[int, ValueSet, ValueSet, ValueSet]]:
    """Every call of the NtWaitForSingleObjectEx wrapper in the body of `start` with its handle,
    milliseconds and alertable arguments (T593: the loader thread's 16 ms wait)."""
    found: list[tuple[int, ValueSet, ValueSet, ValueSet]] = []
    for address in sorted(graph.body(start)):
        insn = graph.code.insn_at(address)
        if insn is None or insn.mnemonic != "call" or insn.op_str != hex(WAIT_WRAPPER):
            continue
        handle, milliseconds, alertable = graph.call_arguments(address, 3)
        found.append((address, handle, milliseconds, alertable))
    return found


def guarded_references(graph: ReachGraph, direct: Closure) -> dict[int, list[int]]:
    """Instructions in the bodies of the direct closure whose memory operand displacement is a
    guarded word. A DIRECT reference only: an access through a computed pointer is not seen."""
    addresses: set[int] = set()
    for member in direct.parent:
        addresses |= graph.body(member)
    found: dict[int, list[int]] = {word: [] for word in GUARDED_WORDS}
    for insn in graph.code.insns:
        if insn.address not in addresses:
            continue
        for operand in insn.operands:
            disp = operand.mem.disp & 0xFFFFFFFF
            if operand.type == capstone.x86.X86_OP_MEM and disp in found:
                found[disp].append(insn.address)
    return found


def thread_sites(graph: ReachGraph) -> list[ThreadSite]:
    """Every direct call of the CreateThread wrapper with its six pushed arguments."""
    sites: list[ThreadSite] = []
    for transfer in graph.code.callers(WRAPPER):
        owners = graph.enclosing_entries(transfer.address)
        values = graph.call_arguments(transfer.address, len(WRAPPER_ARGUMENTS))
        sites.append(ThreadSite(transfer.address, max(owners) if owners else 0, tuple(values)))
    return sorted(sites, key=lambda site: site.call)


def _straight_line_push(code: Code, taken_at: int, call: int) -> bool:
    """True when `taken_at` is a push and the wrapper call at `call` follows it in a straight line
    of at most a dozen instructions with no call and no branch between them."""
    insn = code.insn_at(taken_at)
    if insn is None or insn.mnemonic != "push":
        return False
    for _ in range(12):
        insn = code.next_insn(insn)
        if insn is None:
            return False
        if insn.address == call:
            return True
        if insn.mnemonic == "call" or capstone.CS_GRP_JUMP in insn.groups:
            return False
    return False


def barrier(
    graph: ReachGraph,
    targets: tuple[int, ...],
    direct: Closure,
    sites: list[ThreadSite],
) -> Barrier:
    """Backward closure of `targets`, every address-taking occurrence of its bodies, explained."""
    backward = graph.callers_closure(targets)
    addresses: set[int] = set()
    for member in backward.members:
        addresses |= graph.body(member)
    addresses |= set(backward.members)
    raw = graph.address_hits(sorted(addresses))
    taking = {
        address: found for address, hits in raw.items() if (found := graph.address_taking(hits))
    }
    dismissed: dict[str, int] = {}
    for hits in raw.values():
        for hit in hits:
            if hit.kind not in TAKING_KINDS:
                dismissed[hit.kind] = dismissed.get(hit.kind, 0) + 1
    sinks: dict[int, int] = {}
    unexplained: list[tuple[int, Hit]] = []
    for address, hits in sorted(taking.items()):
        for hit in hits:
            site = next(
                (
                    candidate
                    for candidate in sites
                    if candidate.start.values == frozenset({address})
                    and candidate.start.bounded
                    and hit.kind == "immediate"
                    and _straight_line_push(graph.code, hit.where, candidate.call)
                ),
                None,
            )
            if site is None or address not in backward.members:
                unexplained.append((address, hit))
            else:
                sinks[address] = hit.where
    overlap = sorted(set(direct.parent) & set(backward.members))
    return Barrier(targets, backward, overlap, taking, unexplained, sinks, dismissed)


def poll_resolutions(
    registrations: list[tuple[int, ValueSet]], timer_callbacks: list[tuple[int, ValueSet]]
) -> dict[int, list[int]]:
    """The two indirect sites of the poll thread the T424 registrations resolve: the callback table
    walk `0x3C0F67` (literal argument of the registrar) and the socket timer `0x3BC4FC`."""
    resolutions: dict[int, list[int]] = {}
    for _site_address, value in registrations:
        if value.bounded:
            resolutions[0x3C0F67] = sorted(value.values)
    for _site_address, value in timer_callbacks:
        if value.bounded:
            resolutions[0x3BC4FC] = sorted(value.values)
    return resolutions


def poll_claims(graph: ReachGraph) -> dict[int, list[int]]:
    """`poll_resolutions` read from the image, the claims `saturate` starts the poll thread with."""
    return poll_resolutions(
        graph.literal_arguments(REGISTRAR, 1), graph.literal_arguments(SOCKET_OPTION_SETTER, 5)
    )


def analyse(code: Code, graph: ReachGraph | None = None, start: int = START) -> Report:
    """Run the T424 entry barrier study on a decoded image for the thread started at `start`.

    The what-is-it corroboration of the network polling thread (callback table, registrations,
    creator chain) is specific to START and is only produced for it.
    """
    entries = [XBE_ENTRY_FALLBACK, START, TRAMPOLINE, start]
    sites_seed = ReachGraph(code, known_entries=entries)
    graph = graph or sites_seed
    graph.known_entries = graph.known_entries | set(entries)
    sites = thread_sites(graph)
    starts = {
        next(iter(site.start.values)) for site in sites if site.start.bounded and site.start.values
    }
    graph.known_entries = graph.known_entries | frozenset(starts)
    sites = thread_sites(graph)
    creator_site = next((site for site in sites if site.start.values == frozenset({start})), None)
    if creator_site is None:
        raise ValueError(f"{start:#x} is not a start routine of any CreateThread wrapper call")
    direct = graph.closure([start])
    getter = barrier(graph, (GETTER,), direct, sites)
    reader_starts = starts & set(getter.backward.members)
    creators = tuple(
        sorted(
            {
                owner
                for site in sites
                if site.start.values and site.start.values <= reader_starts
                for owner in graph.enclosing_entries(site.call)
            }
        )
    )
    spawner = barrier(graph, creators, direct, sites)
    header = graph.header_u32(STACK_COMMIT_SLOT)
    registrations: list[tuple[int, ValueSet]] = []
    timer_callbacks: list[tuple[int, ValueSet]] = []
    partial: Closure | None = None
    table_refs: list[int] = []
    creation: Backward | None = None
    creation_taken: dict[int, list[Hit]] = {}
    priority: ValueSet | None = None
    if start == START:
        registrations = graph.literal_arguments(REGISTRAR, 1)
        timer_callbacks = graph.literal_arguments(SOCKET_OPTION_SETTER, 5)
        partial = graph.closure([START], poll_resolutions(registrations, timer_callbacks))
        table_refs = graph.memory_operand_refs(CALLBACK_TABLE[0], CALLBACK_TABLE[1])
        creation = graph.callers_closure([CREATOR_FUNCTION])
        creation_taken = {
            address: graph.address_taking(hits)
            for address, hits in graph.address_hits(sorted(creation.members)).items()
            if graph.address_taking(hits)
        }
        priority = graph.literal_arguments(0x3BC550, 1)[0][1]
    sleep = _sleep_milliseconds(graph, start)
    return Report(
        start=start,
        sites=sites,
        creator=creator_site.function,
        creator_site=creator_site,
        stack_commit=header,
        priority_argument=priority,
        direct=direct,
        getter=getter,
        spawner=spawner,
        wrapper_callers=sorted(t.address for t in code.callers(WRAPPER)),
        wrapper_tail_jumps=sorted(t.address for t in code.tail_jumps(WRAPPER)),
        wrapper_pointer_hits=graph.address_hits([WRAPPER]).get(WRAPPER, []),
        kernel_slot_refs=sorted(i.address for i in code.absolute_operand_refs(THREAD_SLOT)),
        trampoline_taking=graph.address_taking(
            graph.address_hits([TRAMPOLINE]).get(TRAMPOLINE, [])
        ),
        trampoline_transfers=sorted(t.address for t in code.transfers().get(TRAMPOLINE, [])),
        trampoline_indirect_sites=[
            site.address for site in graph.summary(TRAMPOLINE).sites if site.kind != "import"
        ],
        start_taking=graph.address_taking(graph.address_hits([start]).get(start, [])),
        registrations=registrations,
        table_references=table_refs,
        timer_callbacks=timer_callbacks,
        partial=partial,
        creation=creation,
        creation_taken=creation_taken,
        sleep_milliseconds=sleep,
        waits=wait_calls(graph, start),
        guarded_refs=guarded_references(graph, direct),
        owners={a: graph.enclosing_entries(a) for a in table_refs},
    )


def _hex(values: object) -> str:
    return ", ".join(f"{int(v):#x}" for v in values)  # type: ignore[attr-defined]


def render(report: Report) -> str:
    lines = [f"T424 thread {report.start:#x}: {report.verdict}"]
    lines.append("")
    lines.append("CreateThread wrapper call sites (stdcall, six arguments)")
    for site in report.sites:
        shown = ", ".join(value.describe() for value in site.arguments)
        lines.append(f"  call {site.call:#x} in {site.function:#x}: {shown}")
    site = report.creator_site
    lines.append("")
    lines.append(
        f"thread {report.start:#x}: created at {site.call:#x} (return address {site.call + 5:#x}) in {report.creator:#x}"
    )
    lines.append(
        f"  stack size argument 0 = default, the PE stack commit {report.stack_commit:#x} read from the image header"
        if report.stack_commit is not None
        else "  stack size argument 0 = default, header stack commit unreadable"
    )
    if report.priority_argument is not None:
        lines.append(
            f"  priority argument of the creator chain: {report.priority_argument.describe()} (KeSetBasePriorityThread)"
        )
        lines.append(
            "  start context 0, flags 0 (not suspended), handle closed right after the priority call"
        )
        lines.append(
            f"  loop: calls {POLL_FUNCTION:#x} then sleeps {report.sleep_milliseconds} ms while the run flag is set"
        )
    lines.append(
        f"  start routine address taken at: {[f'{h.kind}@{h.where:#x}({h.section})' for h in report.start_taking]}"
    )
    for call, handle, milliseconds, alertable in report.waits:
        lines.append(
            f"  wait call {call:#x} (NtWaitForSingleObjectEx wrapper {WAIT_WRAPPER:#x}): "
            f"handle {handle.describe()}, milliseconds {milliseconds.describe()}, "
            f"alertable {alertable.describe()}"
        )
    for word, name in GUARDED_WORDS.items():
        refs = report.guarded_refs.get(word, [])
        lines.append(
            f"  direct references to {word:#x} ({name}) in the closure: {_hex(refs) or 'none'}"
        )
    lines.append(f"  direct closure of the start routine: {len(report.direct.parent)} functions")
    lines.append(f"    {_hex(sorted(report.direct.parent))}")
    lines.append(f"  kernel imports in it: {dict(report.direct.kernel)}")
    lines.append(
        f"  indirect sites in the direct closure: {[(hex(s.address), s.operand) for s in report.direct.unresolved]}"
    )
    lines.append("")
    for label, barrier_ in (
        ("getter 0x22030", report.getter),
        ("reader thread creators", report.spawner),
    ):
        lines.append(f"entry barrier for {label}: {'BARRED' if barrier_.barred else 'OPEN'}")
        lines.append(
            f"  callers closure ({len(barrier_.backward.members)}): {_hex(sorted(barrier_.backward.members))}"
        )
        lines.append(
            f"  overlap with the direct closure: {_hex(barrier_.direct_overlap) or 'none'}"
        )
        lines.append(
            f"  caller instructions no function contains: {_hex(barrier_.backward.unattributed) or 'none'}"
        )
        for address, hits in sorted(barrier_.taking.items()):
            shown = ", ".join(f"{hit.kind}@{hit.where:#x}({hit.section})" for hit in hits)
            lines.append(f"  address taken {address:#x}: {shown}")
        lines.append(
            f"  explained by a wrapper start routine push: { {hex(k): hex(v) for k, v in barrier_.sinks.items()} }"
        )
        lines.append(
            f"  unexplained: {[(hex(a), h.kind) for a, h in barrier_.unexplained] or 'none'}"
        )
        lines.append(f"  coincidences dismissed: {barrier_.dismissed}")
    lines.append("")
    lines.append("start routine pointer sinks")
    lines.append(
        f"  wrapper direct callers {_hex(report.wrapper_callers)}, tail jumps {_hex(report.wrapper_tail_jumps) or 'none'}"
    )
    lines.append(
        f"  kernel slot {THREAD_KERNEL_NAME} referenced at {_hex(report.kernel_slot_refs)}"
    )
    lines.append(
        f"  trampoline {TRAMPOLINE:#x} address taken at {[hex(h.where) for h in report.trampoline_taking]}, called by {_hex(report.trampoline_transfers) or 'nobody'}, indirect call sites {_hex(report.trampoline_indirect_sites)}"
    )
    lines.append(f"  sinks consistent: {report.sinks_ok}")
    if report.partial is not None and report.creation is not None:
        lines.extend(_render_poll_detail(report, report.partial, report.creation))
    lines.append("")
    lines.append("ASSUMPTIONS")
    lines.extend(f"  - {item}" for item in ASSUMPTIONS)
    return "\n".join(lines)


def _render_poll_detail(report: Report, partial: Closure, creation: Backward) -> list[str]:
    """The T424 corroboration that only exists for the network polling thread."""
    lines: list[str] = [""]
    lines.append(
        "what the loop does (corroboration, partial forward closure with resolved registries)"
    )
    lines.append(
        f"  callback registrar {REGISTRAR:#x} callers: {[(hex(a), v.describe()) for a, v in report.registrations]}"
    )
    lines.append(
        f"  callback table {CALLBACK_TABLE[0]:#x}..{CALLBACK_TABLE[1]:#x} touched only by instructions in "
        f"{_hex(sorted({owner for address in report.table_references for owner in report.owners_of(address)}))}"
    )
    lines.append(
        f"  socket timeout callback setter {SOCKET_OPTION_SETTER:#x} argument 5: {[(hex(a), v.describe()) for a, v in report.timer_callbacks]}"
    )
    lines.append(
        f"  closure with those two sites resolved: {len(partial.parent)} functions, "
        f"{len(partial.unresolved)} indirect sites unresolved, getter reached: "
        f"{any(g in partial.parent for g in (GETTER, *READERS))}"
    )
    lines.append("")
    lines.append(
        f"who can create the thread (callers closure of {CREATOR_FUNCTION:#x}, {len(creation.members)} functions)"
    )
    lines.append(f"  {_hex(sorted(creation.members))}")
    lines.append(f"  reached from main 0x18e8d0 by direct calls: {0x18E8D0 in creation.members}")
    for address, hits in sorted(report.creation_taken.items()):
        lines.append(
            f"  address taken {address:#x}: {', '.join(f'{h.kind}@{h.where:#x}({h.section})' for h in hits)}"
        )
    return lines


def to_json(report: Report) -> dict[str, object]:
    def barrier_json(item: Barrier) -> dict[str, object]:
        return {
            "targets": [hex(t) for t in item.targets],
            "barred": item.barred,
            "members": [hex(m) for m in sorted(item.backward.members)],
            "direct_overlap": [hex(m) for m in item.direct_overlap],
            "unattributed": [hex(m) for m in item.backward.unattributed],
            "address_taken": {
                hex(a): [f"{h.kind}@{h.where:#x}" for h in hits] for a, hits in item.taking.items()
            },
            "sinks": {hex(a): hex(w) for a, w in item.sinks.items()},
            "unexplained": [hex(a) for a, _ in item.unexplained],
            "dismissed": item.dismissed,
        }

    return {
        "start": hex(report.start),
        "verdict": report.verdict,
        "sites": [
            {
                "call": hex(site.call),
                "function": hex(site.function),
                "start": site.start.describe(),
            }
            for site in report.sites
        ],
        "creator": hex(report.creator),
        "stack_commit": report.stack_commit,
        "priority": report.priority_argument.describe() if report.priority_argument else None,
        "start_address_taken": [f"{h.kind}@{h.where:#x}" for h in report.start_taking],
        "waits": [
            {
                "call": hex(call),
                "handle": handle.describe(),
                "milliseconds": milliseconds.describe(),
                "alertable": alertable.describe(),
            }
            for call, handle, milliseconds, alertable in report.waits
        ],
        "guarded_references": {hex(w): [hex(a) for a in r] for w, r in report.guarded_refs.items()},
        "indirect_sites": [[hex(s.address), s.operand] for s in report.direct.unresolved],
        "kernel_imports": dict(report.direct.kernel),
        "sleep_milliseconds": report.sleep_milliseconds,
        "direct_closure": [hex(f) for f in sorted(report.direct.parent)],
        "getter": barrier_json(report.getter),
        "reader_creators": barrier_json(report.spawner),
        "sinks_ok": report.sinks_ok,
        "partial_closure": {
            "functions": len(report.partial.parent),
            "unresolved": [hex(s.address) for s in report.partial.unresolved],
        }
        if report.partial is not None
        else None,
        "creation": {
            "members": [hex(m) for m in sorted(report.creation.members)],
            "address_taken": [hex(a) for a in sorted(report.creation_taken)],
        }
        if report.creation is not None
        else None,
        "assumptions": list(ASSUMPTIONS),
    }
